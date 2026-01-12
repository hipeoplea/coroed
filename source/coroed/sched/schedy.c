#include "schedy.h"

#include <assert.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coroed/api/task.h"
#include "coroed/core/relax.h"
#include "coroed/core/spinlock.h"
#include "sched_policy.h"
#include "sched_task.h"
#include "sched_worker.h"
#include "uthread.h"

enum {
  /**
   * Максимальное количество файберов, которое может
   * одновременно выполняться на планировщике.
   */
  SCHED_THREADS_LIMIT = 512,

  /**
   * Количество рабочих потоков для исполнения файберов.
   */
  SCHED_WORKERS_COUNT = (size_t)(8),

  /**
   * Обеспечивает костыль для ретрая операций
   * `submit` и `acquire_next` при высокой
   * конкуренции на спинлоках файберов.
   */
  SCHED_NEXT_MAX_ATTEMPTS = (size_t)(16),
};

static struct spinlock tasks_lock;              // Защищает список задач
static size_t next_task_index = 0;              // Для планирования round-robin
static struct task tasks[SCHED_THREADS_LIMIT];  // Список всех задач

// Hint: для реализации более сложных схем управления, вам
//       вам могут понадобиться связаные списки (`core/list.h`).

static kthread_id_t kthread_ids[SCHED_WORKERS_COUNT];
static struct worker workers[SCHED_WORKERS_COUNT];

static task_sched_policy sched_policy_current;
static const struct sched_policy_ops* sched_ops;
static uint64_t sched_start_ts = 0;
static uint64_t sched_end_ts = 0;

static uint64_t sched_time_now_ns() {
  struct timespec spec;
  int code = clock_gettime(CLOCK_MONOTONIC, &spec);
  assert(code == 0);
  return (uint64_t)spec.tv_sec * 1000000000ULL + (uint64_t)spec.tv_nsec;
}

static int sched_u64_cmp(const void* left, const void* right) {
  uint64_t a = *(const uint64_t*)left;
  uint64_t b = *(const uint64_t*)right;
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

/**
 * Установить задачу в пустое состояние.
 */
void sched_task_init(struct task* task) {
  task->thread = NULL;
  task->worker = NULL;
  task->state = UTHREAD_ZOMBIE;
  spinlock_init(&task->lock);
  task->mlfq_level = 0;
  task->mlfq_ticks = 0;
  task->vruntime = 0;
  task->weight = 0;
  task->submit_ts = 0;
  task->first_run_ts = 0;
  task->finish_ts = 0;
  task->run_time_ns = 0;
  task->yields = 0;
}

/**
 * Установить рабочего в пустое состояние.
 */
void sched_worker_init(struct worker* worker, size_t index) {
  worker->index = index;
  worker->sched_thread.context = NULL;
  worker->running_task = NULL;
  worker->statistics.steps = 0;
  worker->statistics.finished = 0;
  worker->statistics.run_time_ns = 0;
}

void sched_init(task_sched_policy policy) {
  sched_policy_current = policy;
  sched_ops = sched_policy_get(policy);
  assert(sched_ops != NULL);
  sched_ops->init();
  spinlock_init(&tasks_lock);
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    sched_task_init(&tasks[i]);
  }
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    kthread_ids[i] = 0;
    sched_worker_init(&workers[i], i);
  }
}

/**
 * Находясь в контексте задачи `task`, переключиться
 * в контекст планировщика.
 */
void sched_switch_to_scheduler(struct task* task) {
  struct uthread* sched = &task->worker->sched_thread;
  task->worker = NULL;
  uthread_switch(task->thread, sched);
}

/**
 * Находясь в контексте планировщика, переключиться
 * в контекст задачи `task`. Выполнять ее до следующего
 * невынужденного возвращения в планировщик.
 */
