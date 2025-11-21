/* thread.c — priority scheduling + priority donation */

#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define A 55

static struct list ready_list;
static struct list all_list;
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

struct kernel_thread_frame 
  {
    void *eip;
    thread_func *function;
    void *aux;
  };

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

bool thread_mlfqs;

/* ---------------- Priority helper ------------------ */
/* Compare threads by priority: return true if A's priority > B's. */
static bool
thread_priority_cmp (const struct list_elem *a,
                     const struct list_elem *b,
                     void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}

/* Compare threads by effective priority for donation lists (same semantics) */
static bool
donor_priority_cmp (const struct list_elem *a,
                    const struct list_elem *b,
                    void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, donation_elem);
  struct thread *tb = list_entry (b, struct thread, donation_elem);
  return ta->priority > tb->priority;
}

/* Forward declarations */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* --------------- Priority donation helpers ------------- */

/* Donate priority from donor to lock holder(s) (nested). */
void
thread_donate_priority(struct thread *donor)
{
  /* Donate along the chain of waiting_lock -> holder */
  struct lock *lock = donor->waiting_lock;
  int donated = donor->priority;

  while (lock != NULL && lock->holder != NULL)
    {
      struct thread *holder = lock->holder;
      if (holder->priority < donated)
        {
          holder->priority = donated;
          /* ensure donor is recorded in holder's donations list */
          if (!list_empty(&holder->donations))
            {
              /* check if donor is already there; if not, insert ordered */
              bool found = false;
              struct list_elem *e;
              for (e = list_begin(&holder->donations); e != list_end(&holder->donations); e = list_next(e))
                {
                  struct thread *t = list_entry(e, struct thread, donation_elem);
                  if (t == donor) { found = true; break; }
                }
              if (!found)
                list_insert_ordered(&holder->donations, &donor->donation_elem, donor_priority_cmp, NULL);
            }
          else
            list_insert_ordered(&holder->donations, &donor->donation_elem, donor_priority_cmp, NULL);
          /* go to next level: if holder is also waiting for a lock, continue */
          lock = holder->waiting_lock;
        }
      else
        {
          /* if holder already has priority >= donated, still want to
             ensure donor is in the donations list for later cleanup */
          if (!list_empty(&holder->donations))
            {
              bool found = false;
              struct list_elem *e;
              for (e = list_begin(&holder->donations); e != list_end(&holder->donations); e = list_next(e))
                {
                  struct thread *t = list_entry(e, struct thread, donation_elem);
                  if (t == donor) { found = true; break; }
                }
              if (!found)
                list_insert_ordered(&holder->donations, &donor->donation_elem, donor_priority_cmp, NULL);
            }
          else
            list_insert_ordered(&holder->donations, &donor->donation_elem, donor_priority_cmp, NULL);
          /* still continue nested donation so long as holder is waiting on a lock */
          lock = holder->waiting_lock;
        }
    }
}

/* Remove donations in current thread's donations list that are associated
   with a given lock (called when releasing lock). */
void
thread_remove_with_lock(struct lock *lock)
{
  struct thread *cur = thread_current ();
  struct list_elem *e = list_begin(&cur->donations);
  while (e != list_end(&cur->donations))
    {
      struct thread *t = list_entry(e, struct thread, donation_elem);
      e = list_next(e);
      if (t->waiting_lock == lock)
        {
          list_remove(&t->donation_elem);
        }
    }
}

/* Refresh a thread's effective priority: max(original_priority, donors priorities) */
void
thread_refresh_priority(struct thread *t)
{
  int maxp = t->original_priority;
  if (!list_empty(&t->donations))
    {
      struct thread *top = list_entry(list_front(&t->donations), struct thread, donation_elem);
      if (top->priority > maxp)
        maxp = top->priority;
    }
  t->priority = maxp;
}

/* -------------- Standard thread system --------------- */

void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

void
thread_start (void) 
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();

  sema_down (&idle_started);
}

void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  thread_unblock (t);

  /* Preempt if new thread has higher priority. If in interrupt context, don't yield now. */
  if (!intr_context () && t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  /* Insert ordered by priority */
  list_insert_ordered (&ready_list, &t->elem, thread_priority_cmp, NULL);
  t->status = THREAD_READY;

  /* If the newly unblocked thread has higher priority than current, preempt */
  if (!intr_context ())
    {
      struct thread *cur = thread_current ();
      struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
      if (front->priority > cur->priority)
        {
          intr_set_level (old_level);
          thread_yield ();
          return;
        }
    }

  intr_set_level (old_level);
}

const char *
thread_name (void) 
{
  return thread_current ()->name;
}

struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    {
      list_insert_ordered (&ready_list, &cur->elem, thread_priority_cmp, NULL);
    }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* When a thread explicitly sets its base priority, update original_priority
   and then recompute effective priority considering donations. */
void
thread_set_priority (int new_priority) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level = intr_disable ();
  cur->original_priority = new_priority;
  /* refresh effective priority */
  thread_refresh_priority(cur);

  /* If there's a ready thread with higher priority, yield. */
  if (!list_empty (&ready_list))
    {
      struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
      if (front->priority > cur->priority)
        {
          intr_set_level (old_level);
          thread_yield ();
          return;
        }
    }

  intr_set_level (old_level);
}

int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* nice/load_avg/recent_cpu not implemented in this patch */
void
thread_set_nice (int nice UNUSED) { }
int thread_get_nice (void) { return 0; }
int thread_get_load_avg (void) { return 0; }
int thread_get_recent_cpu (void) { return 0; }

static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
}

static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);
  intr_enable ();
  function (aux);
  thread_exit ();
}

struct thread *
running_thread (void) 
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;

  /* initialize priority fields (original + effective) */
  t->original_priority = priority;
  t->priority = priority;

  /* donation fields */
  t->waiting_lock = NULL;
  list_init(&t->donations);

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

static void *
alloc_frame (struct thread *t, size_t size) 
{
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);
  t->stack -= size;
  return t->stack;
}

static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  ASSERT (intr_get_level () == INTR_OFF);
  cur->status = THREAD_RUNNING;
  thread_ticks = 0;
#ifdef USERPROG
  process_activate ();
#endif
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;
  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* Offset of `stack' member within `struct thread'. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
