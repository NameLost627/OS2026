#include <pmm.h>
#include <list.h>
#include <buddy_system_pmm.h>

/* Buddy system physical memory manager.
 *
 * A block of order k is 2^k continuous pages aligned to 2^k pages, so the two
 * halves of an order-(k+1) block are buddies: block_index ^ (1 << k).  Only
 * the head page of a free block carries PG_property, and its `property' field
 * stores the order.  The other pages of the block carry no flag.
 *
 * alloc_pages() takes the smallest order that has a free block and splits it
 * down to the requested order; free_pages() inserts the block back and merges
 * it with its free buddy recursively.
 *
 * The manager is registered in this module but is not the default one.  To
 * use it, set
 *     pmm_manager = &buddy_system_pmm_manager;
 * in init_pmm_manager() (kern/mm/pmm.c), so that page_init() registers the
 * free memory with this manager before check_alloc_page() runs.
 */

#define MAX_ORDER 16                    // orders 0..15: blocks of 1..32768 pages

static free_area_t free_area[MAX_ORDER];

static inline size_t block_pages(int order) {
    return (size_t)1 << order;
}

// smallest order that can hold n continuous pages
static inline int pages_order(size_t n) {
    int order = 0;
    while (order < MAX_ORDER && block_pages(order) < n) {
        order ++;
    }
    return order;
}

static size_t buddy_system_nr_free_pages(void) {
    size_t total = 0;
    for (int order = 0; order < MAX_ORDER; order ++) {
        total += block_pages(order) * free_area[order].nr_free;
    }
    return total;
}

static void buddy_add_block(struct Page *base, int order) {
    base->property = order;
    SetPageProperty(base);
    list_add(&(free_area[order].free_list), &(base->page_link));
    free_area[order].nr_free ++;
}

static void buddy_remove_block(struct Page *base, int order) {
    list_del(&(base->page_link));
    ClearPageProperty(base);
    free_area[order].nr_free --;
}

static void buddy_system_init(void) {
    for (int order = 0; order < MAX_ORDER; order ++) {
        list_init(&(free_area[order].free_list));
        free_area[order].nr_free = 0;
    }
}

static void buddy_system_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    // split [base, base + n) into maximal aligned power-of-two blocks
    size_t idx = (size_t)(base - pages);
    size_t end = idx + n;
    while (idx < end) {
        int order = 0;
        while (order + 1 < MAX_ORDER &&
               (idx & (block_pages(order + 1) - 1)) == 0 &&
               idx + block_pages(order + 1) <= end) {
            order ++;
        }
        buddy_add_block(pages + idx, order);
        idx += block_pages(order);
    }
}

static struct Page *buddy_system_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > buddy_system_nr_free_pages()) {
        return NULL;
    }
    int order = pages_order(n);
    if (order >= MAX_ORDER) {
        return NULL;
    }

    // find the smallest order that still has a free block
    int k = order;
    while (k < MAX_ORDER && free_area[k].nr_free == 0) {
        k ++;
    }
    if (k == MAX_ORDER) {
        return NULL;
    }

    struct Page *page = le2page(list_next(&(free_area[k].free_list)), page_link);
    buddy_remove_block(page, k);

    // split the block until only 2^order pages are left
    while (k > order) {
        k --;
        buddy_add_block(page + block_pages(k), k);
    }
    return page;
}

static void buddy_system_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    int order = pages_order(n);
    assert(order < MAX_ORDER);

    size_t size = block_pages(order);
    size_t idx = (size_t)(base - pages);
    assert(idx + size <= (size_t)(npage - nbase));
    assert((idx & (size - 1)) == 0);

    struct Page *p = base;
    for (; p != base + size; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }

    buddy_add_block(base, order);

    // merge with the free buddy as long as possible
    while (order + 1 < MAX_ORDER) {
        size_t buddy_idx = idx ^ block_pages(order);
        if (buddy_idx >= (size_t)(npage - nbase)) {
            break;
        }
        struct Page *buddy = pages + buddy_idx;
        if (!PageProperty(buddy) || buddy->property != order) {
            break;
        }

        buddy_remove_block(buddy, order);
        buddy_remove_block(base, order);

        if (buddy < base) {
            base = buddy;
            idx = buddy_idx;
        }
        order ++;
        buddy_add_block(base, order);
    }
}

/* ------------------------------ self test ------------------------------- */

static bool blocks_overlap(struct Page *a, size_t na, struct Page *b, size_t nb) {
    return (a < b + nb) && (b < a + na);
}

/* A free block must be a power-of-two number of pages, aligned to its own
 * size, and its buddy must have been merged already.  Flags, per-order block
 * counts and the free page total are cross-checked as well. */
