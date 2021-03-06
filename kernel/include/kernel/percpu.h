// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <arch/ops.h>
#include <kernel/stats.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <list.h>
#include <sys/types.h>
#include <zircon/compiler.h>

__BEGIN_CDECLS

struct percpu {
    /* per cpu timer queue */
    struct list_node timer_queue;

    /* per cpu preemption timer */
    timer_t preempt_timer;

    /* per cpu run queue and bitmap to indicate which queues are non empty */
    struct list_node run_queue[NUM_PRIORITIES];
    uint32_t run_queue_bitmap;

    /* timestamp of the last reschedule IPI sent to this cpu */
    /* 0 means no pending IPI */
    zx_time_t ipi_timestamp;

    /* thread/cpu level statistics */
    struct cpu_stats stats;

    /* per cpu idle thread */
    thread_t idle_thread;
} __CPU_ALIGN;

/* the kernel per-cpu structure */
extern struct percpu percpu[SMP_MAX_CPUS];

/* make sure the bitmap is large enough to cover our number of priorities */
static_assert(NUM_PRIORITIES <= sizeof(percpu[0].run_queue_bitmap) * CHAR_BIT, "");

static inline struct percpu* get_local_percpu(void) {
    return &percpu[arch_curr_cpu_num()];
}

__END_CDECLS
