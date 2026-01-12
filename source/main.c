#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coroed/api/log.h"
#include "coroed/api/task.h"

struct test {
  const char* name;
  void (*rountine)();
};

void test_counter();
void test_event();
void test_print();

static task_sched_policy parse_policy(const char* name) {
  if (strcmp(name, "rr") == 0) {
    return TASK_SCHED_RR;
  }
  if (strcmp(name, "mlfq") == 0) {
    return TASK_SCHED_MLFQ;
  }
  if (strcmp(name, "cfs") == 0) {
    return TASK_SCHED_CFS;
  }

  fprintf(stderr, "Unknown policy '%s', expected rr|mlfq|cfs\n", name);
  exit(1);
}

int main(int argc, char** argv) {
  log_init();

  task_sched_policy policy = TASK_SCHED_RR;
  if (argc > 1) {
    policy = parse_policy(argv[1]);
  }
  tasks_set_default_policy(policy);

  struct test tests[] = {
      {"counter", test_counter},
      {  "event",   test_event},
      {  "print",   test_print},
      {     NULL,         NULL},
  };

  for (struct test* test = tests; test->name != NULL; ++test) {
    printf("Running test '%s' with policy '%s'... ",
           test->name,
           policy == TASK_SCHED_RR    ? "rr"
           : policy == TASK_SCHED_MLFQ ? "mlfq"
                                       : "cfs");
    test->rountine();
    printf("Passed!\n");
  }

  return 0;
}
