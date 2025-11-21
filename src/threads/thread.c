/* thread.c -- versão adaptada para MLFQS */
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
#include "devices/timer.h"   /* <-- adicionado: TIMER_FREQ, timer_ticks() */
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

/* Statistics. */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* Scheduling. */
#define TIME_SLICE 4
static unsigned thread_ticks;

bool thread_mlfqs;

/* ----------------------- MLFQS: ponto fixo 17.14 ----------------------- */
/* ponto fixo (F = 1 << 14) */
#define FP (1 << 14)

/* conversões e operações de ponto fixo (implementação simples e testada) */
static int int_to_fp (int n)        { return n * FP; }
static int fp_to_int (int x)        { return x / FP; }
static int fp_to_int_round (int x)  { return x >= 0 ? (x + FP/2) / FP : (x - FP/2) / FP; }

/* fixed + fixed */
static int fp_add (int x, int y)    { return x + y; }
static int fp_sub (int x, int y)    { return x - y; }

/* fixed * fixed  -> (x * y) / FP */
static int fp_mul (int x, int y)    { return (int) (((int64_t) x) * y / FP); }

/* fixed / fixed -> (x * FP) / y */
static int fp_div (int x, int y)    { return (int) (((int64_t) x) * FP / y); }

/* fixed * int, fixed / int */
static int fp_mul_int (int x, int n){ return x * n; }
static int fp_div_int (int x, int n){ return x / n; }

/* load_avg em ponto fixo */
static int load_avg = 0;

/* compara prioridade para ordenar ready_list (maior prioridade primeiro) */
static bool thread_priority_cmp (const struct list_elem *a,
                                 const struct list_elem *b,
                                 void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}

/* atualiza prioridade de uma thread (usa recent_cpu e nice) */
static void mlfqs_recalc_priority (struct thread *t)
{
  if (t == idle_thread)
    return;

  /* priority = PRI_MAX - (recent_cpu / 4) - (nice * 2) */
  int recent_div4 = fp_to_int_round ( fp_div_int (t->recent_cpu, 4) );
  int new_prio = PRI_MAX - recent_div4 - (t->nice * 2);

  if (new_prio > PRI_MAX) new_prio = PRI_MAX;
  if (new_prio < PRI_MIN) new_prio = PRI_MIN;
  t->priority = new_prio;
}

/* recalcula load_avg: load_avg = (59/60)*load_avg + (1/60)*ready_threads */
static void mlfqs_recalc_load_avg (void)
{
  /* ready_threads = tamanho do ready_list;
     se thread atual não for idle, contar também a running thread */
  int ready_threads = (int) list_size (&ready_list);
  if (thread_current () != idle_thread)
    ready_threads++;

  /* load_avg = (59/60)*load_avg + (1/60)*ready_threads
     Em ponto fixo: load_avg = (59*load_avg + int_to_fp(ready_threads)) / 60
     Note: load_avg e int_to_fp(ready_threads) já são valores fixed. */
  int numerator = fp_add ( fp_mul_int (load_avg, 59), int_to_fp (ready_threads) );
  load_avg = fp_div_int (numerator, 60);
}

/* recalcula recent_cpu para todas threads: recent_cpu = (2*load_avg)/(2*load_avg+1) * recent_cpu + nice */
static void mlfqs_recalc_recent_cpu_all (void)
{
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      if (t == idle_thread)
        continue;

      /* coef = (2*load_avg) / (2*load_avg + 1)  (todos fixed)
         Calculamos coef como fixed: coef = fp_div (2*load_avg, 2*load_avg + FP) */
      int two_load = fp_mul_int (load_avg, 2);
      int denom = fp_add ( two_load, int_to_fp (1) ); /* 2*load_avg + 1 (fixed) */
      int coef = fp_div ( two_load, denom );

      /* recent_cpu = coef * recent_cpu + nice */
      t->recent_cpu = fp_add ( fp_mul (coef, t->recent_cpu), int_to_fp (t->nice) );
    }
}

