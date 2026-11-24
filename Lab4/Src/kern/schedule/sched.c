#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 2213633 CODE
// schedule - pick the next runnable thread (round-robin starting after the
// current one) and switch to it.  If nothing is runnable, fall back to the
// idle thread.  The selection and the switch are done with interrupts
// disabled so that the run queue cannot change half-way through.
void schedule(void)
{
    bool intr_flag;
    list_entry_t *le, *last;
    struct proc_struct *next = NULL;

    local_intr_save(intr_flag);
    {
        current->need_resched = 0;

        // idleproc is not linked into proc_list, so start from the list head
        // for it; otherwise start right after the current thread
        last = (current == idleproc) ? &proc_list : &(current->list_link);
        le = last;
        do
        {
            if ((le = list_next(le)) != &proc_list)
            {
                next = le2proc(le, list_link);
                if (next->state == PROC_RUNNABLE)
                {
                    break;
                }
            }
        } while (le != last);

        if (next == NULL || next->state != PROC_RUNNABLE)
        {
            next = idleproc;
        }

        next->runs++;
        if (next != current)
        {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}


