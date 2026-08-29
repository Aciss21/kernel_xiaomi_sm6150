// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/cpufreq/cpufreq_nerV.c
 *
 * NerV CPUFreq Governor
 * Busy-time / idle-time based governor tuned for multitasking, gaming and
 * daily use. Fast hispeed ramp-up for responsiveness, per-policy load
 * aggregation so background cores keep the big cores ready (multitasking),
 * and asymmetric rate limits that climb fast for smooth gaming but taper
 * down slowly to avoid frequency bounce.
 *
 * Author: Aciss21 <wawa22arpk@gmail.com>
 *
 * This governor is provided "as is", with no warranty of any kind.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/time.h>

/* ================= Tunables ================= */

struct nerv_tunables {
	struct gov_attr_set attr_set;

	/* Jump to this frequency when load is at/above go_hispeed_load. */
	unsigned int hispeed_freq;

	/* CPU load at/above which we ramp straight to hispeed_freq. */
#define DEFAULT_GO_HISPEED_LOAD 85
	unsigned long go_hispeed_load;

	/*
	 * Targeted load: given current freq f and observed load l, the
	 * governor seeks the lowest frequency whose computed load
	 * (l * f / f_target) stays at or below the targeted load at that
	 * frequency. Higher frequencies map to lower target loads so that
	 * heavy, bursty, multi-core work gets headroom (good for gaming and
	 * multitasking) without pinning the CPU at max.
	 */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;

	/*
	 * Minimum time (usecs) to hold a frequency before allowing a drop.
	 * Prevents frequency bounce when a core dips briefly.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (60 * USEC_PER_MSEC)
	unsigned long min_sample_time;

	/* Sampling rate for the timer that raises frequency. */
#define DEFAULT_SAMPLING_RATE (20 * USEC_PER_MSEC)
	unsigned long sampling_rate;

	/*
	 * Additional idle time we tolerate before waking up to reduce the
	 * frequency. Larger values save power on idle.
	 */
	spinlock_t above_hispeed_delay_lock;
	unsigned int *above_hispeed_delay;
	int nabove_hispeed_delay;

	/* Non-zero means indefinite speed boost active (gaming / bench). */
	int boost;
	/* Duration of a boost pulse in usecs (app launch, keys, frames). */
	int boostpulse_duration;
	u64 boostpulse_endtime;
	bool boosted;

	/* If true, I/O wait counts as busy. Good for multitask storage hits. */
	bool io_is_busy;
};

/* ================= Per-policy / per-cpu state ================= */

struct nerv_policy {
	struct cpufreq_policy *policy;
	struct nerv_tunables *tunables;
	struct list_head tunables_hook;
};

struct nerv_cpu {
	struct update_util_data update_util;
	struct nerv_policy *np;

	struct irq_work irq_work;
	u64 last_sample_time;
	unsigned long next_sample_jiffies;
	bool work_in_progress;

	struct rw_semaphore enable_sem;

	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;

	spinlock_t target_freq_lock; /* protects target freq */
	unsigned int target_freq;

	unsigned int floor_freq;
	u64 pol_floor_val_time;
	u64 loc_floor_val_time;
	u64 pol_hispeed_val_time;
	u64 loc_hispeed_val_time;
};

static DEFINE_PER_CPU(struct nerv_cpu, nerv_cpu);

/* Realtime thread that applies the highest requested frequency of a policy */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;

/* Defaults */
#define DEFAULT_TARGET_LOAD 90
static unsigned int default_target_loads[] = { DEFAULT_TARGET_LOAD };
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_SAMPLING_RATE
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY
};

#define for_each_np(__np)	\
	list_for_each_entry(__np, &tunables->attr_set.policy_list, tunables_hook)

static struct nerv_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

/* ================= Helpers ================= */

