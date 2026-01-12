#include "sched_policy.h"

#include <assert.h>

#include "sched_cfs.h"
#include "sched_mlfq.h"
#include "sched_rr.h"

const struct sched_policy_ops* sched_policy_get(task_sched_policy policy) {
  switch (policy) {
    case TASK_SCHED_RR:
      return sched_rr_ops();
    case TASK_SCHED_MLFQ:
      return sched_mlfq_ops();
    case TASK_SCHED_CFS:
      return sched_cfs_ops();
  }

  assert(false && "Unknown scheduling policy");
  return NULL;
}
