#include "sched_mlfq.h"

#include <assert.h>

#include "coroed/core/spinlock.h"
#include "queue.h"

enum {
  SCHED_MLFQ_LEVELS = 4,
};

static const size_t mlfq_quantums[SCHED_MLFQ_LEVELS] = {1, 2, 4, 8};

TAILQ_HEAD(task_queue, task);

static struct spinlock mlfq_lock;
static struct task_queue mlfq_queues[SCHED_MLFQ_LEVELS];

static void sched_mlfq_init() {
  
}

static void sched_mlfq_on_submit(struct task* task) {
  
}

static struct task* sched_mlfq_dequeue() {
  
}

static void sched_mlfq_requeue(struct task* task) {
  
}

static void sched_mlfq_on_yield(struct task* task) {
  
}

static void sched_mlfq_on_finish(struct task* task) {
  
}

static void sched_mlfq_destroy() {
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
