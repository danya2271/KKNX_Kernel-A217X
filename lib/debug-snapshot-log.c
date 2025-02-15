/*
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Debug-SnapShot: Debug Framework for Ramdump based debugging method
 * The original code is Exynos-Snapshot for Exynos SoC
 *
 * Author: Hosung Kim <hosung0.kim@samsung.com>
 * Author: Changki Kim <changki.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/kallsyms.h>
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/pstore_ram.h>
#include <linux/sched/clock.h>
#include <linux/ftrace.h>

#include "debug-snapshot-local.h"
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/hardirq.h>
#include <asm/stacktrace.h>
#include <asm/arch_timer.h>
#include <linux/debug-snapshot.h>
#include <linux/kernel_stat.h>
#include <linux/irqnr.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/nmi.h>

#include <linux/sec_debug.h>

#include <linux/sched/debug.h>
#include <linux/list_sort.h>
#include "../kernel/sched/sched.h"

struct dbg_snapshot_lastinfo {
	atomic_t freq_last_idx[DSS_FLAG_END];
	char log[DSS_NR_CPUS][SZ_1K];
	char *last_p[DSS_NR_CPUS];
};

struct dss_dumper {
	bool active;
	u32 items;
	int init_idx;
	int cur_idx;
	u32 cur_cpu;
	u32 step;
};

struct dbg_snapshot_log_item {
	int id;
	char *name;
	struct dbg_snapshot_base entry;
};

struct dbg_snapshot_log_item dss_log_items[] = {
	{DSS_LOG_TASK_ID,	DSS_LOG_TASK,		{0, 0, 0, false, false}, },
	{DSS_LOG_WORK_ID,	DSS_LOG_WORK,		{0, 0, 0, false, false}, },
	{DSS_LOG_CPUIDLE_ID,	DSS_LOG_CPUIDLE,	{0, 0, 0, false, false}, },
	{DSS_LOG_SUSPEND_ID,	DSS_LOG_SUSPEND,	{0, 0, 0, false, false}, },
	{DSS_LOG_IRQ_ID,	DSS_LOG_IRQ,		{0, 0, 0, false, false}, },
	{DSS_LOG_SPINLOCK_ID,	DSS_LOG_SPINLOCK,	{0, 0, 0, false, false}, },
	{DSS_LOG_IRQ_DISABLED_ID,DSS_LOG_IRQ_DISABLED,	{0, 0, 0, false, false}, },
	{DSS_LOG_REG_ID,	DSS_LOG_REG,		{0, 0, 0, false, false}, },
	{DSS_LOG_HRTIMER_ID,	DSS_LOG_HRTIMER,	{0, 0, 0, false, false}, },
	{DSS_LOG_CLK_ID,	DSS_LOG_CLK,		{0, 0, 0, false, false}, },
	{DSS_LOG_PMU_ID,	DSS_LOG_PMU,		{0, 0, 0, false, false}, },
	{DSS_LOG_FREQ_ID,	DSS_LOG_FREQ,		{0, 0, 0, false, false}, },
	{DSS_LOG_FREQ_MISC_ID,	DSS_LOG_FREQ_MISC,	{0, 0, 0, false, false}, },
	{DSS_LOG_DM_ID,		DSS_LOG_DM,		{0, 0, 0, false, false}, },
	{DSS_LOG_REGULATOR_ID,	DSS_LOG_REGULATOR,	{0, 0, 0, false, false}, },
	{DSS_LOG_THERMAL_ID,	DSS_LOG_THERMAL,	{0, 0, 0, false, false}, },
	{DSS_LOG_I2C_ID,	DSS_LOG_I2C,		{0, 0, 0, false, false}, },
	{DSS_LOG_SPI_ID,	DSS_LOG_SPI,		{0, 0, 0, false, false}, },
	{DSS_LOG_BINDER_ID,	DSS_LOG_BINDER,		{0, 0, 0, false, false}, },
	{DSS_LOG_ACPM_ID,	DSS_LOG_ACPM,		{0, 0, 0, false, false}, },
	{DSS_LOG_PRINTK_ID,	DSS_LOG_PRINTK,		{0, 0, 0, false, false}, },
	{DSS_LOG_PRINTKL_ID,	DSS_LOG_PRINTKL,	{0, 0, 0, false, false}, },
};

struct dbg_snapshot_log_misc {
	atomic_t task_log_idx[DSS_NR_CPUS];
	atomic_t work_log_idx[DSS_NR_CPUS];
	atomic_t cpuidle_log_idx[DSS_NR_CPUS];
	atomic_t suspend_log_idx;
	atomic_t irq_log_idx[DSS_NR_CPUS];
#ifdef CONFIG_DEBUG_SNAPSHOT_SPINLOCK
	atomic_t spinlock_log_idx[DSS_NR_CPUS];
#endif
#ifdef CONFIG_DEBUG_SNAPSHOT_IRQ_DISABLED
	atomic_t irqs_disabled_log_idx[DSS_NR_CPUS];
#endif
#ifdef CONFIG_DEBUG_SNAPSHOT_REG
	atomic_t reg_log_idx[DSS_NR_CPUS];
#endif
	atomic_t hrtimer_log_idx[DSS_NR_CPUS];
	atomic_t clk_log_idx;
	atomic_t pmu_log_idx;
	atomic_t freq_log_idx;
	atomic_t freq_misc_log_idx;
	atomic_t dm_log_idx;
	atomic_t regulator_log_idx;
	atomic_t thermal_log_idx;
	atomic_t i2c_log_idx;
	atomic_t spi_log_idx;
#ifdef CONFIG_DEBUG_SNAPSHOT_BINDER
	atomic_t binder_log_idx;
#endif
	atomic_t printkl_log_idx;
	atomic_t printk_log_idx;
	atomic_t acpm_log_idx;
};

int dbg_snapshot_log_size = sizeof(struct dbg_snapshot_log);
/*
 *  including or excluding options
 *  if you want to except some interrupt, it should be written in this array
 */
int dss_irqlog_exlist[DSS_EX_MAX_NUM] = {
/*  interrupt number ex) 152, 153, 154, */
	-1,
};

static char *dss_freq_name[] = {
	"LIT", "BIG", "INT", "MIF", "CAM", "DISP", "INTCAM", "AUD", "MFC", "NPU", "DSP", "TNR", "DNC", "G3D",
};

/*  Internal interface variable */
static struct dbg_snapshot_log_misc dss_log_misc;
static struct dbg_snapshot_lastinfo dss_lastinfo;

void __init dbg_snapshot_init_log_idx(void)
{
	int i;

	atomic_set(&(dss_log_misc.printk_log_idx), -1);
	atomic_set(&(dss_log_misc.printkl_log_idx), -1);
	atomic_set(&(dss_log_misc.regulator_log_idx), -1);
	atomic_set(&(dss_log_misc.thermal_log_idx), -1);
	atomic_set(&(dss_log_misc.freq_log_idx), -1);
	atomic_set(&(dss_log_misc.freq_misc_log_idx), -1);
	atomic_set(&(dss_log_misc.dm_log_idx), -1);
	atomic_set(&(dss_log_misc.clk_log_idx), -1);
	atomic_set(&(dss_log_misc.pmu_log_idx), -1);
	atomic_set(&(dss_log_misc.acpm_log_idx), -1);
	atomic_set(&(dss_log_misc.i2c_log_idx), -1);
	atomic_set(&(dss_log_misc.spi_log_idx), -1);
#ifdef CONFIG_DEBUG_SNAPSHOT_BINDER
	atomic_set(&(dss_log_misc.binder_log_idx), -1);
#endif
	atomic_set(&(dss_log_misc.suspend_log_idx), -1);

	for (i = 0; i < DSS_NR_CPUS; i++) {
		atomic_set(&(dss_log_misc.task_log_idx[i]), -1);
		atomic_set(&(dss_log_misc.work_log_idx[i]), -1);
		atomic_set(&(dss_log_misc.cpuidle_log_idx[i]), -1);
		atomic_set(&(dss_log_misc.irq_log_idx[i]), -1);
#ifdef CONFIG_DEBUG_SNAPSHOT_SPINLOCK
		atomic_set(&(dss_log_misc.spinlock_log_idx[i]), -1);
#endif
#ifdef CONFIG_DEBUG_SNAPSHOT_IRQ_DISABLED
		atomic_set(&(dss_log_misc.irqs_disabled_log_idx[i]), -1);
#endif
#ifdef CONFIG_DEBUG_SNAPSHOT_REG
		atomic_set(&(dss_log_misc.reg_log_idx[i]), -1);
#endif
		atomic_set(&(dss_log_misc.hrtimer_log_idx[i]), -1);
	}
}

