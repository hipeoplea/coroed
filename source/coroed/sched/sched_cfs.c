#include "sched_cfs.h"

#include "coroed/core/spinlock.h"
#include "tree.h"

enum {
  CFS_WEIGHT_DEFAULT = 1024,
  CFS_VRUNTIME_DELTA = 1,
};

RB_HEAD(cfs_tree, task);

static struct spinlock cfs_lock;
static struct cfs_tree cfs_runqueue;
static uint64_t cfs_min_vruntime = 0;

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

static void sched_cfs_init() {
  spinlock_init(&cfs_lock);
  RB_INIT(&cfs_runqueue);
  cfs_min_vruntime = 0;
}

static void sched_cfs_on_submit(struct task* task, struct worker* worker) {
  (void)worker;
  if (task->weight == 0) {
    task->weight = CFS_WEIGHT_DEFAULT;
  }
  task->vruntime = cfs_min_vruntime;
  spinlock_lock(&cfs_lock);
  cfs_tree_RB_INSERT(&cfs_runqueue, task);
  spinlock_unlock(&cfs_lock);
}

static struct task* sched_cfs_dequeue(struct worker* worker) {
  (void)worker;
  spinlock_lock(&cfs_lock);
  struct task* task = RB_MIN(cfs_tree, &cfs_runqueue);
  if (task == NULL) {
    spinlock_unlock(&cfs_lock);
    return NULL;
  }
  cfs_tree_RB_REMOVE(&cfs_runqueue, task);
  struct task* next_min = RB_MIN(cfs_tree, &cfs_runqueue);
  if (next_min != NULL) {
    cfs_min_vruntime = next_min->vruntime;
  } else {
    cfs_min_vruntime = task->vruntime;
  }
  spinlock_unlock(&cfs_lock);
  return task;
}

static void sched_cfs_requeue(struct task* task, struct worker* worker) {
  (void)worker;
  spinlock_lock(&cfs_lock);
  if (task->vruntime < cfs_min_vruntime) {
    task->vruntime = cfs_min_vruntime;
  }
  cfs_tree_RB_INSERT(&cfs_runqueue, task);
  spinlock_unlock(&cfs_lock);
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