void sched_switch_to(struct worker* worker, struct task* task) {
  assert(task->thread != &worker->sched_thread);

  task->state = UTHREAD_RUNNING;

  task->worker = worker;
  worker->running_task = task;

  struct uthread* sched = &worker->sched_thread;
  uint64_t start_ts = sched_time_now_ns();
  if (task->first_run_ts == 0) {
    task->first_run_ts = start_ts;
  }
  uthread_switch(sched, task->thread);
  uint64_t end_ts = sched_time_now_ns();
  uint64_t delta_ns = end_ts - start_ts;
  task->run_time_ns += delta_ns;
  worker->statistics.run_time_ns += delta_ns;
}

/**
 * Получить следующую задачу на исполнение в
 * соответствии с текущим алгоритмом планирования.
 *
 * Вызывающему передается владение задачей, а также
 * захваченный лок `task->lock`.
 */
static struct task* sched_acquire_next_rr();
static struct task* sched_acquire_next_policy(struct worker* worker);
static struct task* sched_acquire_next(struct worker* worker);
static task_t sched_submit_with_worker(void (*entry)(),
                                       void* argument,
                                       struct worker* worker);

/**
 * Вернуть задачу в очередь планирования.
 *
 * Очереди передается владение задачей,
 * а также она отпустит лок `task->lock`.
 */
void sched_release(struct worker* worker, struct task* task);

/**
 * Цикл планировщика. Выполняется, пока есть задачи.
 */
int sched_loop(void* argument) {
  struct worker* worker = argument;
  kthread_ids[worker->index] = kthread_id();

  for (;;) {
    struct task* task = sched_acquire_next(worker);
    if (task == NULL) {
      break;
    }

    sched_switch_to(worker, task);

    worker->statistics.steps += 1;
    if (task->state == UTHREAD_FINISHED) {
      worker->statistics.finished += 1;
    }

    sched_release(worker, task);

    // Hint: где-то здесь можно было бы опросить
    //       механизмы для неблокирующего ввода-вывода
    //       и перевести удовлетворенные BLOCKED
    //       потоки в RUNNABLE состояние.
  }

  return 0;
}

static struct task* sched_acquire_next_rr() {
  // На всякий случай пытаемся найти задачу несколько раз,
  // так как какие-то `task->lock` могли быть отпущены.

  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    spinlock_lock(&tasks_lock);  // Защитим `next_task_index`

    for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
      struct task* task = &tasks[next_task_index];
      if (!spinlock_try_lock(&task->lock)) {
        continue;
      }

      // Планирование round-robin
      next_task_index = (next_task_index + 1) % SCHED_THREADS_LIMIT;

      if (task->thread != NULL && task->state == UTHREAD_RUNNABLE) {
        spinlock_unlock(&tasks_lock);
        return task;
      }

      spinlock_unlock(&task->lock);
    }

    spinlock_unlock(&tasks_lock);
    SPINLOOP(2 * attempt);
  }

  return NULL;
}

static struct task* sched_acquire_next_policy(struct worker* worker) {
  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    struct task* task = sched_ops->dequeue(worker);
    if (task == NULL) {
      break;
    }

    if (!spinlock_try_lock(&task->lock)) {
      sched_ops->requeue(task, worker);
      continue;
    }

    if (task->thread != NULL && task->state == UTHREAD_RUNNABLE) {
      return task;
    }

    spinlock_unlock(&task->lock);
    sched_ops->requeue(task, worker);
  }

  return NULL;
}

static struct task* sched_acquire_next(struct worker* worker) {
  if (sched_policy_current == TASK_SCHED_RR) {
    return sched_acquire_next_rr();
  }

  return sched_acquire_next_policy(worker);
}