unsigned long secdbg_base_get_kevent_index_addr(int type)
{
	switch (type) {
	case DSS_KEVENT_TASK:
		return virt_to_phys(&(dss_log_misc.task_log_idx[0]));

	case DSS_KEVENT_WORK:
		return virt_to_phys(&(dss_log_misc.work_log_idx[0]));

	case DSS_KEVENT_IRQ:
		return virt_to_phys(&(dss_log_misc.irq_log_idx[0]));

	case DSS_KEVENT_FREQ:
		return virt_to_phys(&(dss_log_misc.freq_log_idx));

	case DSS_KEVENT_IDLE:
		return virt_to_phys(&(dss_log_misc.cpuidle_log_idx[0]));

	case DSS_KEVENT_THRM:
		return virt_to_phys(&(dss_log_misc.thermal_log_idx));

	case DSS_KEVENT_ACPM:
		return virt_to_phys(&(dss_log_misc.acpm_log_idx));

	default:
		return 0;
	}
}

void __init dbg_snapshot_early_init_log_enabled(const char *name, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item;
	int i;

	if (!item->entry.enabled || !name)
		return;

	for (i = 0; i < (int)ARRAY_SIZE(dss_log_items); i++) {
		if (!strncmp(dss_log_items[i].name, name, strlen(name))) {
			log_item = &dss_log_items[i];
			log_item->entry.enabled = en;
			pr_info("debug-snapshot: log item - %s is %sabled\n",
					name, en ? "en" : "dis");
			break;
		}
	}
}

bool dbg_snapshot_dumper_one(void *v_dumper, char *line, size_t size, size_t *len)
{
	bool ret = false;
	int idx, array_size;
	unsigned int cpu, items;
	unsigned long rem_nsec;
	u64 ts;
	struct dss_dumper *dumper = (struct dss_dumper *)v_dumper;

	if (!line || size < SZ_128 ||
		dumper->cur_cpu >= NR_CPUS)
		goto out;

	if (dumper->active) {
		if (dumper->init_idx == dumper->cur_idx)
			goto out;
	}

	cpu = dumper->cur_cpu;
	idx = dumper->cur_idx;
	items = dumper->items;

	switch(items) {
	case DSS_LOG_TASK_ID:
	{
		struct task_struct *task;
		array_size = ARRAY_SIZE(dss_log->task[0]) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.task_log_idx[0]) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->task[cpu][idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);
		task = dss_log->task[cpu][idx].task;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] task_name:%16s,  "
					    "task:0x%16p,  stack:0x%16p,  exec_start:%16llu\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						task->comm, task, task->stack,
						task->se.exec_start);
		break;
	}
	case DSS_LOG_WORK_ID:
	{
		char work_fn[KSYM_NAME_LEN] = {0,};
		char *task_comm;
		int en;

		array_size = ARRAY_SIZE(dss_log->work[0]) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.work_log_idx[0]) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->work[cpu][idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);
		lookup_symbol_name((unsigned long)dss_log->work[cpu][idx].fn, work_fn);
		task_comm = dss_log->work[cpu][idx].task_comm;
		en = dss_log->work[cpu][idx].en;

		dumper->step = 6;
		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] task_name:%16s,  work_fn:%32s,  %3s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						task_comm, work_fn,
						en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
	case DSS_LOG_CPUIDLE_ID:
	{
		unsigned int delta;
		int state, num_cpus, en;
		char *index;

		array_size = ARRAY_SIZE(dss_log->cpuidle[0]) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.cpuidle_log_idx[0]) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->cpuidle[cpu][idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		index = dss_log->cpuidle[cpu][idx].modes;
		en = dss_log->cpuidle[cpu][idx].en;
		state = dss_log->cpuidle[cpu][idx].state;
		num_cpus = dss_log->cpuidle[cpu][idx].num_online_cpus;
		delta = dss_log->cpuidle[cpu][idx].delta;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] cpuidle: %s,  "
					    "state:%d,  num_online_cpus:%d,  stay_time:%8u,  %3s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						index, state, num_cpus, delta,
						en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
	case DSS_LOG_SUSPEND_ID:
	{
		char suspend_fn[KSYM_NAME_LEN];
		int en;

		array_size = ARRAY_SIZE(dss_log->suspend) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.suspend_log_idx) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->suspend[idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		lookup_symbol_name((unsigned long)dss_log->suspend[idx].fn, suspend_fn);
		en = dss_log->suspend[idx].en;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] suspend_fn:%s,  %3s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						suspend_fn, en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
	case DSS_LOG_IRQ_ID:
	{
		char irq_fn[KSYM_NAME_LEN];
		int en, irq;

		array_size = ARRAY_SIZE(dss_log->irq[0]) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.irq_log_idx[0]) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->irq[cpu][idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		lookup_symbol_name((unsigned long)dss_log->irq[cpu][idx].fn, irq_fn);
		irq = dss_log->irq[cpu][idx].irq;
		en = dss_log->irq[cpu][idx].en;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] irq:%6d,  irq_fn:%32s,  %3s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						irq, irq_fn, en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
#ifdef CONFIG_DEBUG_SNAPSHOT_SPINLOCK
	case DSS_LOG_SPINLOCK_ID:
	{
		unsigned int jiffies_local;
		char callstack[CONFIG_DEBUG_SNAPSHOT_CALLSTACK][KSYM_NAME_LEN];
		int en, i;
		u16 locked_pending, tail;

		array_size = ARRAY_SIZE(dss_log->spinlock[0]) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.spinlock_log_idx[0]) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->spinlock[cpu][idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		jiffies_local = dss_log->spinlock[cpu][idx].jiffies;
		en = dss_log->spinlock[cpu][idx].en;
		for (i = 0; i < CONFIG_DEBUG_SNAPSHOT_CALLSTACK; i++)
			lookup_symbol_name((unsigned long)dss_log->spinlock[cpu][idx].caller[i],
						callstack[i]);

		locked_pending = dss_log->spinlock[cpu][idx].locked_pending;
		tail = dss_log->spinlock[cpu][idx].tail;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] locked_pending:%8x,  tail:%8x  jiffies:%12u,  %3s\n"
					    "callstack: %s\n"
					    "           %s\n"
					    "           %s\n"
					    "           %s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						locked_pending, tail, jiffies_local,
						en == DSS_FLAG_IN ? "IN" : "OUT",
						callstack[0], callstack[1], callstack[2], callstack[3]);
		break;
	}
#endif
	case DSS_LOG_CLK_ID:
	{
		const char *clk_name;
		char clk_fn[KSYM_NAME_LEN];
		struct clk_hw *clk;
		int en;

		array_size = ARRAY_SIZE(dss_log->clk) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.clk_log_idx) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->clk[idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		clk = (struct clk_hw *)dss_log->clk[idx].clk;
		clk_name = clk_hw_get_name(clk);
		lookup_symbol_name((unsigned long)dss_log->clk[idx].f_name, clk_fn);
		en = dss_log->clk[idx].mode;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU] clk_name:%30s,  clk_fn:%30s,  "
					    ",  %s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx,
						clk_name, clk_fn, en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
	case DSS_LOG_FREQ_ID:
	{
		char *freq_name;
		unsigned int on_cpu;
		unsigned long old_freq, target_freq;
		int en;

		array_size = ARRAY_SIZE(dss_log->freq) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.freq_log_idx) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->freq[idx].time;
		rem_nsec = do_div(ts, NSEC_PER_SEC);

		freq_name = dss_log->freq[idx].freq_name;
		old_freq = dss_log->freq[idx].old_freq;
		target_freq = dss_log->freq[idx].target_freq;
		on_cpu = dss_log->freq[idx].cpu;
		en = dss_log->freq[idx].en;

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] freq_name:%16s,  "
					    "old_freq:%16lu,  target_freq:%16lu,  %3s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, on_cpu,
						freq_name, old_freq, target_freq,
						en == DSS_FLAG_IN ? "IN" : "OUT");
		break;
	}
