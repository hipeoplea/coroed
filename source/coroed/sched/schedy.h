#pragma once

#include <stddef.h>

#include "coroed/api/task.h"
#include "uthread.h"

struct task;

void sched_init(task_sched_policy policy);

task_t sched_submit(uthread_routine entry, void* argument);

void sched_start();

void sched_wait();

void sched_print_statistics();

void sched_destroy();

size_t sched_workers_count();