static void buddy_test_check_lists(void) {
    size_t total_pages = (size_t)(npage - nbase);
    size_t pages_sum = 0;
    size_t block_count = 0;

    for (int order = 0; order < MAX_ORDER; order ++) {
        size_t size = block_pages(order);
        size_t count = 0;
        list_entry_t *head = &(free_area[order].free_list);
        list_entry_t *le = head;

        // 块的页数必须是 2 的 n 次幂
        assert(size > 0 && (size & (size - 1)) == 0);

        while ((le = list_next(le)) != head) {
            struct Page *p = le2page(le, page_link);
            size_t idx = (size_t)(p - pages);

            assert(PageProperty(p));                // 只有块头带 PG_property
            assert(p->property == order);           // property 记录块的大小(阶)
            assert(block_pages(p->property) == size);
            assert((idx & (size - 1)) == 0);        // 块按自身大小对齐
            assert(idx + size <= total_pages);

            // 同阶的两个伙伴不能同时空闲，否则说明没有合并
            size_t buddy_idx = idx ^ size;
            if (buddy_idx < total_pages) {
                struct Page *buddy = pages + buddy_idx;
                assert(!(PageProperty(buddy) && buddy->property == order));
            }

            // 块内除头页外都不能带 PG_property
            for (size_t k = 1; k < size; k ++) {
                assert(!PageProperty(p + k));
            }

            count ++;
            block_count ++;
            pages_sum += size;
        }
        assert(count == free_area[order].nr_free);
    }

    assert(pages_sum == buddy_system_nr_free_pages());

    // 链表之外也不能残留“僵尸块头”
    size_t flagged = 0;
    for (size_t i = 0; i < total_pages; i ++) {
        if (PageProperty(pages + i)) {
            flagged ++;
        }
    }
    assert(flagged == block_count);
}

/* Allocate n pages and verify everything a correct buddy allocation must
 * satisfy: rounded up to a power of two, aligned to its own size, no longer
 * marked free, and the free counter drops by exactly the block size. */
static struct Page *buddy_test_alloc(size_t n) {
    size_t before = buddy_system_nr_free_pages();
    struct Page *p = buddy_system_alloc_pages(n);

    if (p == NULL) {
        return NULL;
    }

    int order = pages_order(n);
    size_t size = block_pages(order);
    size_t idx = (size_t)(p - pages);

    assert(order < MAX_ORDER);
    assert(size >= n);
    assert((size & (size - 1)) == 0);           // 2 的幂
    assert(!PageProperty(p));                   // 分配出去的块不再是空闲块
    assert(page_ref(p) == 0);                   // 引用计数由上层维护
    assert((idx & (size - 1)) == 0);            // 按自身大小对齐
    assert(idx + size <= (size_t)(npage - nbase));
    assert(before - buddy_system_nr_free_pages() == size);

    return p;
}

/* Free n pages; merging must not change the total, which grows by exactly the
 * rounded-up block size. */
static void buddy_test_free(struct Page *p, size_t n) {
    size_t before = buddy_system_nr_free_pages();
    int order = pages_order(n);

    assert(order < MAX_ORDER);
    buddy_system_free_pages(p, n);
    assert(buddy_system_nr_free_pages() == before + block_pages(order));
}