#ifndef CONFIG_DEBUG_SNAPSHOT_USER_MODE
	case DSS_LOG_PRINTK_ID:
	{
		char *log;
		char callstack[CONFIG_DEBUG_SNAPSHOT_CALLSTACK][KSYM_NAME_LEN];
		unsigned int cpu;
		int i;

		array_size = ARRAY_SIZE(dss_log->printk) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.printk_log_idx) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->printk[idx].time;
		cpu = dss_log->printk[idx].cpu;
		rem_nsec = do_div(ts, NSEC_PER_SEC);
		log = dss_log->printk[idx].log;
		for (i = 0; i < CONFIG_DEBUG_SNAPSHOT_CALLSTACK; i++)
			lookup_symbol_name((unsigned long)dss_log->printk[idx].caller[i],
						callstack[i]);

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] log:%s, callstack:%s, %s, %s, %s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						log, callstack[0], callstack[1], callstack[2], callstack[3]);
		break;
	}
	case DSS_LOG_PRINTKL_ID:
	{
		char callstack[CONFIG_DEBUG_SNAPSHOT_CALLSTACK][KSYM_NAME_LEN];
		size_t msg, val;
		unsigned int cpu;
		int i;

		array_size = ARRAY_SIZE(dss_log->printkl) - 1;
		if (!dumper->active) {
			idx = (atomic_read(&dss_log_misc.printkl_log_idx) + 1) & array_size;
			dumper->init_idx = idx;
			dumper->active = true;
		}
		ts = dss_log->printkl[idx].time;
		cpu = dss_log->printkl[idx].cpu;
		rem_nsec = do_div(ts, NSEC_PER_SEC);
		msg = dss_log->printkl[idx].msg;
		val = dss_log->printkl[idx].val;
		for (i = 0; i < CONFIG_DEBUG_SNAPSHOT_CALLSTACK; i++)
			lookup_symbol_name((unsigned long)dss_log->printkl[idx].caller[i],
						callstack[i]);

		*len = snprintf(line, size, "[%8lu.%09lu][%04d:CPU%u] msg:%zx, val:%zx, callstack: %s, %s, %s, %s\n",
						(unsigned long)ts, rem_nsec / NSEC_PER_USEC, idx, cpu,
						msg, val, callstack[0], callstack[1], callstack[2], callstack[3]);
		break;
	}
#endif
	default:
		snprintf(line, size, "unsupported inforation to dump\n");
		goto out;
	}
	if (array_size == idx)
		dumper->cur_idx = 0;
	else
		dumper->cur_idx = idx + 1;

	ret = true;
out:
	return ret;
}

#ifdef CONFIG_ARM64
static inline unsigned long pure_arch_local_irq_save(void)
{
	unsigned long flags;

	asm volatile(
		"mrs	%0, daif		// arch_local_irq_save\n"
		"msr	daifset, #2"
		: "=r" (flags)
		:
		: "memory");

	return flags;
}

static inline void pure_arch_local_irq_restore(unsigned long flags)
{
	asm volatile(
		"msr    daif, %0                // arch_local_irq_restore"
		:
		: "r" (flags)
		: "memory");
}
#else
static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;

	asm volatile(
		"	mrs	%0, cpsr	@ arch_local_irq_save\n"
		"	cpsid	i"
		: "=r" (flags) : : "memory", "cc");
	return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	asm volatile(
		"	msr	cpsr_c, %0	@ local_irq_restore"
		:
		: "r" (flags)
		: "memory", "cc");
}
#endif

void dbg_snapshot_task(int cpu, void *v_task)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_TASK_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		unsigned long i = atomic_inc_return(&dss_log_misc.task_log_idx[cpu]) &
				    (ARRAY_SIZE(dss_log->task[0]) - 1);

		dss_log->task[cpu][i].time = cpu_clock(cpu);
		dss_log->task[cpu][i].sp = (unsigned long)current_stack_pointer;
		dss_log->task[cpu][i].task = (struct task_struct *)v_task;
		dss_log->task[cpu][i].pid = (int)((struct task_struct *)v_task)->pid;
		strncpy(dss_log->task[cpu][i].task_comm,
			dss_log->task[cpu][i].task->comm,
			TASK_COMM_LEN - 1);
	}
}

void dbg_snapshot_work(void *worker, void *v_task, void *fn, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_WORK_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.work_log_idx[cpu]) &
					(ARRAY_SIZE(dss_log->work[0]) - 1);
		struct task_struct *task = (struct task_struct *)v_task;
		dss_log->work[cpu][i].time = cpu_clock(cpu);
		dss_log->work[cpu][i].sp = (unsigned long) current_stack_pointer;
		dss_log->work[cpu][i].worker = (struct worker *)worker;
		strncpy(dss_log->work[cpu][i].task_comm, task->comm, TASK_COMM_LEN - 1);
		dss_log->work[cpu][i].fn = (work_func_t)fn;
		dss_log->work[cpu][i].en = en;
	}
}

void dbg_snapshot_cpuidle(char *modes, unsigned state, int diff, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_CPUIDLE_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.cpuidle_log_idx[cpu]) &
				(ARRAY_SIZE(dss_log->cpuidle[0]) - 1);

		dss_log->cpuidle[cpu][i].time = cpu_clock(cpu);
		dss_log->cpuidle[cpu][i].modes = modes;
		dss_log->cpuidle[cpu][i].state = state;
		dss_log->cpuidle[cpu][i].sp = (unsigned long) current_stack_pointer;
		dss_log->cpuidle[cpu][i].num_online_cpus = num_online_cpus();
		dss_log->cpuidle[cpu][i].delta = diff;
		dss_log->cpuidle[cpu][i].en = en;
	}
}

void dbg_snapshot_suspend(char *log, void *fn, void *dev, int state, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_SUSPEND_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int len;
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.suspend_log_idx) &
				(ARRAY_SIZE(dss_log->suspend) - 1);

		dss_log->suspend[i].time = cpu_clock(cpu);
		dss_log->suspend[i].sp = (unsigned long) current_stack_pointer;

		if (log) {
			len = strlen(log);
			memcpy(dss_log->suspend[i].log, log,
					len < DSS_LOG_GEN_LEN ?
					len : DSS_LOG_GEN_LEN - 1);
		} else {
			memset(dss_log->suspend[i].log, 0, DSS_LOG_GEN_LEN - 1);
		}

		dss_log->suspend[i].fn = fn;
		dss_log->suspend[i].dev = (struct device *)dev;
		dss_log->suspend[i].core = cpu;
		dss_log->suspend[i].en = en;
	}
}

static void dbg_snapshot_print_calltrace(void)
{
	int i;

	dev_info(dss_desc.dev, "\n<Call trace>\n");
	for (i = 0; i < DSS_NR_CPUS; i++) {
		dev_info(dss_desc.dev, "CPU ID: %d -----------------------------------------------\n", i);
		dev_info(dss_desc.dev, "\n%s", dss_lastinfo.log[i]);
	}
}

void dbg_snapshot_save_log(int cpu, unsigned long where)
{
	if (dss_lastinfo.last_p[cpu] == NULL)
		dss_lastinfo.last_p[cpu] = &dss_lastinfo.log[cpu][0];

	if (dss_lastinfo.last_p[cpu] > &dss_lastinfo.log[cpu][SZ_1K - SZ_128])
		return;

	*(unsigned long *)&(dss_lastinfo.last_p[cpu]) += sprintf(dss_lastinfo.last_p[cpu],
			"[<%p>] %pS\n", (void *)where, (void *)where);

}