static unsigned int
freq_to_above_hispeed_delay(struct nerv_tunables *tunables, unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
	     freq >= tunables->above_hispeed_delay[i + 1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return ret;
}

static unsigned int freq_to_targetload(struct nerv_tunables *tunables,
				       unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
	     freq >= tunables->target_loads[i + 1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return ret;
}

/*
 * Find the lowest frequency that keeps the scaled load at or below its
 * targeted load for the current frequency.
 */
static unsigned int choose_freq(struct nerv_cpu *ncpu, unsigned int loadadjfreq)
{
	struct cpufreq_policy *policy = ncpu->np->policy;
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	unsigned int prevfreq, freqmin = 0, freqmax = UINT_MAX, tl;
	unsigned int freq = policy->cur;
	int index;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(ncpu->np->tunables, freq);

		index = cpufreq_frequency_table_target(policy, loadadjfreq / tl,
						       CPUFREQ_RELATION_L);
		freq = freq_table[index].frequency;

		if (freq > prevfreq) {
			freqmin = prevfreq;

			if (freq < freqmax)
				continue;

			index = cpufreq_frequency_table_target(policy,
					freqmax - 1, CPUFREQ_RELATION_H);
			freq = freq_table[index].frequency;

			if (freq == freqmin) {
				freq = freqmax;
				break;
			}
		} else if (freq < prevfreq) {
			freqmax = prevfreq;

			if (freq > freqmin)
				continue;

			index = cpufreq_frequency_table_target(policy,
					freqmin + 1, CPUFREQ_RELATION_L);
			freq = freq_table[index].frequency;

			if (freq == freqmax)
				break;
		}
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(struct nerv_cpu *ncpu, int cpu)
{
	struct nerv_tunables *tunables = ncpu->np->tunables;
	u64 now_idle, now, active_time, delta_idle, delta_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = now_idle - ncpu->time_in_idle;
	delta_time = now - ncpu->time_in_idle_timestamp;

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	ncpu->cputime_speedadj += active_time * ncpu->np->policy->cur;

	ncpu->time_in_idle = now_idle;
	ncpu->time_in_idle_timestamp = now;

	return now;
}

/* ================= Load evaluation ================= */

static void eval_target_freq(struct nerv_cpu *ncpu)
{
	struct nerv_tunables *tunables = ncpu->np->tunables;
	struct cpufreq_policy *policy = ncpu->np->policy;
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	u64 cputime_speedadj, now, max_fvtime;
	unsigned int new_freq, loadadjfreq, index, delta_time;
	unsigned long flags;
	int cpu_load;
	int cpu = smp_processor_id();

	spin_lock_irqsave(&ncpu->load_lock, flags);
	now = update_load(ncpu, smp_processor_id());
	delta_time = (unsigned int)(now - ncpu->cputime_speedadj_timestamp);
	cputime_speedadj = ncpu->cputime_speedadj;
	spin_unlock_irqrestore(&ncpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		return;

	spin_lock_irqsave(&ncpu->target_freq_lock, flags);
	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / policy->cur;
	tunables->boosted = tunables->boost ||
			    now < tunables->boostpulse_endtime;

	if (cpu_load >= tunables->go_hispeed_load || tunables->boosted) {
		if (policy->cur < tunables->hispeed_freq) {
			new_freq = tunables->hispeed_freq;
		} else {
			new_freq = choose_freq(ncpu, loadadjfreq);

			if (new_freq < tunables->hispeed_freq)
				new_freq = tunables->hispeed_freq;
		}
	} else {
		new_freq = choose_freq(ncpu, loadadjfreq);
		if (new_freq > tunables->hispeed_freq &&
		    policy->cur < tunables->hispeed_freq)
			new_freq = tunables->hispeed_freq;
	}

	/* Above hispeed, delay further ramps to avoid overshoot on bursts. */
	if (policy->cur >= tunables->hispeed_freq &&
	    new_freq > policy->cur &&
	    now - ncpu->pol_hispeed_val_time <
			freq_to_above_hispeed_delay(tunables, policy->cur))
		goto exit;

	ncpu->loc_hispeed_val_time = now;

	index = cpufreq_frequency_table_target(policy, new_freq,
					       CPUFREQ_RELATION_L);
	new_freq = freq_table[index].frequency;

	/* Hold the floor frequency for at least min_sample_time. */
	max_fvtime = max(ncpu->pol_floor_val_time, ncpu->loc_floor_val_time);
	if (new_freq < ncpu->floor_freq && ncpu->target_freq >= policy->cur) {
		if (now - max_fvtime < tunables->min_sample_time)
			goto exit;
	}

	if (!tunables->boosted || new_freq > tunables->hispeed_freq) {
		ncpu->floor_freq = new_freq;
		if (ncpu->target_freq >= policy->cur || new_freq >= policy->cur)
			ncpu->loc_floor_val_time = now;
	}

	if (ncpu->target_freq == new_freq &&
	    ncpu->target_freq <= policy->cur)
		goto exit;

	ncpu->target_freq = new_freq;
	spin_unlock_irqrestore(&ncpu->target_freq_lock, flags);

	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(cpu, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	wake_up_process(speedchange_task);
	return;

exit:
	spin_unlock_irqrestore(&ncpu->target_freq_lock, flags);
}

static void nerv_update(struct nerv_cpu *ncpu)
{
	eval_target_freq(ncpu);
}

/* ================= Policy-level aggregation ================= */

static void nerv_get_policy_info(struct cpufreq_policy *policy,
				 unsigned int *pmax_freq, u64 *phvt,
				 u64 *pfvt)
{
	struct nerv_cpu *ncpu;
	u64 hvt = ~0ULL, fvt = 0;
	unsigned int max_freq = 0, i;

	for_each_cpu(i, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, i);

		fvt = max(fvt, ncpu->loc_floor_val_time);
		if (ncpu->target_freq > max_freq) {
			max_freq = ncpu->target_freq;
			hvt = ncpu->loc_hispeed_val_time;
		} else if (ncpu->target_freq == max_freq) {
			hvt = min(hvt, ncpu->loc_hispeed_val_time);
		}
	}

	*pmax_freq = max_freq;
	*phvt = hvt;
	*pfvt = fvt;
}

static void nerv_adjust_cpu(unsigned int cpu, struct cpufreq_policy *policy)
{
	struct nerv_cpu *ncpu;
	u64 hvt, fvt;
	unsigned int max_freq;
	int i;

	nerv_get_policy_info(policy, &max_freq, &hvt, &fvt);

	for_each_cpu(i, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, i);
		ncpu->pol_floor_val_time = fvt;
	}

	/*
	 * Raise to the highest requested frequency across the policy. This
	 * keeps the cluster ready when any core is busy, which benefits both
	 * multitasking (background + foreground) and gaming (multi-core).
	 */
	if (max_freq != policy->cur) {
		__cpufreq_driver_target(policy, max_freq, CPUFREQ_RELATION_H);
		for_each_cpu(i, policy->cpus) {
			ncpu = &per_cpu(nerv_cpu, i);
			ncpu->pol_hispeed_val_time = hvt;
		}
	}
}

static int nerv_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;

again:
	set_current_state(TASK_INTERRUPTIBLE);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);

	if (cpumask_empty(&speedchange_cpumask)) {
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
		schedule();

		if (kthread_should_stop())
			return 0;

		spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	}

	set_current_state(TASK_RUNNING);
	tmp_mask = speedchange_cpumask;
	cpumask_clear(&speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	for_each_cpu(cpu, &tmp_mask) {
		struct nerv_cpu *ncpu = &per_cpu(nerv_cpu, cpu);
		struct cpufreq_policy *policy;

		if (unlikely(!down_read_trylock(&ncpu->enable_sem)))
			continue;

		if (likely(ncpu->np)) {
			policy = ncpu->np->policy;
			nerv_adjust_cpu(cpu, policy);
		}

		up_read(&ncpu->enable_sem);
	}

	goto again;
}

static void nerv_boost(struct nerv_tunables *tunables)
{
	struct nerv_policy *np;
	struct cpufreq_policy *policy;
	struct nerv_cpu *ncpu;
	unsigned long flags[2];
	bool wakeup = false;
	int i;

	tunables->boosted = true;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags[0]);

	for_each_np(np) {
		policy = np->policy;

		for_each_cpu(i, policy->cpus) {
			ncpu = &per_cpu(nerv_cpu, i);

			if (!down_read_trylock(&ncpu->enable_sem))
				continue;

			if (!ncpu->np) {
				up_read(&ncpu->enable_sem);
				continue;
			}

			spin_lock_irqsave(&ncpu->target_freq_lock, flags[1]);
			if (ncpu->target_freq < tunables->hispeed_freq) {
				ncpu->target_freq = tunables->hispeed_freq;
				cpumask_set_cpu(i, &speedchange_cpumask);
				ncpu->pol_hispeed_val_time = ktime_to_us(ktime_get());
				wakeup = true;
			}
			spin_unlock_irqrestore(&ncpu->target_freq_lock, flags[1]);

			up_read(&ncpu->enable_sem);
		}
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags[0]);

	if (wakeup)
		wake_up_process(speedchange_task);
}

/* ================= Idle / transition notifiers ================= */

static int nerv_notifier(struct notifier_block *nb, unsigned long val,
			 void *data)
{
	struct cpufreq_freqs *freq = data;
	struct nerv_cpu *ncpu = &per_cpu(nerv_cpu, freq->cpu);
	unsigned long flags;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	if (!down_read_trylock(&ncpu->enable_sem))
		return 0;

	if (!ncpu->np) {
		up_read(&ncpu->enable_sem);
		return 0;
	}

	spin_lock_irqsave(&ncpu->load_lock, flags);
	update_load(ncpu, freq->cpu);
	spin_unlock_irqrestore(&ncpu->load_lock, flags);

	up_read(&ncpu->enable_sem);

	return 0;
}

static struct notifier_block nerv_notifier_block = {
	.notifier_call = nerv_notifier,
};

/* ================= Sysfs interface ================= */

static struct nerv_tunables *to_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct nerv_tunables, attr_set);
}

