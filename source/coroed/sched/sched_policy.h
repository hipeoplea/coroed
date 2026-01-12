#pragma once

#include "coroed/api/task.h"
#include "sched_task.h"

struct sched_policy_ops {
  void (*init)();
  void (*on_submit)(struct task* task, struct worker* worker);
  struct task* (*dequeue)(struct worker* worker);
  void (*requeue)(struct task* task, struct worker* worker);
  void (*on_yield)(struct task* task, struct worker* worker);
  void (*on_finish)(struct task* task, struct worker* worker);
  void (*destroy)();
};

const struct sched_policy_ops* sched_policy_get(task_sched_policy policy);