static void buddy_system_check(void) {
    size_t total_pages = (size_t)(npage - nbase);
    size_t free_store = buddy_system_nr_free_pages();
    size_t order_cnt[MAX_ORDER];

    assert(free_store > 0 && free_store <= total_pages);

    /* 1. 初始化后的空闲区必须由若干 2 的幂大小的块组成 */
    buddy_test_check_lists();
    for (int order = 0; order < MAX_ORDER; order ++) {
        order_cnt[order] = free_area[order].nr_free;
    }

    /* 2. 分配：任意 n 都向上取整到 2 的幂，块按自身大小对齐，互不重叠 */
    static const size_t request[] = {1, 2, 3, 4, 5, 7, 8, 9, 16, 17, 31, 32};
    struct Page *blocks[12];
    size_t sizes[12];
    size_t nr_blocks = sizeof(request) / sizeof(request[0]);

    assert(nr_blocks <= 12);
    for (size_t i = 0; i < nr_blocks; i ++) {
        blocks[i] = buddy_test_alloc(request[i]);
        assert(blocks[i] != NULL);

        sizes[i] = block_pages(pages_order(request[i]));
        assert(sizes[i] >= request[i]);
        assert((sizes[i] & (sizes[i] - 1)) == 0);
        for (size_t j = 0; j < i; j ++) {
            assert(!blocks_overlap(blocks[i], sizes[i], blocks[j], sizes[j]));
        }
    }
    buddy_test_check_lists();

    /* 3. 逆序释放：所有块逐级合并回去，空闲页数精确复原 */
    for (size_t i = nr_blocks; i > 0; i --) {
        buddy_test_free(blocks[i - 1], request[i - 1]);
    }
    assert(buddy_system_nr_free_pages() == free_store);
    buddy_test_check_lists();

    /* 4. 换一种分配/释放顺序，结果必须一致 */
    for (size_t i = 0; i < nr_blocks; i ++) {
        blocks[i] = buddy_test_alloc(request[i]);
        assert(blocks[i] != NULL);
    }
    buddy_test_check_lists();
    for (size_t i = 0; i < nr_blocks; i ++) {
        buddy_test_free(blocks[i], request[i]);
    }
    assert(buddy_system_nr_free_pages() == free_store);
    buddy_test_check_lists();

    /* 5. 边界：单页，以及当前最大的空闲块 */
    int max_order = -1;
    for (int order = MAX_ORDER - 1; order >= 0; order --) {
        if (free_area[order].nr_free > 0) {
            max_order = order;
            break;
        }
    }
    assert(max_order >= 0);

    struct Page *one = buddy_test_alloc(1);
    assert(one != NULL);
    buddy_test_free(one, 1);
    assert(buddy_system_nr_free_pages() == free_store);
    buddy_test_check_lists();

    struct Page *big = buddy_test_alloc(block_pages(max_order));
    assert(big != NULL);
    buddy_test_free(big, block_pages(max_order));
    assert(buddy_system_nr_free_pages() == free_store);
    buddy_test_check_lists();

    /* 6. 边界：超过全部空闲内存、以及超大请求都必须失败且不改状态 */
    assert(buddy_system_alloc_pages(free_store + 1) == NULL);
    assert(buddy_system_nr_free_pages() == free_store);

    assert(buddy_system_alloc_pages((size_t)1 << 40) == NULL);
    assert(buddy_system_nr_free_pages() == free_store);

    /* 总空闲页数够、但没有一个块装得下时也必须返回 NULL（碎片边界） */
    if (pages_order(free_store) > max_order) {
        assert(buddy_system_alloc_pages(free_store) == NULL);
        assert(buddy_system_nr_free_pages() == free_store);
    }

    /* 7. 固定种子的随机分配/释放，覆盖反复分裂与合并 */
    {
        struct Page *pool[16];
        size_t pool_n[16];
        size_t pool_cnt = 0;
        unsigned long seed = 0x5DEECE66DUL;

        for (int step = 0; step < 64; step ++) {
            seed = seed * 6364136223846793005UL + 1442695040888963407UL;

            if (pool_cnt < 16 && ((seed >> 33) & 1) != 0) {
                size_t n = 1 + (size_t)((seed >> 17) % 16);
                struct Page *p = buddy_system_alloc_pages(n);

                if (p != NULL) {
                    size_t size = block_pages(pages_order(n));
                    assert(!PageProperty(p));
                    assert(((size_t)(p - pages) & (size - 1)) == 0);
                    pool[pool_cnt] = p;
                    pool_n[pool_cnt] = n;
                    pool_cnt ++;
                }
            } else if (pool_cnt > 0) {
                size_t k = (size_t)((seed >> 20) % pool_cnt);
                buddy_system_free_pages(pool[k], pool_n[k]);
                pool[k] = pool[pool_cnt - 1];
                pool_n[k] = pool_n[pool_cnt - 1];
                pool_cnt --;
            }

            buddy_test_check_lists();
            assert(buddy_system_nr_free_pages() <= free_store);
        }

        while (pool_cnt > 0) {
            pool_cnt --;
            buddy_system_free_pages(pool[pool_cnt], pool_n[pool_cnt]);
        }
        assert(buddy_system_nr_free_pages() == free_store);
        buddy_test_check_lists();
    }

    /* 8. 全部释放后，各阶块数必须回到初始分解，而不只是总页数相等 */
    for (int order = 0; order < MAX_ORDER; order ++) {
        assert(free_area[order].nr_free == order_cnt[order]);
    }
}

const struct pmm_manager buddy_system_pmm_manager = {
    .name = "buddy_system_pmm_manager",
    .init = buddy_system_init,
    .init_memmap = buddy_system_init_memmap,
    .alloc_pages = buddy_system_alloc_pages,
    .free_pages = buddy_system_free_pages,
    .nr_free_pages = buddy_system_nr_free_pages,
    .check = buddy_system_check,
};
