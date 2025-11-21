#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Forward declaration to avoid circular include with synch.h */
struct lock;

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
   Each thread structure is stored in its own 4 kB page... (doc omitted) */

/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */

    /* Priority fields */
    int priority;                       /* Effective priority (may change due to donation). */
    int original_priority;              /* Base/original priority (not changed by donations). */

    /* Sleep/wakeup support (if you use it) */
    int64_t wake_up_time;               /* tick to mark when thread needs to wake up */

    /* MLFQS fields (kept here but may be unused if mlfqs disabled) */
    int nice;                           /* used in MLFQS calculus */
    int recent_cpu;                     /* used in MLFQS calculus */

    /* Donation-related fields */
    struct lock *waiting_lock;          /* Lock the thread is waiting on (for donation). */
    struct list donations;              /* List of threads that donated to this thread. */
    struct list_elem donation_elem;     /* List elem for being in another thread's donations list. */

    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element (run queue or semaphore wait list). */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

/* Thread system functions */
void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

/* Priority API */
int thread_get_priority (void);
void thread_set_priority (int new_priority);

/* MLFQS API (kept, may be unimplemented if not using) */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

/* --- Donation helpers (internal helpers exposed for linkage) --- */
/* Donate priority from donor along lock-holder chain */
void thread_donate_priority(struct thread *donor);
/* Remove donations associated with a released lock from current thread */
void thread_remove_with_lock(struct lock *lock);
/* Refresh effective priority from original_priority and current donations */
void thread_refresh_priority(struct thread *t);
/* --------------------------------------------------------------- */

#endif /* threads/thread.h */