void dbg_snapshot_regulator(unsigned long long timestamp, char* f_name, unsigned int addr, unsigned int volt, unsigned int rvolt, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_REGULATOR_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.regulator_log_idx) &
				(ARRAY_SIZE(dss_log->regulator) - 1);
		int size = strlen(f_name);
		if (size >= SZ_16)
			size = SZ_16 - 1;
		dss_log->regulator[i].time = cpu_clock(cpu);
		dss_log->regulator[i].cpu = cpu;
		dss_log->regulator[i].acpm_time = timestamp;
		strncpy(dss_log->regulator[i].name, f_name, size);
		dss_log->regulator[i].reg = addr;
		dss_log->regulator[i].en = en;
		dss_log->regulator[i].voltage = volt;
		dss_log->regulator[i].raw_volt = rvolt;
	}
}

void dbg_snapshot_thermal(void *data, unsigned int temp, char *name, unsigned long long max_cooling)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_THERMAL_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.thermal_log_idx) &
				(ARRAY_SIZE(dss_log->thermal) - 1);

		dss_log->thermal[i].time = cpu_clock(cpu);
		dss_log->thermal[i].cpu = cpu;
		dss_log->thermal[i].data = (struct exynos_tmu_data *)data;
		dss_log->thermal[i].temp = temp;
		dss_log->thermal[i].cooling_device = name;
		dss_log->thermal[i].cooling_state = max_cooling;
	}
}

void dbg_snapshot_irq(int irq, void *fn, void *val, unsigned long long start_time, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_IRQ_ID];
	unsigned long flags;

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;

	flags = pure_arch_local_irq_save();
	{
		int cpu = raw_smp_processor_id();
		unsigned long long time, latency;
		unsigned long i;

		time = cpu_clock(cpu);

		if (start_time == 0)
			start_time = time;

		latency = time - start_time;
		i = atomic_inc_return(&dss_log_misc.irq_log_idx[cpu]) &
				(ARRAY_SIZE(dss_log->irq[0]) - 1);

		dss_log->irq[cpu][i].time = time;
		dss_log->irq[cpu][i].sp = (unsigned long) current_stack_pointer;
		dss_log->irq[cpu][i].irq = irq;
		dss_log->irq[cpu][i].fn = (void *)fn;
		dss_log->irq[cpu][i].desc = (struct irq_desc *)val;
		dss_log->irq[cpu][i].latency = latency;
		dss_log->irq[cpu][i].en = en;
	}
	pure_arch_local_irq_restore(flags);
}

#ifdef CONFIG_DEBUG_SNAPSHOT_SPINLOCK
void dbg_snapshot_spinlock(void *v_lock, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_SPINLOCK_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long index = atomic_inc_return(&dss_log_misc.spinlock_log_idx[cpu]);
		unsigned long j, i = index & (ARRAY_SIZE(dss_log->spinlock[0]) - 1);
		raw_spinlock_t *lock = (raw_spinlock_t *)v_lock;
#ifdef CONFIG_ARM_ARCH_TIMER
		dss_log->spinlock[cpu][i].time = cpu_clock(cpu);
#else
		dss_log->spinlock[cpu][i].time = index;
#endif
		dss_log->spinlock[cpu][i].sp = (unsigned long) current_stack_pointer;
		dss_log->spinlock[cpu][i].jiffies = jiffies_64;
#ifdef CONFIG_DEBUG_SPINLOCK
		dss_log->spinlock[cpu][i].lock = lock;
		dss_log->spinlock[cpu][i].locked_pending = lock->raw_lock.locked_pending;
		dss_log->spinlock[cpu][i].tail = lock->raw_lock.tail;
#endif
		dss_log->spinlock[cpu][i].en = en;

		for (j = 0; j < dss_desc.callstack; j++) {
			dss_log->spinlock[cpu][i].caller[j] =
				(void *)((size_t)return_address(j + 1));
		}
	}
}
#endif

#ifdef CONFIG_DEBUG_SNAPSHOT_IRQ_DISABLED
void dbg_snapshot_irqs_disabled(unsigned long flags)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_IRQ_DISABLED_ID];
	int cpu = raw_smp_processor_id();

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;

	if (unlikely(flags)) {
		unsigned long j, local_flags = pure_arch_local_irq_save();

		/* If flags has one, it shows interrupt enable status */
		atomic_set(&dss_log_misc.irqs_disabled_log_idx[cpu], -1);
		dss_log->irqs_disabled[cpu][0].time = 0;
		dss_log->irqs_disabled[cpu][0].index = 0;
		dss_log->irqs_disabled[cpu][0].task = NULL;
		dss_log->irqs_disabled[cpu][0].task_comm = NULL;

		for (j = 0; j < dss_desc.callstack; j++) {
			dss_log->irqs_disabled[cpu][0].caller[j] = NULL;
		}

		pure_arch_local_irq_restore(local_flags);
	} else {
		unsigned long index = atomic_inc_return(&dss_log_misc.irqs_disabled_log_idx[cpu]);
		unsigned long j, i = index % ARRAY_SIZE(dss_log->irqs_disabled[0]);

		dss_log->irqs_disabled[cpu][0].time = jiffies_64;
		dss_log->irqs_disabled[cpu][i].index = index;
		dss_log->irqs_disabled[cpu][i].task = get_current();
		dss_log->irqs_disabled[cpu][i].task_comm = get_current()->comm;

		for (j = 0; j < dss_desc.callstack; j++) {
			dss_log->irqs_disabled[cpu][i].caller[j] =
				(void *)((size_t)return_address(j + 1));
		}
	}
}
#endif

void dbg_snapshot_clk(void *clock, const char *func_name, unsigned long arg, int mode)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_CLK_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.clk_log_idx) &
				(ARRAY_SIZE(dss_log->clk) - 1);

		dss_log->clk[i].time = cpu_clock(cpu);
		dss_log->clk[i].mode = mode;
		dss_log->clk[i].arg = arg;
		dss_log->clk[i].clk = (struct clk_hw *)clock;
		dss_log->clk[i].f_name = func_name;
	}
}

void dbg_snapshot_pmu(int id, const char *func_name, int mode)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_PMU_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.pmu_log_idx) &
				(ARRAY_SIZE(dss_log->pmu) - 1);

		dss_log->pmu[i].time = cpu_clock(cpu);
		dss_log->pmu[i].mode = mode;
		dss_log->pmu[i].id = id;
		dss_log->pmu[i].f_name = func_name;
	}
}

static struct notifier_block **dss_should_check_nl[] = {
	(struct notifier_block **)(&panic_notifier_list.head),
	(struct notifier_block **)(&reboot_notifier_list.head),
	(struct notifier_block **)(&restart_handler_list.head),
#ifdef CONFIG_PM_SLEEP
	(struct notifier_block **)(&pm_chain_head.head),
#endif
#ifdef CONFIG_EXYNOS_ITMON
	(struct notifier_block **)(&itmon_notifier_list.head),
#endif
};

void dbg_snapshot_print_notifier_call(void **nl, unsigned long func, int en)
{
	struct notifier_block **nl_org = (struct notifier_block **)nl;
	char notifier_name[KSYM_NAME_LEN];
	char notifier_func_name[KSYM_NAME_LEN];
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(dss_should_check_nl); i++) {
		if (nl_org == dss_should_check_nl[i]) {
			lookup_symbol_name((unsigned long)nl_org, notifier_name);
			lookup_symbol_name((unsigned long)func, notifier_func_name);

			dev_info(dss_desc.dev, "debug-snapshot: %s -> %s call %s\n",
				notifier_name,
				notifier_func_name,
				en == DSS_FLAG_IN ? "+" : "-");
			break;
		}
	}
}

