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

static int cfs_task_cmp(struct task* left, struct task* right) {
}

RB_PROTOTYPE_STATIC(cfs_tree, task, cfs_entry, cfs_task_cmp);
RB_GENERATE_STATIC(cfs_tree, task, cfs_entry, cfs_task_cmp);

static void sched_cfs_init() {
}

static void sched_cfs_on_submit(struct task* task) {
}

static struct task* sched_cfs_dequeue() {
}

static void sched_cfs_requeue(struct task* task) {
}

static void sched_cfs_on_yield(struct task* task) {
}

static void sched_cfs_on_finish(struct task* task) {
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