#define show_one(file_name, type)					\
static ssize_t show_##file_name(struct gov_attr_set *attr_set, char *buf) \
{									\
	struct nerv_tunables *tunables = to_tunables(attr_set);		\
	return sprintf(buf, type "\n", tunables->file_name);		\
}

#define gov_attr_ro(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)
#define gov_attr_wo(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0200, NULL, store_##_name)
#define gov_attr_rw(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp = buf;
	int ntokens = 1, i = 0;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kcalloc(ntokens, sizeof(*tokenized_data), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	while (i < ntokens) {
		if (kstrtouint(cp, 0, &tokenized_data[i++]) < 0)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t show_target_loads(struct gov_attr_set *attr_set, char *buf)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long flags;
	ssize_t ret = 0;
	int i;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return ret;
}

static ssize_t store_target_loads(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned int *new_target_loads;
	unsigned long flags;
	int ntokens;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_ERR(new_target_loads);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return count;
}

static ssize_t show_above_hispeed_delay(struct gov_attr_set *attr_set,
					char *buf)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long flags;
	ssize_t ret = 0;
	int i;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s",
			       tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return ret;
}

static ssize_t store_above_hispeed_delay(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;
	int ntokens;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_ERR(new_above_hispeed_delay);

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return count;
}

static ssize_t store_hispeed_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long int val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->hispeed_freq = val;

	return count;
}