void dbg_snapshot_freq(int type, unsigned long old_freq, unsigned long target_freq, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_FREQ_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.freq_log_idx) &
				(ARRAY_SIZE(dss_log->freq) - 1);

		if (atomic_read(&dss_log_misc.freq_log_idx) > atomic_read(&dss_lastinfo.freq_last_idx[type]))
			atomic_set(&dss_lastinfo.freq_last_idx[type], atomic_read(&dss_log_misc.freq_log_idx));

		dss_log->freq[i].time = cpu_clock(cpu);
		dss_log->freq[i].cpu = cpu;
		dss_log->freq[i].freq_name = dss_freq_name[type];
		dss_log->freq[i].freq_type = type;
		dss_log->freq[i].old_freq = old_freq;
		dss_log->freq[i].target_freq = target_freq;
		dss_log->freq[i].en = en;
#ifdef CONFIG_SEC_DEBUG_FREQ
		secdbg_freq_check(type, i, target_freq, en);
#endif
	}
}

void dbg_snapshot_freq_misc(int type, unsigned long old_freq, unsigned long target_freq, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_FREQ_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.freq_misc_log_idx) &
				(ARRAY_SIZE(dss_log->freq_misc) - 1);

		dss_log->freq_misc[i].time = cpu_clock(cpu);
		dss_log->freq_misc[i].cpu = cpu;
		dss_log->freq_misc[i].freq_name = dss_freq_name[type];
		dss_log->freq_misc[i].freq_type = type;
		dss_log->freq_misc[i].old_freq = old_freq;
		dss_log->freq_misc[i].target_freq = target_freq;
		dss_log->freq_misc[i].en = en;
	}
}

static void dbg_snapshot_get_sec(unsigned long long ts, unsigned long *sec, unsigned long *msec)
{
	*sec = ts / NSEC_PER_SEC;
	*msec = (ts % NSEC_PER_SEC) / USEC_PER_MSEC;
}

static void dbg_snapshot_print_last_irq(int cpu)
{
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_IRQ_ID];
	unsigned long idx, sec, msec;
	char fn_name[KSYM_NAME_LEN];

	if (!log_item->entry.enabled)
		return;

	idx = atomic_read(&dss_log_misc.irq_log_idx[cpu]) & (ARRAY_SIZE(dss_log->irq[0]) - 1);
	dbg_snapshot_get_sec(dss_log->irq[cpu][idx].time, &sec, &msec);
	lookup_symbol_name((unsigned long)dss_log->irq[cpu][idx].fn, fn_name);

	dev_info(dss_desc.dev, "%-16s: [%4ld] %10lu.%06lu sec, %10s: %24s, %8s: %8d, %10s: %2d, %s\n",
			">>> last irq", idx, sec, msec,
			"handler", fn_name,
			"irq", dss_log->irq[cpu][idx].irq,
			"en", dss_log->irq[cpu][idx].en,
			(dss_log->irq[cpu][idx].en == 1) ? "[Missmatch]" : "");
}

static void dbg_snapshot_print_last_task(int cpu)
{
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_TASK_ID];
	unsigned long idx, sec, msec;
	struct task_struct *task;

	if (!log_item->entry.enabled)
		return;

	idx = atomic_read(&dss_log_misc.task_log_idx[cpu]) & (ARRAY_SIZE(dss_log->task[0]) - 1);
	dbg_snapshot_get_sec(dss_log->task[cpu][idx].time, &sec, &msec);
	task = dss_log->task[cpu][idx].task;

	dev_info(dss_desc.dev, "%-16s: [%4lu] %10lu.%06lu sec, %10s: %24s, %8s: 0x%-16p, %10s: %16llu\n",
			">>> last task", idx, sec, msec,
			"task_comm", (task) ? task->comm : "NULL",
			"task", task,
			"exec_start", (task) ? task->se.exec_start : 0);
}

static void dbg_snapshot_print_last_work(int cpu)
{
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_WORK_ID];
	unsigned long idx, sec, msec;
	char fn_name[KSYM_NAME_LEN];

	if (!log_item->entry.enabled)
		return;

	idx = atomic_read(&dss_log_misc.work_log_idx[cpu]) & (ARRAY_SIZE(dss_log->work[0]) - 1);
	dbg_snapshot_get_sec(dss_log->work[cpu][idx].time, &sec, &msec);
	lookup_symbol_name((unsigned long)dss_log->work[cpu][idx].fn, fn_name);

	dev_info(dss_desc.dev, "%-16s: [%4lu] %10lu.%06lu sec, %10s: %24s, %8s: %20s, %3s: %3d %s\n",
			">>> last work", idx, sec, msec,
			"task_name", dss_log->work[cpu][idx].task_comm,
			"work_fn", fn_name,
			"en", dss_log->work[cpu][idx].en,
			(dss_log->work[cpu][idx].en == 1) ? "[Missmatch]" : "");
}

static void dbg_snapshot_print_last_cpuidle(int cpu)
{
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_CPUIDLE_ID];
	unsigned long idx, sec, msec;

	if (!log_item->entry.enabled)
		return;

	idx = atomic_read(&dss_log_misc.cpuidle_log_idx[cpu]) & (ARRAY_SIZE(dss_log->cpuidle[0]) - 1);
	dbg_snapshot_get_sec(dss_log->cpuidle[cpu][idx].time, &sec, &msec);

	dev_info(dss_desc.dev, "%-16s: [%4lu] %10lu.%06lu sec, %10s: %24d, %8s: %4s, %6s: %3d, %12s: %2d, %3s: %3d %s\n",
			">>> last cpuidle", idx, sec, msec,
			"stay time", dss_log->cpuidle[cpu][idx].delta,
			"modes", dss_log->cpuidle[cpu][idx].modes,
			"state", dss_log->cpuidle[cpu][idx].state,
			"online_cpus", dss_log->cpuidle[cpu][idx].num_online_cpus,
			"en", dss_log->cpuidle[cpu][idx].en,
			(dss_log->cpuidle[cpu][idx].en == 1) ? "[Missmatch]" : "");
}

static void dbg_snapshot_print_lastinfo(void)
{
	int cpu;

	dev_info(dss_desc.dev, "<last info>\n");
	for (cpu = 0; cpu < DSS_NR_CPUS; cpu++) {
		dev_info(dss_desc.dev, "CPU ID: %d -----------------------------------------------\n", cpu);
		dbg_snapshot_print_last_task(cpu);
		dbg_snapshot_print_last_work(cpu);
		dbg_snapshot_print_last_irq(cpu);
		dbg_snapshot_print_last_cpuidle(cpu);
	}
}

static void dbg_snapshot_print_freqinfo(void)
{
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_FREQ_ID];
	unsigned long idx, sec, msec;
	char *freq_name;
	unsigned int i;
	unsigned long old_freq, target_freq;

	if (!log_item->entry.enabled)
		return;

	dev_info(dss_desc.dev, "\n<freq info>\n");

	for (i = 0; i < DSS_FLAG_END; i++) {
		idx = atomic_read(&dss_lastinfo.freq_last_idx[i]) & (ARRAY_SIZE(dss_log->freq) - 1);
		freq_name = dss_log->freq[idx].freq_name;
		if ((!freq_name) || strncmp(freq_name, dss_freq_name[i], strlen(dss_freq_name[i]))) {
			dev_info(dss_desc.dev, "%10s: no infomation\n", dss_freq_name[i]);
			continue;
		}

		dbg_snapshot_get_sec(dss_log->freq[idx].time, &sec, &msec);
		old_freq = dss_log->freq[idx].old_freq;
		target_freq = dss_log->freq[idx].target_freq;
		dev_info(dss_desc.dev, "%10s: [%4lu] %10lu.%06lu sec, %12s: %6luMhz, %12s: %6luMhz, %3s: %3d %s\n",
					freq_name, idx, sec, msec,
					"old_freq", old_freq/1000,
					"target_freq", target_freq/1000,
					"en", dss_log->freq[idx].en,
					(dss_log->freq[idx].en == 1) ? "[Missmatch]" : "");
	}
}