void sched_release(struct worker* worker, struct task* task) {
  task->worker = NULL;
  if (task->state == UTHREAD_FINISHED) {
    // Отправляем задачу на кладбище, а могли бы
    // еще, например, разблокировать зависимые задачи.
    uthread_reset(task->thread);
    task->state = UTHREAD_ZOMBIE;
    if (task->finish_ts == 0) {
      task->finish_ts = sched_time_now_ns();
    }
    if (sched_policy_current != TASK_SCHED_RR) {
      sched_ops->on_finish(task, worker);
    }
  } else if (task->state == UTHREAD_RUNNING) {
    task->state = UTHREAD_RUNNABLE;
    task->yields += 1;
    if (sched_policy_current != TASK_SCHED_RR) {
      sched_ops->on_yield(task, worker);
    }
  } else /* if (task->state == UTHREAD_BLOCKED) */ {
    assert(false && "Not implemented");
  }
  spinlock_unlock(&task->lock);
}

/**
 * Отметить задачу завершенной.
 */
void sched_finish(struct task* task) {
  task->state = UTHREAD_FINISHED;
}

void task_yield(struct task* caller) {
  sched_switch_to_scheduler(caller);
}

void task_exit(struct task* caller) {
  sched_finish(caller);
  task_yield(caller);
}

static task_t sched_try_submit_with_worker(void (*entry)(),
                                           void* argument,
                                           struct worker* worker) {
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    struct task* task = &tasks[i];

    if (!spinlock_try_lock(&task->lock)) {
      continue;
    }

    if (task->thread == NULL) {
      task->thread = uthread_allocate();
      assert(task->thread != NULL);
      task->state = UTHREAD_ZOMBIE;
    }

    const bool is_submitted = task->state == UTHREAD_ZOMBIE;

    if (task->state == UTHREAD_ZOMBIE) {
      uthread_reset(task->thread);
      uthread_set_entry(task->thread, entry);
      uthread_set_arg_0(task->thread, task);
      uthread_set_arg_1(task->thread, argument);
      task->state = UTHREAD_RUNNABLE;
      task->submit_ts = sched_time_now_ns();
      task->first_run_ts = 0;
      task->finish_ts = 0;
      task->run_time_ns = 0;
      task->yields = 0;
      if (sched_policy_current != TASK_SCHED_RR) {
        sched_ops->on_submit(task, worker);
      }
    }

    spinlock_unlock(&task->lock);
    if (is_submitted) {
      return (task_t){.task = task};
    }
  }

  return (task_t){.task = NULL};
}

task_t task_submit(struct task* caller, uthread_routine entry, void* argument) {
  task_t child = sched_submit_with_worker(*entry, argument, caller->worker);
  return child;
}

task_t sched_try_submit(void (*entry)(), void* argument) {
  return sched_try_submit_with_worker(entry, argument, NULL);
}

static task_t sched_submit_with_worker(void (*entry)(),
                                       void* argument,
                                       struct worker* worker) {
  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    task_t handle = sched_try_submit_with_worker(entry, argument, worker);
    if (handle.task != NULL) {
      return handle;
    }
    SPINLOOP(2 * attempt);
  }

  assert(false && "Can't create a task");
}

task_t sched_submit(void (*entry)(), void* argument) {
  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    task_t handle = sched_try_submit_with_worker(entry, argument, NULL);
    if (handle.task != NULL) {
      return handle;
    }
    SPINLOOP(2 * attempt);
  }

  assert(false && "Can't create a task");
}

void sched_start() {
  sched_start_ts = sched_time_now_ns();
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    enum kthread_status status = kthread_create(&worker->kthread, sched_loop, worker);
    assert(status == KTHREAD_SUCCESS);
  }
}

void sched_wait() {
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    enum kthread_status status = kthread_join(&worker->kthread);
    assert(status == KTHREAD_SUCCESS);
  }
  sched_end_ts = sched_time_now_ns();
}

