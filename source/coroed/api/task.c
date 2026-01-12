#include "task.h"

#include "coroed/sched/schedy.h"

void tasks_init(task_sched_policy policy) {
  sched_init(policy);
}

void tasks_submit(task_body body, void* argument) {
  sched_submit(body, argument);
}

void tasks_start() {
  sched_start();
}

void tasks_wait() {
  sched_wait();
}

void tasks_print_statistics() {
  sched_print_statistics();
}

void tasks_destroy() {
  sched_destroy();
}
