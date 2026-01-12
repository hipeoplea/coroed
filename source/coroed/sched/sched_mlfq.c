#include "sched_mlfq.h"

#include <assert.h>
#include <stdlib.h>

#include "coroed/core/spinlock.h"
#include "queue.h"
#include "sched_worker.h"
#include "schedy.h"

enum {
  SCHED_MLFQ_LEVELS = 4,
};

static const size_t mlfq_quantums[SCHED_MLFQ_LEVELS] = {1, 2, 4, 8};

TAILQ_HEAD(task_queue, task);

struct mlfq_runqueue {
  struct spinlock lock;
  struct task_queue queues[SCHED_MLFQ_LEVELS];
};

static struct mlfq_runqueue* mlfq_runqueues = NULL;
static size_t mlfq_workers = 0;
static struct spinlock mlfq_submit_lock;
static size_t mlfq_next_worker = 0;

static struct mlfq_runqueue* sched_mlfq_rq(struct worker* worker) {
  assert(worker != NULL);
  assert(worker->index < mlfq_workers);
  return &mlfq_runqueues[worker->index];
}

static struct mlfq_runqueue* sched_mlfq_pick_rq() {
  spinlock_lock(&mlfq_submit_lock);
  size_t index = mlfq_next_worker;
  mlfq_next_worker = (mlfq_next_worker + 1) % mlfq_workers;
  spinlock_unlock(&mlfq_submit_lock);
  return &mlfq_runqueues[index];
}

static void sched_mlfq_init() {
  mlfq_workers = sched_workers_count();
  mlfq_runqueues = calloc(mlfq_workers, sizeof(*mlfq_runqueues));
  assert(mlfq_runqueues != NULL);

  spinlock_init(&mlfq_submit_lock);
  mlfq_next_worker = 0;

  for (size_t worker = 0; worker < mlfq_workers; ++worker) {
    spinlock_init(&mlfq_runqueues[worker].lock);
    for (size_t level = 0; level < SCHED_MLFQ_LEVELS; ++level) {
      TAILQ_INIT(&mlfq_runqueues[worker].queues[level]);
    }
  }
}

static void sched_mlfq_on_submit(struct task* task, struct worker* worker) {
  task->mlfq_level = 0;
  task->mlfq_ticks = 0;
  struct mlfq_runqueue* rq = worker == NULL ? sched_mlfq_pick_rq() : sched_mlfq_rq(worker);
  spinlock_lock(&rq->lock);
  TAILQ_INSERT_TAIL(&rq->queues[task->mlfq_level], task, mlfq_entry);
  spinlock_unlock(&rq->lock);
}

static struct task* sched_mlfq_dequeue(struct worker* worker) {
  assert(worker != NULL);
  struct mlfq_runqueue* rq = sched_mlfq_rq(worker);

  spinlock_lock(&rq->lock);
  for (size_t level = 0; level < SCHED_MLFQ_LEVELS; ++level) {
    struct task* task = TAILQ_FIRST(&rq->queues[level]);
    if (task != NULL) {
      TAILQ_REMOVE(&rq->queues[level], task, mlfq_entry);
      spinlock_unlock(&rq->lock);
      return task;
    }
  }
  spinlock_unlock(&rq->lock);

  for (size_t attempt = 0; attempt < mlfq_workers; ++attempt) {
    size_t victim_index = (worker->index + attempt + 1) % mlfq_workers;
    if (victim_index == worker->index) {
      continue;
    }

    struct mlfq_runqueue* victim = &mlfq_runqueues[victim_index];
    if (!spinlock_try_lock(&victim->lock)) {
      continue;
    }

    for (size_t level = SCHED_MLFQ_LEVELS; level-- > 0;) {
      struct task* task = TAILQ_LAST(&victim->queues[level], task_queue);
      if (task != NULL) {
        TAILQ_REMOVE(&victim->queues[level], task, mlfq_entry);
        spinlock_unlock(&victim->lock);
        return task;
      }
    }

    spinlock_unlock(&victim->lock);
  }

  return NULL;
}

static void sched_mlfq_requeue(struct task* task, struct worker* worker) {
  assert(worker != NULL);
  struct mlfq_runqueue* rq = sched_mlfq_rq(worker);
  spinlock_lock(&rq->lock);
  TAILQ_INSERT_TAIL(&rq->queues[task->mlfq_level], task, mlfq_entry);
  spinlock_unlock(&rq->lock);
}

static void sched_mlfq_on_yield(struct task* task, struct worker* worker) {
  task->mlfq_ticks += 1;
  const size_t quantum = mlfq_quantums[task->mlfq_level];
  if (task->mlfq_ticks >= quantum) {
    task->mlfq_ticks = 0;
    if (task->mlfq_level + 1 < SCHED_MLFQ_LEVELS) {
      task->mlfq_level += 1;
    }
  }
  sched_mlfq_requeue(task, worker);
}

static void sched_mlfq_on_finish(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static void sched_mlfq_destroy() {
  free(mlfq_runqueues);
  mlfq_runqueues = NULL;
  mlfq_workers = 0;
}

static const struct sched_policy_ops sched_mlfq_ops_instance = {
    .init = sched_mlfq_init,
    .on_submit = sched_mlfq_on_submit,
    .dequeue = sched_mlfq_dequeue,
    .requeue = sched_mlfq_requeue,
    .on_yield = sched_mlfq_on_yield,
    .on_finish = sched_mlfq_on_finish,
    .destroy = sched_mlfq_destroy,
};

const struct sched_policy_ops* sched_mlfq_ops() {
  return &sched_mlfq_ops_instance;
}
