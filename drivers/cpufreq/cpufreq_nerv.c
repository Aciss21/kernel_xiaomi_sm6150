/*
 *  drivers/cpufreq/cpufreq_nerv.c
 *
 *  Copyright (C) 2024 Aciss
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  Nerv - A balanced CPU frequency governor for daily use and gaming.
 *
 *  Design goals:
 *  - Smooth, gradual frequency scaling to avoid thermal spikes during
 *    everyday workloads (browsing, video playback, office apps).
 *  - Fast ramp-up to a configurable hispeed frequency when a burst of
 *    load is detected (gaming, heavy multitasking).
 *  - No aggressive 100% frequency jumping; instead, proportional scaling
 *    based on real CPU utilization.
 *  - Tunable via sysfs for flexibility across different devices.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/stat.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/tick.h>
#include <linux/cpu.h>
#include <linux/sched/cputime.h>
#include <linux/irq_work.h>
#include <linux/interrupt.h>

/* Nerv governor tunables */
struct nerv_governor {
	struct cpufreq_policy *policy;
	struct hrtimer timer;
	struct irq_work irq_work;
	u64 sample_rate_ms;
	u64 hispeed_freq;
	u64 target_load;       /* target CPU load percentage (0-100) */
	u64 freq_step;         /* frequency step percentage (0-100) */
	u64 up_threshold;      /* load % to start ramping up */
	u64 down_threshold;    /* load % to start ramping down */
	u64 hispeed_load;      /* load % to jump to hispeed */
	u64 min_sample_time;   /* minimum time between samples (us) */
	u64 boost_duration;    /* how long to stay boosted (ms) */

	/* Runtime state */
	u64 last_sample_time;
	u64 last_freq_update;
	u64 freq_fast;         /* cached fast path frequency */
	u64 freq_slow;         /* cached slow path frequency */
	unsigned long boost_until; /* jiffies until boost is active */
	bool boost_active;
	bool suspended;

	/* Statistics */
	u64 total_samples;
	u64 total_up;
	u64 total_down;
};

static DEFINE_PER_CPU(struct nerv_governor *, nerv_gov);
static DEFINE_MUTEX(nerv_mutex);

/* Default tunables */
#define NERV_DEFAULT_SAMPLE_RATE_MS    20
#define NERV_DEFAULT_HISPEED_LOAD      85
#define NERV_DEFAULT_UP_THRESHOLD      70
#define NERV_DEFAULT_DOWN_THRESHOLD    40
#define NERV_DEFAULT_FREQ_STEP         15
#define NERV_DEFAULT_MIN_SAMPLE_TIME   10000  /* 10ms in us */
#define NERV_DEFAULT_BOOST_DURATION    2000   /* 2 seconds */

/* Sysfs show/store helpers */
static ssize_t show_sample_rate_ms(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->sample_rate_ms);
}

static ssize_t store_sample_rate_ms(struct cpufreq_policy *policy,
				    const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 5 || val > 500)
		return -EINVAL;

	nerv->sample_rate_ms = val;
	return count;
}

static struct freq_attr attr_sample_rate_ms = __ATTR(sample_rate_ms, 0644,
	show_sample_rate_ms, store_sample_rate_ms);

static ssize_t show_hispeed_load(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->hispeed_load);
}

static ssize_t store_hispeed_load(struct cpufreq_policy *policy,
				  const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 50 || val > 100)
		return -EINVAL;

	nerv->hispeed_load = val;
	return count;
}

static struct freq_attr attr_hispeed_load = __ATTR(hispeed_load, 0644,
	show_hispeed_load, store_hispeed_load);

static ssize_t show_up_threshold(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->up_threshold);
}

static ssize_t store_up_threshold(struct cpufreq_policy *policy,
				  const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 30 || val > 95)
		return -EINVAL;

	nerv->up_threshold = val;
	return count;
}

static struct freq_attr attr_up_threshold = __ATTR(up_threshold, 0644,
	show_up_threshold, store_up_threshold);

