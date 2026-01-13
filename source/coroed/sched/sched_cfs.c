#include "sched_cfs.h"

#include <assert.h>
#include <stdlib.h>

#include "coroed/core/spinlock.h"
#include "sched_worker.h"
#include "schedy.h"
#include "tree.h"

enum {
  CFS_WEIGHT_DEFAULT = 1024,
  CFS_VRUNTIME_DELTA = 1,
};

RB_HEAD(cfs_tree, task);

struct cfs_runqueue {
  struct spinlock lock;
  struct cfs_tree runqueue;
  uint64_t min_vruntime;
};

static struct cfs_runqueue* cfs_runqueues = NULL;
static size_t cfs_workers = 0;
static struct spinlock cfs_submit_lock;
static size_t cfs_next_worker = 0;

static uint64_t cfs_vruntime_delta(const struct task* task) {
  if (task->weight == 0) {
    return CFS_VRUNTIME_DELTA;
  }
  uint64_t delta = (CFS_VRUNTIME_DELTA * CFS_WEIGHT_DEFAULT) / task->weight;
  return delta == 0 ? 1 : delta;
}

static int cfs_task_cmp(struct task* left, struct task* right) {
  if (left->vruntime < right->vruntime) {
    return -1;
  }
  if (left->vruntime > right->vruntime) {
    return 1;
  }
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

RB_PROTOTYPE_STATIC(cfs_tree, task, cfs_entry, cfs_task_cmp);
RB_GENERATE_STATIC(cfs_tree, task, cfs_entry, cfs_task_cmp);

static struct cfs_runqueue* sched_cfs_rq(struct worker* worker) {
  assert(worker->index < cfs_workers);
  return &cfs_runqueues[worker->index];
}

static struct cfs_runqueue* sched_cfs_pick_rq() {
  spinlock_lock(&cfs_submit_lock);
  size_t index = cfs_next_worker;
  cfs_next_worker = (cfs_next_worker + 1) % cfs_workers;
  spinlock_unlock(&cfs_submit_lock);
  return &cfs_runqueues[index];
}

static void sched_cfs_init() {
  cfs_workers = sched_workers_count();
  cfs_runqueues = calloc(cfs_workers, sizeof(*cfs_runqueues));
  assert(cfs_runqueues != NULL);
  spinlock_init(&cfs_submit_lock);
  cfs_next_worker = 0;
  for (size_t i = 0; i < cfs_workers; ++i) {
    spinlock_init(&cfs_runqueues[i].lock);
    RB_INIT(&cfs_runqueues[i].runqueue);
    cfs_runqueues[i].min_vruntime = 0;
  }
}

static void sched_cfs_on_submit(struct task* task, struct worker* worker) {
  struct cfs_runqueue* rq = worker == NULL ? sched_cfs_pick_rq() : sched_cfs_rq(worker);
  if (task->weight == 0) {
    task->weight = CFS_WEIGHT_DEFAULT;
  }
  task->vruntime = rq->min_vruntime;
  spinlock_lock(&rq->lock);
  cfs_tree_RB_INSERT(&rq->runqueue, task);
  spinlock_unlock(&rq->lock);
}

static struct task* sched_cfs_dequeue(struct worker* worker) {
  struct cfs_runqueue* rq = sched_cfs_rq(worker);
  spinlock_lock(&rq->lock);
  struct task* task = RB_MIN(cfs_tree, &rq->runqueue);
  if (task != NULL) {
    cfs_tree_RB_REMOVE(&rq->runqueue, task);
    struct task* next_min = RB_MIN(cfs_tree, &rq->runqueue);
    rq->min_vruntime = next_min != NULL ? next_min->vruntime : task->vruntime;
    spinlock_unlock(&rq->lock);
    return task;
  }
  spinlock_unlock(&rq->lock);

  for (size_t attempt = 0; attempt < cfs_workers; ++attempt) {
    size_t victim_index = (worker->index + attempt + 1) % cfs_workers;
    struct cfs_runqueue* victim = &cfs_runqueues[victim_index];
    spinlock_lock(&victim->lock);
    task = RB_MIN(cfs_tree, &victim->runqueue);
    if (task == NULL) {
      spinlock_unlock(&victim->lock);
      continue;
    }
    cfs_tree_RB_REMOVE(&victim->runqueue, task);
    struct task* next_min = RB_MIN(cfs_tree, &victim->runqueue);
    victim->min_vruntime = next_min != NULL ? next_min->vruntime : task->vruntime;
    spinlock_unlock(&victim->lock);
    return task;
  }

  return NULL;
}

static void sched_cfs_requeue(struct task* task, struct worker* worker) {
  struct cfs_runqueue* rq = sched_cfs_rq(worker);
  spinlock_lock(&rq->lock);
  if (task->vruntime < rq->min_vruntime) {
    task->vruntime = rq->min_vruntime;
  }
  cfs_tree_RB_INSERT(&rq->runqueue, task);
  spinlock_unlock(&rq->lock);
}

static void sched_cfs_on_yield(struct task* task, struct worker* worker) {
  task->vruntime += cfs_vruntime_delta(task);
  sched_cfs_requeue(task, worker);
}

static void sched_cfs_on_finish(struct task* task, struct worker* worker) {
  (void)task;
  (void)worker;
}

static void sched_cfs_destroy() {
  free(cfs_runqueues);
  cfs_runqueues = NULL;
  cfs_workers = 0;
}

static const struct sched_policy_ops sched_cfs_ops_instance = {
    .init = sched_cfs_init,
    .on_submit = sched_cfs_on_submit,
    .dequeue = sched_cfs_dequeue,
    .requeue = sched_cfs_requeue,
    .on_yield = sched_cfs_on_yield,
    .on_finish = sched_cfs_on_finish,
    .destroy = sched_cfs_destroy,
};

const struct sched_policy_ops* sched_cfs_ops() {
  return &sched_cfs_ops_instance;
}