#ifndef arch_irq_stat
#define arch_irq_stat() 0
#endif

static void dbg_snapshot_print_irq(void)
{
	int i, j;
	u64 sum = 0;

	for_each_possible_cpu(i) {
		sum += kstat_cpu_irqs_sum(i);
		sum += arch_irq_stat_cpu(i);
	}
	sum += arch_irq_stat();

	dev_info(dss_desc.dev, "\n<irq info>\n");
	dev_info(dss_desc.dev, "------------------------------------------------------------------\n");
	dev_info(dss_desc.dev, "\n");
	dev_info(dss_desc.dev, "sum irq : %llu", (unsigned long long)sum);
	dev_info(dss_desc.dev, "------------------------------------------------------------------\n");

	for_each_irq_nr(j) {
		unsigned int irq_stat = kstat_irqs(j);

		if (irq_stat) {
			struct irq_desc *desc = irq_to_desc(j);
			const char *name;

			name = desc->action ? (desc->action->name ? desc->action->name : "???") : "???";
			dev_info(dss_desc.dev, "irq-%-4d(hwirq-%-4d) : %8u %s\n",
				j, (int)desc->irq_data.hwirq, irq_stat, name);
		}
	}
}

void dbg_snapshot_print_panic_report(void)
{
	if (unlikely(!dss_base.enabled))
		return;

	dev_info(dss_desc.dev, "============================================================\n");
	dev_info(dss_desc.dev, "Panic Report\n");
	dev_info(dss_desc.dev, "============================================================\n");
	dbg_snapshot_print_lastinfo();
	dbg_snapshot_print_freqinfo();
	dbg_snapshot_print_calltrace();
	dbg_snapshot_print_irq();
	dev_info(dss_desc.dev, "============================================================\n");
}

void dbg_snapshot_dm(int type, unsigned long min, unsigned long max, s32 wait_t, s32 t)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_DM_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.dm_log_idx) &
				(ARRAY_SIZE(dss_log->dm) - 1);

		dss_log->dm[i].time = cpu_clock(cpu);
		dss_log->dm[i].cpu = cpu;
		dss_log->dm[i].dm_num = type;
		dss_log->dm[i].min_freq = min;
		dss_log->dm[i].max_freq = max;
		dss_log->dm[i].wait_dmt = wait_t;
		dss_log->dm[i].do_dmt = t;
	}
}

void dbg_snapshot_hrtimer(void *timer, s64 *now, void *fn, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_HRTIMER_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.hrtimer_log_idx[cpu]) &
				(ARRAY_SIZE(dss_log->hrtimers[0]) - 1);

		dss_log->hrtimers[cpu][i].time = cpu_clock(cpu);
		dss_log->hrtimers[cpu][i].now = *now;
		dss_log->hrtimers[cpu][i].timer = (struct hrtimer *)timer;
		dss_log->hrtimers[cpu][i].fn = fn;
		dss_log->hrtimers[cpu][i].en = en;
	}
}

void dbg_snapshot_i2c(struct i2c_adapter *adap, struct i2c_msg *msgs, int num, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_I2C_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.i2c_log_idx) &
				(ARRAY_SIZE(dss_log->i2c) - 1);

		dss_log->i2c[i].time = cpu_clock(cpu);
		dss_log->i2c[i].cpu = cpu;
		dss_log->i2c[i].adap = adap;
		dss_log->i2c[i].msgs = msgs;
		dss_log->i2c[i].num = num;
		dss_log->i2c[i].en = en;
	}
}

void dbg_snapshot_spi(struct spi_controller *ctlr, struct spi_message *cur_msg, int en)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_SPI_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.spi_log_idx) &
				(ARRAY_SIZE(dss_log->spi) - 1);

		dss_log->spi[i].time = cpu_clock(cpu);
		dss_log->spi[i].cpu = cpu;
		dss_log->spi[i].ctlr = ctlr;
		dss_log->spi[i].cur_msg = cur_msg;
		dss_log->spi[i].en = en;
	}
}

#ifdef CONFIG_DEBUG_SNAPSHOT_BINDER
void dbg_snapshot_binder(struct trace_binder_transaction_base *base,
			 struct trace_binder_transaction *transaction,
			 struct trace_binder_transaction_error *error)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_BINDER_ID];
	int cpu;
	unsigned long i;

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;

	if (base == NULL)
		return;

	cpu = raw_smp_processor_id();
	i = atomic_inc_return(&dss_log_misc.binder_log_idx) &
				(ARRAY_SIZE(dss_log->binder) - 1);

	dss_log->binder[i].time = cpu_clock(cpu);
	dss_log->binder[i].cpu = cpu;
	dss_log->binder[i].base = *base;

	if (transaction) {
		dss_log->binder[i].transaction = *transaction;
	} else {
		dss_log->binder[i].transaction.to_node_id = 0;
		dss_log->binder[i].transaction.reply = 0;
		dss_log->binder[i].transaction.flags = 0;
		dss_log->binder[i].transaction.code = 0;
	}
	if (error) {
		dss_log->binder[i].error = *error;
	} else {
		dss_log->binder[i].error.return_error = 0;
		dss_log->binder[i].error.return_error_param = 0;
		dss_log->binder[i].error.return_error_line = 0;
	}
}
#endif

void dbg_snapshot_acpm(unsigned long long timestamp, const char *log, unsigned int data)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_ACPM_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.acpm_log_idx) &
				(ARRAY_SIZE(dss_log->acpm) - 1);
		int len = strlen(log);

		if (len >= 8)
			len = 8;

		dss_log->acpm[i].time = cpu_clock(cpu);
		dss_log->acpm[i].acpm_time = timestamp;
		strncpy(dss_log->acpm[i].log, log, len);
		dss_log->acpm[i].log[len] = '\0';
		dss_log->acpm[i].data = data;
	}
}

#ifdef CONFIG_DEBUG_SNAPSHOT_REG
void dbg_snapshot_reg(char io_type, char data_type, void *addr)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_REG_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;

	{
		int cpu = raw_smp_processor_id();
		unsigned long i = atomic_inc_return(&dss_log_misc.reg_log_idx[cpu]) &
			(ARRAY_SIZE(dss_log->reg[0]) - 1);

		dss_log->reg[cpu][i].time = cpu_clock(cpu);
		dss_log->reg[cpu][i].io_type = io_type;
		dss_log->reg[cpu][i].data_type = data_type;
		dss_log->reg[cpu][i].addr = addr;
		dss_log->reg[cpu][i].caller = __builtin_return_address(0);
	}
}
#endif

#ifndef CONFIG_DEBUG_SNAPSHOT_USER_MODE
void dbg_snapshot_printk(const char *fmt, ...)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_PRINTK_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		va_list args;
		int ret;
		unsigned long j, i = atomic_inc_return(&dss_log_misc.printk_log_idx) &
				(ARRAY_SIZE(dss_log->printk) - 1);

		va_start(args, fmt);
		ret = vsnprintf(dss_log->printk[i].log,
				sizeof(dss_log->printk[i].log), fmt, args);
		va_end(args);

		dss_log->printk[i].time = cpu_clock(cpu);
		dss_log->printk[i].cpu = cpu;

		for (j = 0; j < dss_desc.callstack; j++) {
			dss_log->printk[i].caller[j] =
				(void *)((size_t)return_address(j));
		}
	}
}

void dbg_snapshot_printkl(size_t msg, size_t val)
{
	struct dbg_snapshot_item *item = &dss_items[DSS_ITEM_KEVENTS_ID];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_PRINTKL_ID];

	if (unlikely(!dss_base.enabled || !item->entry.enabled || !log_item->entry.enabled))
		return;
	{
		int cpu = raw_smp_processor_id();
		unsigned long j, i = atomic_inc_return(&dss_log_misc.printkl_log_idx) &
				(ARRAY_SIZE(dss_log->printkl) - 1);

		dss_log->printkl[i].time = cpu_clock(cpu);
		dss_log->printkl[i].cpu = cpu;
		dss_log->printkl[i].msg = msg;
		dss_log->printkl[i].val = val;

		for (j = 0; j < dss_desc.callstack; j++) {
			dss_log->printkl[i].caller[j] =
				(void *)((size_t)return_address(j));
		}
	}
}
#endif