/* ---------------------------------------------------------------------- */

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

/* Initializes the threading system. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  /* --- MLFQS: inicializações default --- */
  if (thread_mlfqs)
    {
      initial_thread->nice = 0;
      initial_thread->recent_cpu = 0;
      load_avg = 0;
    }
}

/* Starts preemptive thread scheduling. */
void
thread_start (void) 
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();
  sema_down (&idle_started);
}

/* Called by timer interrupt at each tick. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* --- MLFQS: incrementa recent_cpu cada tick para thread rodando (exceto idle) --- */
  if (thread_mlfqs && t != idle_thread)
    t->recent_cpu = fp_add (t->recent_cpu, int_to_fp (1));

  /* A cada 4 ticks: recalcular prioridade da thread corrente */
  if (thread_mlfqs && timer_ticks () % 4 == 0)
    {
      mlfqs_recalc_priority (t);
      /* se houver thread pronta com prioridade maior, ceda (preempt) */
      if (!list_empty (&ready_list))
        {
          struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
          if (front->priority > t->priority)
            intr_yield_on_return ();
        }
    }

  /* A cada segundo (TIMER_FREQ): recalcula load_avg, recent_cpu e priorities para todas */
  if (thread_mlfqs && timer_ticks () % TIMER_FREQ == 0)
    {
      mlfqs_recalc_load_avg ();
      mlfqs_recalc_recent_cpu_all ();
      /* recalcula prioridade de todas as threads e reordena ready_list */
      struct list_elem *e;
      for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
        {
          struct thread *tt = list_entry (e, struct thread, allelem);
          mlfqs_recalc_priority (tt);
        }
      /* reordena ready_list por prioridade (maior primeiro) */
      list_sort (&ready_list, thread_priority_cmp, NULL);
      
    }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/*lalalal*/
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with initial PRIORITY. */
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

  /* Stack frames */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* --- MLFQS: herdar nice e recent_cpu do pai (se MLFQS ativo) --- */
  if (thread_mlfqs)
    {
      struct thread *cur = thread_current ();
      t->nice = cur->nice;
      t->recent_cpu = cur->recent_cpu;
    }

  /* Add to run queue (in order). */
  thread_unblock (t);

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
  /* Inserir na ready_list ordenado por prioridade (maior prioridade na frente) */
  list_insert_ordered (&ready_list, &t->elem, thread_priority_cmp, NULL);
  t->status = THREAD_READY;
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
    list_insert_ordered (&ready_list, &cur->elem, thread_priority_cmp, NULL);
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

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  /* if MLFQS ativo, prioridades são gerenciadas automaticamente */
  if (thread_mlfqs)
    return;

  int old = thread_current ()->priority;
  thread_current ()->priority = new_priority;

  /* se existe thread pronta com prioridade maior, cede */
  if (!list_empty (&ready_list))
    {
      struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
      if (front->priority > thread_current ()->priority)
        thread_yield ();
    }
}

int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  enum intr_level old_level = intr_disable ();
  thread_current ()->nice = nice;
  if (thread_mlfqs)
    {
      /* recalcula prioridade da corrente e pode ceder */
      mlfqs_recalc_priority (thread_current ());
      if (!list_empty (&ready_list))
        {
          struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
          if (front->priority > thread_current ()->priority)
            intr_yield_on_return ();
        }
    }
  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* return round(load_avg * 100) */
  return fp_to_int_round ( fp_mul_int (load_avg, 100) );
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return fp_to_int_round ( fp_mul_int (thread_current ()->recent_cpu, 100) );
}

/* Idle thread. */
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

  /* --- MLFQS: inicializa campos nice e recent_cpu --- */
  if (thread_mlfqs)
    {
      t->nice = 0;
      t->recent_cpu = 0;
    }

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

uint32_t thread_stack_ofs = offsetof (struct thread, stack);