static ssize_t store_go_hispeed_load(struct gov_attr_set *attr_set,
				     const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->go_hispeed_load = val;

	return count;
}

static ssize_t store_min_sample_time(struct gov_attr_set *attr_set,
				     const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->min_sample_time = val;

	return count;
}

static ssize_t show_timer_rate(struct gov_attr_set *attr_set, char *buf)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);

	return sprintf(buf, "%lu\n", tunables->sampling_rate);
}

static ssize_t store_timer_rate(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val, val_round;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);

	tunables->sampling_rate = val_round;

	return count;
}

static ssize_t store_boost(struct gov_attr_set *attr_set, const char *buf,
			   size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boost = val;

	if (tunables->boost) {
		if (!tunables->boosted)
			nerv_boost(tunables);
	} else {
		tunables->boostpulse_endtime = ktime_to_us(ktime_get());
	}

	return count;
}

static ssize_t store_boostpulse(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_endtime = ktime_to_us(ktime_get()) +
					tunables->boostpulse_duration;
	if (!tunables->boosted)
		nerv_boost(tunables);

	return count;
}

static ssize_t store_boostpulse_duration(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_duration = val;

	return count;
}

static ssize_t store_io_is_busy(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->io_is_busy = val;

	return count;
}

show_one(hispeed_freq, "%u");
show_one(go_hispeed_load, "%lu");
show_one(min_sample_time, "%lu");
show_one(boost, "%u");
show_one(boostpulse_duration, "%u");
show_one(io_is_busy, "%u");

gov_attr_rw(target_loads);
gov_attr_rw(above_hispeed_delay);
gov_attr_rw(hispeed_freq);
gov_attr_rw(go_hispeed_load);
gov_attr_rw(min_sample_time);
gov_attr_rw(timer_rate);
gov_attr_rw(boost);
gov_attr_wo(boostpulse);
gov_attr_rw(boostpulse_duration);
gov_attr_rw(io_is_busy);