#if defined(CONFIG_HARDLOCKUP_DETECTOR_OTHER_CPU) && defined(CONFIG_SEC_DEBUG_LOCKUP_INFO)
#define for_each_generated_irq_in_snapshot(idx, i, max, base, cpu)	\
	for (i = 0, idx = base; i < max; ++i, idx = (base - i) & (ARRAY_SIZE(dss_log->irq[0]) - 1))		\
		if (dss_log->irq[cpu][idx].en == DSS_FLAG_IN)

static inline void secdbg_get_busiest_irq(struct hardlockup_info *hl_info, unsigned long start_idx, int cpu)
{
	#define MAX_BUF 5
	int i, j, idx, max_count = 20;
	int buf_count = 0;
	int max_irq_idx = 0;

	struct irq_info_buf {
		unsigned int occurrences;
		int irq;
		void *fn;
		unsigned long long total_duration;
		unsigned long long last_time;
	};

	struct irq_info_buf i_buf[MAX_BUF] = {{0,},};

	for_each_generated_irq_in_snapshot(idx, i, max_count, start_idx, cpu) {
		for (j = 0; j < buf_count; j++) {
			if (i_buf[j].irq == dss_log->irq[cpu][idx].irq) {
				i_buf[j].total_duration += (i_buf[j].last_time - dss_log->irq[cpu][idx].time);
				i_buf[j].last_time = dss_log->irq[cpu][idx].time;
				i_buf[j].occurrences++;
				break;
			}
		}

		if (j == buf_count && buf_count < MAX_BUF) {
			i_buf[buf_count].irq = dss_log->irq[cpu][idx].irq;
			i_buf[buf_count].fn = dss_log->irq[cpu][idx].fn;
			i_buf[buf_count].occurrences = 0;
			i_buf[buf_count].total_duration = 0;
			i_buf[buf_count].last_time = dss_log->irq[cpu][idx].time;
			buf_count++;
		} else if (buf_count == MAX_BUF) {
			pr_info("Buffer overflow. Various irqs were generated!!\n");
		}
	}

	for (i = 1; i < buf_count; i++) {
		if (i_buf[max_irq_idx].occurrences < i_buf[i].occurrences)
			max_irq_idx = i;
	}

	hl_info->irq_info.irq = i_buf[max_irq_idx].irq;
	hl_info->irq_info.fn = i_buf[max_irq_idx].fn;
	hl_info->irq_info.avg_period = i_buf[max_irq_idx].total_duration / i_buf[max_irq_idx].occurrences;
}

void secdbg_hardlockup_get_info(unsigned int cpu,  void *info)
{
	struct hardlockup_info *hl_info = info;
	unsigned long cpuidle_idx, irq_idx, task_idx;
	unsigned long long cpuidle_delay_time, irq_delay_time, task_delay_time;
	unsigned long long curr, thresh;

	thresh = get_hardlockup_thresh();
	curr = local_clock();

	cpuidle_idx = atomic_read(&dss_log_misc.cpuidle_log_idx[cpu]) & (ARRAY_SIZE(dss_log->cpuidle[0]) - 1);
	cpuidle_delay_time = curr > dss_log->cpuidle[cpu][cpuidle_idx].time ?
		curr - dss_log->cpuidle[cpu][cpuidle_idx].time : dss_log->cpuidle[cpu][cpuidle_idx].time - curr;

	if (dss_log->cpuidle[cpu][cpuidle_idx].en == DSS_FLAG_IN
		&& cpuidle_delay_time > thresh) {
		hl_info->delay_time = cpuidle_delay_time;
		hl_info->cpuidle_info.mode = dss_log->cpuidle[cpu][cpuidle_idx].modes;
		hl_info->hl_type = HL_IDLE_STUCK;
		return;
	}

	irq_idx = atomic_read(&dss_log_misc.irq_log_idx[cpu]) & (ARRAY_SIZE(dss_log->irq[0]) - 1);
	irq_delay_time = curr > dss_log->irq[cpu][irq_idx].time ?
		curr - dss_log->irq[cpu][irq_idx].time :  dss_log->irq[cpu][irq_idx].time - curr;

	if (dss_log->irq[cpu][irq_idx].en == DSS_FLAG_IN
		&& irq_delay_time > thresh) {

		hl_info->delay_time = irq_delay_time;

		if (dss_log->irq[cpu][irq_idx].irq < 0) {				// smc calls have negative irq number
			hl_info->smc_info.cmd = dss_log->irq[cpu][irq_idx].irq;
			hl_info->hl_type = HL_SMC_CALL_STUCK;
			return;
		} else {
			hl_info->irq_info.irq = dss_log->irq[cpu][irq_idx].irq;
			hl_info->irq_info.fn = dss_log->irq[cpu][irq_idx].fn;
			hl_info->hl_type = HL_IRQ_STUCK;
			return;
		}
	}

	task_idx = atomic_read(&dss_log_misc.task_log_idx[cpu]) & (ARRAY_SIZE(dss_log->task[0]) - 1);
	task_delay_time = curr > dss_log->task[cpu][task_idx].time ?
		curr - dss_log->task[cpu][task_idx].time : dss_log->task[cpu][task_idx].time - curr;

	if (task_delay_time > thresh) {
		hl_info->delay_time = task_delay_time;
		if (irq_delay_time > thresh) {
			strncpy(hl_info->task_info.task_comm,
				dss_log->task[cpu][task_idx].task_comm,
				TASK_COMM_LEN - 1);
			hl_info->task_info.task_comm[TASK_COMM_LEN - 1] = '\0';
			hl_info->hl_type = HL_TASK_STUCK;
			return;
		} else {
			secdbg_get_busiest_irq(hl_info, irq_idx, cpu);
			hl_info->hl_type = HL_IRQ_STORM;
			return;
		}
	}

	hl_info->hl_type = HL_UNKNOWN_STUCK;
}

void secdbg_softlockup_get_info(unsigned int cpu, void *info)
{
	struct softlockup_info *sl_info = info;
	unsigned long task_idx;
	unsigned long long task_delay_time;
	unsigned long long curr, thresh;

	thresh = get_dss_softlockup_thresh();
	curr = local_clock();
	task_idx = atomic_read(&dss_log_misc.task_log_idx[cpu]) & (ARRAY_SIZE(dss_log->task[0]) - 1);
	task_delay_time = curr - dss_log->task[cpu][task_idx].time;
	sl_info->delay_time = task_delay_time;

	strncpy(sl_info->task_info.task_comm,
		dss_log->task[cpu][task_idx].task_comm,
		TASK_COMM_LEN - 1);
	sl_info->task_info.task_comm[TASK_COMM_LEN - 1] = '\0';

	if (task_delay_time > thresh)
		sl_info->sl_type = SL_TASK_STUCK;
	else
		sl_info->sl_type = SL_UNKNOWN_STUCK;
}
#endif

#ifdef CONFIG_SEC_DEBUG_COMPLETE_HINT
u64 secdbg_snapshot_get_hardlatency_info(unsigned int cpu)
{
	u64 ret = 0;
	unsigned long irq_idx;
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_IRQ_ID];

	if (unlikely(!log_item->entry.enabled))
		return ret;

	irq_idx = atomic_read(&dss_log_misc.irq_log_idx[cpu]) & (ARRAY_SIZE(dss_log->irq[0]) - 1);
	if (dss_log->irq[cpu][irq_idx].en == DSS_FLAG_IN) {
		ret = (u64)dss_log->irq[cpu][irq_idx].fn;
	}
	return ret;
}
#endif

#ifdef CONFIG_SEC_DEBUG_WQ_LOCKUP_INFO
struct busy_info {
	struct list_head node;
	unsigned long long residency;
	struct task_struct *tsk;
	pid_t pid;
};

