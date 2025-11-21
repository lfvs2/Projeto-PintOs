#include "threads/synch.h"
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* --------------------------
   Comparadores estáticos
   -------------------------- */

/* Compara threads pela prioridade (maior prioridade primeiro). */
static bool
cmp_priority (const struct list_elem *a,
              const struct list_elem *b,
              void *aux UNUSED)
{
  const struct thread *t1 = list_entry(a, struct thread, elem);
  const struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->priority > t2->priority;
}

/* Compara semaphore_elem pela prioridade da thread no front do seu semáforo. */
static bool
cmp_sema_priority (const struct list_elem *a,
                   const struct list_elem *b,
                   void *aux UNUSED)
{
  const struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
  const struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

  /* Se algum semáforo não tem waiters (caso improvável aqui), considera PRI_MIN. */
  if (list_empty (&sa->semaphore.waiters))
    return false;
  if (list_empty (&sb->semaphore.waiters))
    return true;

  const struct thread *ta = list_entry(list_front(&sa->semaphore.waiters), struct thread, elem);
  const struct thread *tb = list_entry(list_front(&sb->semaphore.waiters), struct thread, elem);
  return ta->priority > tb->priority;
}

/* --------------------------
   SEMÁFOROS
   -------------------------- */

void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);
  sema->value = value;
  list_init (&sema->waiters);
}

void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      /* inserir ordenado por prioridade */
      list_insert_ordered (&sema->waiters, &thread_current ()->elem, cmp_priority, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;
  struct thread *t_unblocked = NULL;

  ASSERT (sema != NULL);

  old_level = intr_disable ();

  if (!list_empty (&sema->waiters)) 
    {
      /* garante que maior prioridade esteja na frente */
      list_sort (&sema->waiters, cmp_priority, NULL);

      /* pega a thread que vamos acordar */
      t_unblocked = list_entry (list_pop_front (&sema->waiters),
                                    struct thread, elem);

      /* importante: primeiro atualizar o valor do semáforo */
      sema->value++;

      /* desbloqueia a thread (mantendo interrupts off). */
      thread_unblock (t_unblocked);
    }
  else
    {
      /* nenhuma waiter: só incrementa o semáforo */
      sema->value++;
    }

  /* restaura nível de interrupção antes de potencialmente ceder CPU */
  intr_set_level (old_level);

  /* se a thread que acordamos tem maior prioridade, cede CPU */
  if (t_unblocked != NULL && !intr_context () &&
      t_unblocked->priority > thread_current ()->priority)
    thread_yield ();
}

static void sema_test_helper (void *sema_);

/* Self-test para semáforos (mesmo comportamento). */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;
  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;
  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* --------------------------
   LOCK (com hooks para donation)
   -------------------------- */

void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* Acquire com priority donation:
   - se thread atual encontrar lock ocupado: marca waiting_lock e chama
     thread_donate_priority() para propagar doação encadeada.
   - depois faz sema_down() e passa a ser holder. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable ();

  if (lock->holder != NULL)
    {
      /* indica em qual lock estamos esperando (usado por donation). */
      thread_current ()->waiting_lock = lock;
      /* chame helper em thread.c para propagar a doação (inserir na lista de donations
         do holder e propagar recursivamente). */
      thread_donate_priority (thread_current ());
    }

  intr_set_level (old_level);

  /* bloqueia até obter o semáforo do lock */
  sema_down (&lock->semaphore);

  /* adquirimos o lock: torne-nos holder e limpe waiting_lock */
  lock->holder = thread_current ();
  thread_current ()->waiting_lock = NULL;
}

bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Release com limpeza de doações:
   - remove doações relacionadas ao lock do holder
   - recalcula prioridade efetiva do holder
*/
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  /* remove doações que vieram por causa deste lock */
  thread_remove_with_lock (lock);

  /* recalcula prioridade efetiva do liberador */
  thread_refresh_priority (thread_current ());

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);
  return lock->holder == thread_current ();
}

/* --------------------------
   CONDITION VARIABLES
   -------------------------- */

void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);
  list_init (&cond->waiters);
}

void
cond_wait (struct condition *cond, struct lock *lock)
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  sema_init (&waiter.semaphore, 0);

  /* Inserir ordered de acordo com a prioridade do primeiro waiter do semáforo. */
  list_insert_ordered (&cond->waiters, &waiter.elem, cmp_sema_priority, NULL);

  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters))
    {
      /* garante que o semaphore_elem com maior prioridade esteja na frente */
      list_sort (&cond->waiters, cmp_sema_priority, NULL);
      struct semaphore_elem *se =
        list_entry (list_pop_front (&cond->waiters),
                    struct semaphore_elem, elem);
      sema_up (&se->semaphore);
    }
}

void
cond_broadcast (struct condition *cond, struct lock *lock)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