static ssize_t show_down_threshold(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->down_threshold);
}

static ssize_t store_down_threshold(struct cpufreq_policy *policy,
				    const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 10 || val > 80)
		return -EINVAL;

	nerv->down_threshold = val;
	return count;
}

static struct freq_attr attr_down_threshold = __ATTR(down_threshold, 0644,
	show_down_threshold, store_down_threshold);

static ssize_t show_freq_step(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->freq_step);
}

static ssize_t store_freq_step(struct cpufreq_policy *policy,
			       const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 5 || val > 50)
		return -EINVAL;

	nerv->freq_step = val;
	return count;
}

static struct freq_attr attr_freq_step = __ATTR(freq_step, 0644,
	show_freq_step, store_freq_step);

static ssize_t show_boost_duration(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "%llu\n", nerv->boost_duration);
}

static ssize_t store_boost_duration(struct cpufreq_policy *policy,
				    const char *buf, size_t count)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	u64 val;
	int ret;

	if (!nerv)
		return -EINVAL;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 100 || val > 10000)
		return -EINVAL;

	nerv->boost_duration = val;
	return count;
}

static struct freq_attr attr_boost_duration = __ATTR(boost_duration, 0644,
	show_boost_duration, store_boost_duration);

static ssize_t show_stats(struct cpufreq_policy *policy, char *buf)
{
	struct nerv_governor *nerv = this_cpu_read(nerv_gov);
	if (!nerv)
		return -EINVAL;
	return sprintf(buf, "samples: %llu\nup: %llu\ndown: %llu\n",
		       nerv->total_samples, nerv->total_up, nerv->total_down);
}

static struct freq_attr attr_stats = __ATTR(stats, 0444, show_stats, NULL);

static struct attribute *nerv_attributes[] = {
	&attr_sample_rate_ms.attr,
	&attr_hispeed_load.attr,
	&attr_up_threshold.attr,
	&attr_down_threshold.attr,
	&attr_freq_step.attr,
	&attr_boost_duration.attr,
	&attr_stats.attr,
	NULL
};

static struct attribute_group nerv_attr_group = {
	.attrs = nerv_attributes,
	.name = "nerv",
};

/*
 * nerv_get_cpu_load - Calculate CPU load percentage for a given CPU
 *
 * Uses CPU idle time statistics to compute the percentage of time
 * the CPU was busy since the last sample.
 */
static unsigned int nerv_get_cpu_load(struct nerv_governor *nerv)
{
	u64 idle_time, total_time;
	u64 wall_time, idle;
	unsigned int load;

	/* Get wall time since boot */
	wall_time = jiffies64_to_nsecs(get_jiffies_64());

	/* Get idle time for this CPU */
	idle = kcpustat_cpu(nerv->policy->cpu).cpustat[CPUTIME_IDLE];

	/* Calculate load as percentage of non-idle time */
	if (wall_time > 0) {
		idle_time = idle;
		total_time = wall_time;

		/* Load = (1 - idle/total) * 100 */
		if (idle_time >= total_time)
			load = 0;
		else
			load = 100 - ((idle_time * 100) / total_time);

		if (load > 100)
			load = 100;
	} else {
		load = 50;
	}

	return load;
}

/*
 * nerv_find_freq - Find the next frequency based on current load
 *
 * This is the core scaling logic. It uses a proportional approach:
 * - If load > hispeed_load: jump to hispeed frequency
 * - If load > up_threshold: ramp up by freq_step percentage
 * - If load < down_threshold: ramp down by freq_step percentage
 * - Otherwise: hold current frequency
 */
