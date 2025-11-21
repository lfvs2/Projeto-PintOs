/* threads/thread.c  --  MLFQS-ready implementation (fixed-point 17.14) */

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
#include "devices/timer.h" /* for TIMER_FREQ */

/* Random value for struct thread's `magic' member. */
#define THREAD_MAGIC 0xcd6abf4b

/* Fixed-point constants (17.14 format) */
#define F 16384 /* 1 << 14 */

#define INT_TO_FP(n) ((n) * F)
#define FP_TO_INT_ZERO(x) ((x) / F)
#define FP_TO_INT_NEAR(x) ((x) >= 0 ? ((x) + F/2) / F : ((x) - F/2) / F)
#define FP_ADD(x,y) ((x) + (y))
#define FP_SUB(x,y) ((x) - (y))
#define FP_ADD_INT(x,n) ((x) + (n)*F)
#define FP_SUB_INT(x,n) ((x) - (n)*F)
#define FP_MUL(x,y) ((int64_t)(x) * (y) / F)
#define FP_DIV(x,y) ((int64_t)(x) * F / (y))
#define FP_MUL_INT(x,n) ((x) * (n))
#define FP_DIV_INT(x,n) ((x) / (n))

/* Statistics and scheduling structures */
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

/* MLFQS globals (this file defines thread_mlfqs; header declares extern) */
bool thread_mlfqs = true;
static int load_avg; /* fixed-point */

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

/* MLFQS helpers (prototypes so thread_tick can call them) */
static void mlfqs_update_load_avg(void);
static void mlfqs_update_recent_cpu(struct thread *t);
static void mlfqs_update_priority(struct thread *t);

/* Comparator by priority (higher first) for ready_list */
static bool
cmp_priority (const struct list_elem *a,
              const struct list_elem *b,
              void *aux UNUSED)
{
  const struct thread *ta = list_entry(a, struct thread, elem);
  const struct thread *tb = list_entry(b, struct thread, elem);
  return ta->priority > tb->priority;
}

/* thread_init */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  load_avg = 0; /* fixed-point 0 */

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* thread_start */
void
thread_start (void)
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();
  sema_down (&idle_started);
}

/* thread_tick - called on every timer tick */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  /* MLFQS: increment recent_cpu for running non-idle thread */
  if (thread_mlfqs && t != idle_thread)
    t->recent_cpu = FP_ADD_INT(t->recent_cpu, 1);

  /* Update load_avg and recent_cpu once per second */
  if (thread_mlfqs && timer_ticks() % TIMER_FREQ == 0)
    {
      mlfqs_update_load_avg();

      struct list_elem *e;
      for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
        mlfqs_update_recent_cpu(list_entry(e, struct thread, allelem));
    }

  /* Recalculate priorities periodically (every 4 ticks as in spec) */
  if (thread_mlfqs && timer_ticks() % 4 == 0)
    {
      struct list_elem *e;
      for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
        mlfqs_update_priority(list_entry(e, struct thread, allelem));

      /* keep ready_list ordered by new priorities */
      list_sort(&ready_list, cmp_priority, NULL);
    }

  /* Stats update */
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

/* thread_create */
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

  /* stack frames */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* MLFQS defaults */
  t->nice = 0;
  t->recent_cpu = 0;

  thread_unblock (t);
  return tid;
}

/* thread_block */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* thread_unblock (insert ordered) */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);

  /* Insert ordered by priority so highest-priority at front */
  list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* thread_name, thread_current, thread_tid, thread_exit omitted for brevity
   (keep your existing implementations; they are unchanged) */

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

/* thread_yield (insert ordered) */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    list_insert_ordered(&ready_list, &cur->elem, cmp_priority, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* thread_foreach unchanged */
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

/* thread_set_priority: ignored under mlfqs */
void
thread_set_priority (int new_priority)
{
  if (thread_mlfqs)
    return;
  thread_current ()->priority = new_priority;
}

int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Nice / getters */

void
thread_set_nice (int nice)
{
  struct thread *t = thread_current ();
  if (nice < -20) nice = -20;
  if (nice > 20) nice = 20;
  t->nice = nice;
  if (thread_mlfqs)
    mlfqs_update_priority(t);
  thread_yield();
}

int
thread_get_nice (void)
{
  return thread_current ()->nice;
}

/* Returns 100 * recent_cpu (rounded to nearest) */
int
thread_get_recent_cpu (void)
{
  struct thread *t = thread_current ();
  /* recent_cpu is in fixed-point; multiply by 100 then convert */
  return FP_TO_INT_NEAR(FP_MUL_INT(t->recent_cpu, 100));
}

/* Returns 100 * load_avg (rounded to nearest) */
int
thread_get_load_avg (void)
{
  return FP_TO_INT_NEAR(FP_MUL_INT(load_avg, 100));
}

/* Idle, kernel_thread, running_thread, is_thread, init_thread, alloc_frame follow */

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
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  /* MLFQS defaults */
  t->nice = 0;
  t->recent_cpu = 0;

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

/* next_thread_to_run and schedule */
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

/* --------------------
   MLFQS helper functions
   -------------------- */

/* Update priority of thread t:
   priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
   All ops in fixed-point. */
static void
mlfqs_update_priority(struct thread *t)
{
  if (t == idle_thread)
    return;

  /* priority_fp = PRI_MAX - recent_cpu/4 - 2*nice */
  int priority_fp = INT_TO_FP(PRI_MAX);
  priority_fp = FP_SUB(priority_fp, FP_DIV_INT(t->recent_cpu, 4));
  priority_fp = FP_SUB(priority_fp, INT_TO_FP(t->nice * 2));

  int new_priority = FP_TO_INT_ZERO(priority_fp);

  if (new_priority > PRI_MAX) new_priority = PRI_MAX;
  if (new_priority < PRI_MIN) new_priority = PRI_MIN;

  t->priority = new_priority;
}

/* Update recent_cpu of thread t:
   recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice
*/
static void
mlfqs_update_recent_cpu(struct thread *t)
{
  if (t == idle_thread)
    return;

  /* coeff = (2*load_avg) / (2*load_avg + 1) */
  int two_load = FP_MUL_INT(load_avg, 2);
  int coeff = FP_DIV(two_load, FP_ADD_INT(two_load, 1));
  t->recent_cpu = FP_ADD(FP_MUL(coeff, t->recent_cpu), INT_TO_FP(t->nice));
}

/* Update load_avg:
   load_avg = (59/60)*load_avg + (1/60)*ready_threads
*/
static void
mlfqs_update_load_avg(void)
{
  int ready = list_size(&ready_list);
  if (thread_current() != idle_thread)
    ready++;

  /* load_avg = (59/60) * load_avg + (1/60) * ready */
  int part1 = FP_DIV_INT(FP_MUL_INT(load_avg, 59), 60);
  int part2 = FP_DIV_INT(INT_TO_FP(ready), 60);
  load_avg = FP_ADD(part1, part2);
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