static LIST_HEAD(busy_info_list);
static int is_busy;
static bool is_busy_info_list;
static unsigned long long real_duration;

struct list_head *create_busy_info(struct task_struct *tsk, unsigned long long residency)
{
	struct busy_info *info;

	info = kzalloc(sizeof(struct busy_info), GFP_ATOMIC);
	if (!info)
		return NULL;

	info->pid = tsk->pid;
	if (info->pid == 0)
		is_busy = 0;

	info->tsk = tsk;
	info->residency = residency;

	return &info->node;
}

static int residency_cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct busy_info *busy_info_a;
	struct busy_info *busy_info_b;

	busy_info_a = container_of(a, struct busy_info, node);
	busy_info_b = container_of(b, struct busy_info, node);

	if (busy_info_a->residency < busy_info_b->residency)
		return 1;
	else if (busy_info_a->residency > busy_info_b->residency)
		return -1;
	else
		return 0;
}

static void show_callstack(void *dummy)
{
#ifdef CONFIG_SEC_DEBUG_AUTO_COMMENT
	show_stack_auto_comment(NULL, NULL);
#else
	show_stack(NULL, NULL);
#endif
}

#define LOG_LINE_MAX 512

void secdbg_show_sched_info(unsigned int cpu, int count)
{
	unsigned long long task_idx;
	ssize_t offset = 0;
	int max_count = ARRAY_SIZE(dss_log->task[0]);
	char buf[LOG_LINE_MAX];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_TASK_ID];

	if (!log_item->entry.enabled)
		return;

	if (count > max_count)
		count = max_count;

	offset += scnprintf(buf + offset, LOG_LINE_MAX - offset, "Sched info ");
	task_idx = atomic_read(&dss_log_misc.task_log_idx[cpu]) & (max_count - 1);

	while (count--) {
		if (dss_log->task[cpu][task_idx].time == 0)
			break;
		offset += scnprintf(buf + offset, LOG_LINE_MAX - offset, "[%d]<", dss_log->task[cpu][task_idx].pid);
		task_idx = task_idx > 0 ? (task_idx - 1) : (max_count - 1);
	}
	pr_auto(ASL5, "%s\n", buf);
}

static unsigned long long secdbg_make_busy_task_list(unsigned int cpu, unsigned long long duration)
{
	unsigned long next_task_idx;
	unsigned long long residency;
	unsigned long long limit_time = local_clock() - duration * NSEC_PER_SEC;
	struct list_head *entry;
	int max_count = ARRAY_SIZE(dss_log->task[0]);
	int count = max_count;
	unsigned long task_idx = atomic_read(&dss_log_misc.task_log_idx[cpu]) & (max_count - 1);
	struct busy_info *info;

	is_busy_info_list = true;
	is_busy = 1;

	residency = local_clock() - dss_log->task[cpu][task_idx].time;
	real_duration += residency;
	entry = create_busy_info(dss_log->task[cpu][task_idx].task, residency);
	if (!entry)
		return 0;

	list_add(entry, &busy_info_list);

	next_task_idx = task_idx;
	task_idx = task_idx > 0 ? (task_idx - 1) : (max_count - 1);

	while (--count) {
		if (dss_log->task[cpu][task_idx].time == 0 ||
			(dss_log->task[cpu][task_idx].time > dss_log->task[cpu][next_task_idx].time) ||
			(dss_log->task[cpu][task_idx].time < limit_time)) {
			break;
		}

		residency = dss_log->task[cpu][next_task_idx].time - dss_log->task[cpu][task_idx].time;
		real_duration += residency;

		list_for_each_entry(info, &busy_info_list, node) {
			if (info->pid == dss_log->task[cpu][task_idx].pid) {
				info->residency += residency;
				goto next;
			}
		}

		entry = create_busy_info(dss_log->task[cpu][task_idx].task, residency);
		if (!entry)
			return real_duration - residency;

		list_add(entry, &busy_info_list);
next:
		next_task_idx = task_idx;
		task_idx = task_idx > 0 ? (task_idx - 1) : (max_count - 1);
	}

	return real_duration;
}

enum sched_class_type {
	SECDBG_SCHED_STOP,
	SECDBG_SCHED_DL,
	SECDBG_SCHED_RT,
	SECDBG_SCHED_FAIR,
	SECDBG_SCHED_IDLE,
	SECDBG_SCHED_NONE
};

static enum sched_class_type get_sched_class(struct task_struct *tsk)
{
	const struct sched_class *class = tsk->sched_class;

	if (class == &stop_sched_class)
		return SECDBG_SCHED_STOP;

	if (class == &dl_sched_class)
		return SECDBG_SCHED_DL;

	if (class == &rt_sched_class)
		return SECDBG_SCHED_RT;

	if (class == &fair_sched_class)
		return SECDBG_SCHED_FAIR;

	if (class == &idle_sched_class)
		return SECDBG_SCHED_IDLE;

	return SECDBG_SCHED_NONE;
}

int secdbg_show_busy_task(unsigned int cpu, unsigned long long duration, int count)
{
	unsigned long long real_duration;
	struct busy_info *info;
	struct task_struct *main_busy_tsk;
	ssize_t offset = 0;
	char buf[LOG_LINE_MAX];
	struct dbg_snapshot_log_item *log_item = &dss_log_items[DSS_LOG_TASK_ID];
	char sched_class_array[] = {'S', 'D', 'R', 'F', 'I', '0'};

	if (is_busy_info_list)
		return -1;

	pr_auto(ASL5, "CPU%u [CFS util_avg:%lu load_avg:%lu nr_running:%u][RT util_avg:%lu load_avg:%lu rt_nr_running:%u][avg_rt util_avg:%lu]",	\
		cpu, cpu_rq(cpu)->cfs.avg.util_avg, cpu_rq(cpu)->cfs.avg.load_avg, cpu_rq(cpu)->cfs.nr_running,	\
		cpu_rq(cpu)->rt.avg.util_avg, cpu_rq(cpu)->rt.avg.load_avg, cpu_rq(cpu)->rt.rt_nr_running, cpu_rq(cpu)->avg_rt.util_avg);

	if (!log_item->entry.enabled)
		return -1;

	real_duration = secdbg_make_busy_task_list(cpu, duration);
	list_sort(NULL, &busy_info_list, residency_cmp);

	if (list_empty(&busy_info_list))
		return -1;

	offset += scnprintf(buf + offset, LOG_LINE_MAX - offset, "CPU Usage: PERIOD(%us)", (unsigned int)(real_duration / NSEC_PER_SEC));

	list_for_each_entry(info, &busy_info_list, node) {
		offset += scnprintf(buf + offset, LOG_LINE_MAX - offset, \
			" %s:%d[%c,%d](%u%%)", info->tsk->comm, info->tsk->pid, \
			sched_class_array[get_sched_class(info->tsk)],	\
			info->tsk->prio, (unsigned int)((info->residency * 100) / real_duration));
		if (--count == 0)
			break;
	}

	pr_auto(ASL5, "%s\n", buf);

	info = list_first_entry(&busy_info_list, struct busy_info, node);
	main_busy_tsk = info->tsk;

	if (!is_busy) {
		pr_auto(ASL5, "CPU%u is not busy.", cpu);
	} else if (main_busy_tsk == cpu_curr(cpu)) {
		smp_call_function_single(cpu, show_callstack, NULL, 1);
	} else {
#ifdef CONFIG_SEC_DEBUG_AUTO_COMMENT
		show_stack_auto_comment(main_busy_tsk, NULL);
#else
		show_stack(main_busy_tsk, NULL);
#endif
	}

	return is_busy;
}

struct task_struct *get_the_busiest_task(void)
{
	struct busy_info *info;

	if (!is_busy_info_list)
		return NULL;

	if (list_empty(&busy_info_list))
		return NULL;

	info = list_first_entry(&busy_info_list, struct busy_info, node);

	return info->tsk;
}
#endif