static unsigned int nerv_find_freq(struct nerv_governor *nerv,
				   unsigned int load,
				   unsigned int cur_freq,
				   unsigned int min_freq,
				   unsigned int max_freq)
{
	unsigned int next_freq;
	unsigned int freq_step_abs;

	/* Boost path: high load detected, jump to hispeed */
	if (load >= nerv->hispeed_load) {
		next_freq = nerv->hispeed_freq;
		if (next_freq > max_freq)
			next_freq = max_freq;
		if (next_freq < min_freq)
			next_freq = min_freq;
		nerv->boost_until = jiffies +
			msecs_to_jiffies(nerv->boost_duration);
		nerv->boost_active = true;
		nerv->total_up++;
		return next_freq;
	}

	/* Check if we're in a boost window */
	if (nerv->boost_active) {
		if (time_after(jiffies, nerv->boost_until)) {
			nerv->boost_active = false;
		} else {
			/* During boost, stay at hispeed unless load drops low */
			if (load < nerv->down_threshold) {
				nerv->boost_active = false;
			} else {
				next_freq = nerv->hispeed_freq;
				if (next_freq > max_freq)
					next_freq = max_freq;
				return next_freq;
			}
		}
	}

	freq_step_abs = (cur_freq * nerv->freq_step) / 100;
	if (freq_step_abs < 1)
		freq_step_abs = 1;

	if (load >= nerv->up_threshold) {
		/* Ramp up proportionally */
		next_freq = cur_freq + freq_step_abs;
		if (next_freq > max_freq)
			next_freq = max_freq;
		nerv->total_up++;
	} else if (load <= nerv->down_threshold) {
		/* Ramp down proportionally */
		if (cur_freq > freq_step_abs)
			next_freq = cur_freq - freq_step_abs;
		else
			next_freq = min_freq;
		if (next_freq < min_freq)
			next_freq = min_freq;
		nerv->total_down++;
	} else {
		/* Hold frequency */
		next_freq = cur_freq;
	}

	return next_freq;
}

/*
 * nerv_update_freq - Update CPU frequency based on current utilization
 */
static void nerv_update_freq(struct nerv_governor *nerv)
{
	struct cpufreq_policy *policy = nerv->policy;
	unsigned int cur_freq, min_freq, max_freq, next_freq;
	u64 now, delta_us;
	unsigned int load;

	if (!policy || nerv->suspended)
		return;

	now = ktime_to_us(ktime_get());
	delta_us = now - nerv->last_sample_time;

	/* Enforce minimum sample interval */
	if (delta_us < nerv->min_sample_time)
		return;

	nerv->last_sample_time = now;
	nerv->total_samples++;

	cur_freq = policy->cur;
	min_freq = policy->min;
	max_freq = policy->max;

	/* Calculate CPU load percentage */
	load = nerv_get_cpu_load(nerv);

	/* Clamp load */
	if (load > 100)
		load = 100;

	next_freq = nerv_find_freq(nerv, load, cur_freq, min_freq, max_freq);

	if (next_freq != cur_freq) {
		__cpufreq_driver_target(policy, next_freq,
					CPUFREQ_RELATION_L);
		nerv->last_freq_update = now;
	}
}

/*
 * nerv_timer_cb - High-resolution timer callback for sampling
 */
static enum hrtimer_restart nerv_timer_cb(struct hrtimer *timer)
{
	struct nerv_governor *nerv = container_of(timer, struct nerv_governor, timer);

	irq_work_queue(&nerv->irq_work);

	return HRTIMER_NORESTART;
}

/*
 * nerv_irq_work - Process frequency update in IRQ context
 */
static void nerv_irq_work(struct irq_work *irq_work)
{
	struct nerv_governor *nerv = container_of(irq_work, struct nerv_governor, irq_work);

	mutex_lock(&nerv_mutex);
	nerv_update_freq(nerv);
	mutex_unlock(&nerv_mutex);

	/* Reschedule timer */
	hrtimer_start(&nerv->timer,
		      ms_to_ktime(nerv->sample_rate_ms),
		      HRTIMER_MODE_REL_PINNED);
}

/*
 * nerv_start - Initialize governor for a CPU
 */