static struct attribute *nerv_attributes[] = {
	&target_loads.attr,
	&above_hispeed_delay.attr,
	&hispeed_freq.attr,
	&go_hispeed_load.attr,
	&min_sample_time.attr,
	&timer_rate.attr,
	&boost.attr,
	&boostpulse.attr,
	&boostpulse_duration.attr,
	&io_is_busy.attr,
	NULL
};

static struct kobj_type nerv_tunables_ktype = {
	.default_attrs = nerv_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

/* ================= Governor callbacks ================= */

static struct nerv_governor {
	struct cpufreq_governor gov;
	unsigned int usage_count;
} nerv_gov;

static void irq_work(struct irq_work *irq_work)
{
	struct nerv_cpu *ncpu = container_of(irq_work, struct nerv_cpu,
					     irq_work);

	nerv_update(ncpu);
	ncpu->work_in_progress = false;
}

static void update_util_handler(struct update_util_data *data, u64 time,
				unsigned int flags)
{
	struct nerv_cpu *ncpu = container_of(data, struct nerv_cpu,
					     update_util);
	struct nerv_policy *np = ncpu->np;
	struct nerv_tunables *tunables = np->tunables;
	u64 delta_ns;

	if (ncpu->work_in_progress)
		return;

	delta_ns = time - ncpu->last_sample_time;
	if ((s64)delta_ns < tunables->sampling_rate * NSEC_PER_USEC)
		return;

	ncpu->last_sample_time = time;
	ncpu->next_sample_jiffies = usecs_to_jiffies(tunables->sampling_rate) +
				    jiffies;

	ncpu->work_in_progress = true;
	irq_work_queue(&ncpu->irq_work);
}

static void nerv_set_update_util(struct nerv_policy *np)
{
	struct cpufreq_policy *policy = np->policy;
	struct nerv_cpu *ncpu;
	int cpu;

	for_each_cpu(cpu, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, cpu);

		ncpu->last_sample_time = 0;
		ncpu->next_sample_jiffies = 0;
		cpufreq_add_update_util_hook(cpu, &ncpu->update_util,
					     update_util_handler);
	}
}

static inline void nerv_clear_update_util(struct cpufreq_policy *policy)
{
	int i;

	for_each_cpu(i, policy->cpus)
		cpufreq_remove_update_util_hook(i);

	synchronize_sched();
}

static void ncpu_cancel_work(struct nerv_cpu *ncpu)
{
	irq_work_sync(&ncpu->irq_work);
	ncpu->work_in_progress = false;
}

static struct nerv_policy *nerv_policy_alloc(struct cpufreq_policy *policy)
{
	struct nerv_policy *np;

	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np)
		return NULL;

	np->policy = policy;

	return np;
}

static void nerv_policy_free(struct nerv_policy *np)
{
	kfree(np);
}

static struct nerv_tunables *
nerv_tunables_alloc(struct nerv_policy *np)
{
	struct nerv_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return NULL;

	gov_attr_set_init(&tunables->attr_set, &np->tunables_hook);
	if (!have_governor_per_policy())
		global_tunables = tunables;

	np->tunables = tunables;

	return tunables;
}

static void nerv_tunables_free(struct nerv_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static int cpufreq_nerV_init(struct cpufreq_policy *policy)
{
	struct nerv_policy *np;
	struct nerv_tunables *tunables;
	int ret;

	if (policy->governor_data)
		return -EBUSY;

	np = nerv_policy_alloc(policy);
	if (!np)
		return -ENOMEM;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_np;
		}

		policy->governor_data = np;
		np->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &np->tunables_hook);
		goto out;
	}

	tunables = nerv_tunables_alloc(np);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_np;
	}

	tunables->hispeed_freq = policy->max;
	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
		ARRAY_SIZE(default_above_hispeed_delay);
	tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	tunables->boostpulse_duration = DEFAULT_MIN_SAMPLE_TIME;
	tunables->sampling_rate = DEFAULT_SAMPLING_RATE;

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

	policy->governor_data = np;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &nerv_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   nerv_gov.gov.name);
	if (ret)
		goto fail;

	if (!nerv_gov.usage_count++)
		cpufreq_register_notifier(&nerv_notifier_block,
					  CPUFREQ_TRANSITION_NOTIFIER);

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	policy->governor_data = NULL;
	nerv_tunables_free(tunables);