void sched_print_statistics() {
  printf("\nsched statistics\n");

  size_t tasks_count = 0;
  size_t steps_count = 0;
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    tasks_count += worker->statistics.finished;
    steps_count += worker->statistics.steps;
  }

  printf("|- tasks executed %zu\n", tasks_count);
  printf("|- steps done     %zu\n", steps_count);

  uint64_t latencies_ns[SCHED_THREADS_LIMIT];
  uint64_t responses_ns[SCHED_THREADS_LIMIT];
  uint64_t total_latency_ns = 0;
  uint64_t total_response_ns = 0;
  size_t measured = 0;

  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    struct task* task = &tasks[i];
    if (task->submit_ts == 0 || task->finish_ts == 0) {
      continue;
    }
    uint64_t latency = task->finish_ts - task->submit_ts;
    uint64_t response = 0;
    if (task->first_run_ts > task->submit_ts) {
      response = task->first_run_ts - task->submit_ts;
    }
    latencies_ns[measured] = latency;
    responses_ns[measured] = response;
    total_latency_ns += latency;
    total_response_ns += response;
    measured += 1;
  }

  if (measured > 0) {
    qsort(latencies_ns, measured, sizeof(uint64_t), sched_u64_cmp);
    qsort(responses_ns, measured, sizeof(uint64_t), sched_u64_cmp);
    size_t p95_index = (measured * 95 + 99) / 100;
    if (p95_index == 0) {
      p95_index = 1;
    }
    p95_index -= 1;

    uint64_t total_time_ns = 0;
    if (sched_end_ts > sched_start_ts) {
      total_time_ns = sched_end_ts - sched_start_ts;
    }
    double total_time_s = (double)total_time_ns / 1000000000.0;
    double throughput = total_time_s > 0.0 ? (double)measured / total_time_s : 0.0;
    double avg_latency_ms = (double)total_latency_ns / (double)measured / 1000000.0;
    double avg_response_ms = (double)total_response_ns / (double)measured / 1000000.0;
    double p95_latency_ms = (double)latencies_ns[p95_index] / 1000000.0;
    double p95_response_ms = (double)responses_ns[p95_index] / 1000000.0;

    printf("|- total time    %.3f s\n", total_time_s);
    printf("|- throughput    %.2f tasks/s\n", throughput);
    printf("|- latency avg   %.3f ms\n", avg_latency_ms);
    printf("|- latency p95   %.3f ms\n", p95_latency_ms);
    printf("|- response avg  %.3f ms\n", avg_response_ms);
    printf("|- response p95  %.3f ms\n", p95_response_ms);
  }

  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    printf("|- worker %zu %zu\n", i, kthread_ids[i]);
    printf("   |- steps     %zu\n", worker->statistics.steps);
    printf("   |- finished  %zu\n", worker->statistics.finished);
    printf("   |- run time  %.3f ms\n", (double)worker->statistics.run_time_ns / 1000000.0);
    if (worker->statistics.finished > 0) {
      double avg_runtime_ms =
          (double)worker->statistics.run_time_ns / (double)worker->statistics.finished / 1000000.0;
      printf("   |- avg task  %.3f ms\n", avg_runtime_ms);
    }
  }
}

void sched_destroy() {
  if (sched_ops != NULL) {
    sched_ops->destroy();
  }
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    struct task* task = &tasks[i];
    spinlock_lock(&task->lock);
    if (task->thread != NULL) {
      uthread_free(task->thread);
    }
    spinlock_unlock(&task->lock);
  }
}

size_t sched_workers_count() {
  return SCHED_WORKERS_COUNT;
}


static void sched_rr_init() {
}

static void sched_rr_on_submit(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static struct task* sched_rr_dequeue(struct worker* worker) {
  (void)worker;
  return NULL;
}

static void sched_rr_requeue(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static void sched_rr_on_yield(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static void sched_rr_on_finish(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static void sched_rr_destroy() {
}

static const struct sched_policy_ops sched_rr_ops_instance = {
    .init = sched_rr_init,
    .on_submit = sched_rr_on_submit,
    .dequeue = sched_rr_dequeue,
    .requeue = sched_rr_requeue,
    .on_yield = sched_rr_on_yield,
    .on_finish = sched_rr_on_finish,
    .destroy = sched_rr_destroy,
};

const struct sched_policy_ops* sched_rr_ops() {
  return &sched_rr_ops_instance;
}