static int nerv_start(struct cpufreq_policy *policy)
{
	struct nerv_governor *nerv;
	int cpu = policy->cpu;

	if (!policy->cur)
		return -EINVAL;

	nerv = kzalloc(sizeof(*nerv), GFP_KERNEL);
	if (!nerv)
		return -ENOMEM;

	nerv->policy = policy;
	nerv->sample_rate_ms = NERV_DEFAULT_SAMPLE_RATE_MS;
	nerv->hispeed_load = NERV_DEFAULT_HISPEED_LOAD;
	nerv->up_threshold = NERV_DEFAULT_UP_THRESHOLD;
	nerv->down_threshold = NERV_DEFAULT_DOWN_THRESHOLD;
	nerv->freq_step = NERV_DEFAULT_FREQ_STEP;
	nerv->min_sample_time = NERV_DEFAULT_MIN_SAMPLE_TIME;
	nerv->boost_duration = NERV_DEFAULT_BOOST_DURATION;
	nerv->last_sample_time = ktime_to_us(ktime_get());
	nerv->last_freq_update = nerv->last_sample_time;
	nerv->boost_active = false;
	nerv->suspended = false;
	nerv->total_samples = 0;
	nerv->total_up = 0;
	nerv->total_down = 0;

	/* Set hispeed frequency to 80% of max */
	nerv->hispeed_freq = policy->max * 80 / 100;
	if (nerv->hispeed_freq < policy->min)
		nerv->hispeed_freq = policy->min;

	per_cpu(nerv_gov, cpu) = nerv;

	/* Initialize timer */
	hrtimer_init(&nerv->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	nerv->timer.function = nerv_timer_cb;
	init_irq_work(&nerv->irq_work, nerv_irq_work);

	/* Start timer */
	hrtimer_start(&nerv->timer,
		      ms_to_ktime(nerv->sample_rate_ms),
		      HRTIMER_MODE_REL_PINNED);

	/* Create sysfs group */
	if (sysfs_create_group(&policy->kobj, &nerv_attr_group))
		pr_warn("Nerv: unable to create sysfs group for cpu%d\n", cpu);

	pr_info("Nerv: governor started for cpu%d (hispeed=%llu kHz)\n",
		cpu, nerv->hispeed_freq);

	return 0;
}

/*
 * nerv_stop - Stop governor for a CPU
 */
static void nerv_stop(struct cpufreq_policy *policy)
{
	struct nerv_governor *nerv = per_cpu(nerv_gov, policy->cpu);

	if (!nerv)
		return;

	hrtimer_cancel(&nerv->timer);
	irq_work_sync(&nerv->irq_work);

	sysfs_remove_group(&policy->kobj, &nerv_attr_group);

	per_cpu(nerv_gov, policy->cpu) = NULL;
	kfree(nerv);

	pr_info("Nerv: governor stopped for cpu%d\n", policy->cpu);
}

/*
 * nerv_limits - Apply frequency limits
 */
static void nerv_limits(struct cpufreq_policy *policy)
{
	struct nerv_governor *nerv = per_cpu(nerv_gov, policy->cpu);

	if (!nerv)
		return;

	mutex_lock(&nerv_mutex);

	/* Recalculate hispeed if max changed */
	nerv->hispeed_freq = policy->max * 80 / 100;
	if (nerv->hispeed_freq < policy->min)
		nerv->hispeed_freq = policy->min;

	mutex_unlock(&nerv_mutex);
}

static struct cpufreq_governor nerv_governor = {
	.name		= "nerv",
	.start		= nerv_start,
	.stop		= nerv_stop,
	.limits		= nerv_limits,
	.owner		= THIS_MODULE,
};

/*
 * nerv_init - Governor initialization
 */
static int __init nerv_init(void)
{
	return cpufreq_register_governor(&nerv_governor);
}

/*
 * nerv_exit - Governor cleanup
 */
static void __exit nerv_exit(void)
{
	cpufreq_unregister_governor(&nerv_governor);
}

MODULE_AUTHOR("Aciss");
MODULE_DESCRIPTION("Nerv - A balanced CPU frequency governor for daily use and gaming");
MODULE_LICENSE("GPL");

module_init(nerv_init);
module_exit(nerv_exit);
