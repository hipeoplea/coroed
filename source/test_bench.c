#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "coroed/api/sleep.h"
#include "coroed/api/task.h"

enum {
  BENCH_CPU_TASKS = 48,
  BENCH_IO_TASKS = 48,
  BENCH_MIX_TASKS = 32,
  BENCH_CPU_ITERS = 200000,
  BENCH_MIX_ITERS = 80000,
  BENCH_IO_LOOPS = 64,
  BENCH_MIX_LOOPS = 16,
  BENCH_YIELD_EVERY = 256,
  BENCH_SLEEP_MS = 1,
};

static atomic_uint_least64_t bench_sink = 0;

TASK_DEFINE(bench_cpu, void, argument) {
  uint64_t iters = (uint64_t)argument;
  uint64_t acc = 1;
  for (uint64_t i = 0; i < iters; ++i) {
    acc = acc * 1664525u + 1013904223u;
    if ((i % BENCH_YIELD_EVERY) == 0) {
      YIELD;
    }
  }
  atomic_fetch_add(&bench_sink, acc);
}

TASK_DEFINE(bench_io, void, argument) {
  uint64_t loops = (uint64_t)argument;
  for (uint64_t i = 0; i < loops; ++i) {
    SLEEP(BENCH_SLEEP_MS);
  }
  atomic_fetch_add(&bench_sink, loops);
}

TASK_DEFINE(bench_mix, void, argument) {
  uint64_t loops = (uint64_t)argument;
  for (uint64_t i = 0; i < loops; ++i) {
    uint64_t acc = 1;
    for (uint64_t j = 0; j < BENCH_MIX_ITERS; ++j) {
      acc = acc * 22695477u + 1u;
      if ((j % BENCH_YIELD_EVERY) == 0) {
        YIELD;
      }
    }
    atomic_fetch_add(&bench_sink, acc);
    SLEEP(BENCH_SLEEP_MS);
  }
}

void test_bench() {
  tasks_init(tasks_policy());

  for (size_t i = 0; i < BENCH_CPU_TASKS; ++i) {
    tasks_submit(bench_cpu, (void*)BENCH_CPU_ITERS);
  }

  for (size_t i = 0; i < BENCH_IO_TASKS; ++i) {
    tasks_submit(bench_io, (void*)BENCH_IO_LOOPS);
  }

  for (size_t i = 0; i < BENCH_MIX_TASKS; ++i) {
    tasks_submit(bench_mix, (void*)BENCH_MIX_LOOPS);
  }

  tasks_start();
  tasks_wait();
  tasks_print_statistics();
  tasks_destroy();
}