free_np:
	mutex_unlock(&global_tunables_lock);

	nerv_policy_free(np);
	pr_err("governor initialization failed (%d)\n", ret);

	return ret;
}

static void cpufreq_nerV_exit(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;
	struct nerv_tunables *tunables = np->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	if (!--nerv_gov.usage_count)
		cpufreq_unregister_notifier(&nerv_notifier_block,
					    CPUFREQ_TRANSITION_NOTIFIER);

	count = gov_attr_set_put(&tunables->attr_set, &np->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		nerv_tunables_free(tunables);

	mutex_unlock(&global_tunables_lock);

	nerv_policy_free(np);
}

static int cpufreq_nerV_start(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;
	struct nerv_cpu *ncpu;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, cpu);

		ncpu->target_freq = policy->cur;
		ncpu->floor_freq = ncpu->target_freq;
		ncpu->pol_floor_val_time = ktime_to_us(ktime_get());
		ncpu->loc_floor_val_time = ncpu->pol_floor_val_time;
		ncpu->pol_hispeed_val_time = ncpu->pol_floor_val_time;
		ncpu->loc_hispeed_val_time = ncpu->pol_floor_val_time;

		down_write(&ncpu->enable_sem);
		ncpu->np = np;
		up_write(&ncpu->enable_sem);
	}

	nerv_set_update_util(np);
	return 0;
}

static void cpufreq_nerV_stop(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;
	struct nerv_cpu *ncpu;
	unsigned int cpu;

	nerv_clear_update_util(np->policy);

	for_each_cpu(cpu, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, cpu);

		ncpu_cancel_work(ncpu);

		down_write(&ncpu->enable_sem);
		ncpu->np = NULL;
		up_write(&ncpu->enable_sem);
	}
}

static void cpufreq_nerV_limits(struct cpufreq_policy *policy)
{
	struct nerv_cpu *ncpu;
	unsigned int cpu;
	unsigned long flags;

	cpufreq_policy_apply_limits(policy);

	for_each_cpu(cpu, policy->cpus) {
		ncpu = &per_cpu(nerv_cpu, cpu);

		spin_lock_irqsave(&ncpu->target_freq_lock, flags);

		if (policy->max < ncpu->target_freq)
			ncpu->target_freq = policy->max;
		else if (policy->min > ncpu->target_freq)
			ncpu->target_freq = policy->min;

		spin_unlock_irqrestore(&ncpu->target_freq_lock, flags);
	}
}

static struct nerv_governor nerv_gov = {
	.gov = {
		.name			= "NerV",
		.owner			= THIS_MODULE,
		.init			= cpufreq_nerV_init,
		.exit			= cpufreq_nerV_exit,
		.start			= cpufreq_nerV_start,
		.stop			= cpufreq_nerV_stop,
		.limits			= cpufreq_nerV_limits,
	}
};

#define CPU_FREQ_GOV_NERV	(&nerv_gov.gov)

static int __init cpufreq_nerV_gov_init(void)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct nerv_cpu *ncpu;
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		ncpu = &per_cpu(nerv_cpu, cpu);

		init_irq_work(&ncpu->irq_work, irq_work);
		spin_lock_init(&ncpu->load_lock);
		spin_lock_init(&ncpu->target_freq_lock);
		init_rwsem(&ncpu->enable_sem);
	}

	spin_lock_init(&speedchange_cpumask_lock);
	speedchange_task = kthread_create(nerv_speedchange_task, NULL,
					  "cfnerV");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	wake_up_process(speedchange_task);

	return cpufreq_register_governor(CPU_FREQ_GOV_NERV);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_NERV
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_NERV;
}

fs_initcall(cpufreq_nerV_gov_init);
#else
module_init(cpufreq_nerV_gov_init);
#endif

static void __exit cpufreq_nerV_gov_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_NERV);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);
}
module_exit(cpufreq_nerV_gov_exit);

MODULE_AUTHOR("Aciss21 <wawa22arpk@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_nerV' - Busy-time cpufreq governor for multitasking, gaming and daily use");
MODULE_LICENSE("GPL");
