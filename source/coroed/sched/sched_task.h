#pragma once

#include <stddef.h>
#include <stdint.h>

#include "coroed/core/spinlock.h"
#include "queue.h"
#include "tree.h"
#include "uthread.h"

struct worker;

/**
 * Задача, выполняющаяся на планировщике.
 */
struct task {
  /**
   * Контекст исполнения.
   */
  struct uthread* thread;

  /**
   * Установлен при `UTHREAD_RUNNING`. Указывает на
   * рабочий поток, на котором исполняется задача.
   * Необходим для получения контекста локального
   * планировщика при операциях `yield`, `await`, `submit`.
   */
  struct worker* worker;

  enum {
    /** Готова к исполнению. */
    UTHREAD_RUNNABLE,

    /** Прямо сейчас выполняется. */
    UTHREAD_RUNNING,

    /** Завершена и скоро станет зомби. */
    UTHREAD_FINISHED,

    /** Отработала и может быть переиспользования. */
    UTHREAD_ZOMBIE,
  } state;  // Текущее состояние задачи

  /**
   * Защищает поля структуры от неупорядоченного доступа.
   */
  struct spinlock lock;

  /**
   * Поля для MLFQ.
   */
  TAILQ_ENTRY(task) mlfq_entry;
  size_t mlfq_level;
  size_t mlfq_ticks;

  /**
   * Поля для CFS.
   */
  RB_ENTRY(task) cfs_entry;
  uint64_t vruntime;
  uint64_t weight;
};
