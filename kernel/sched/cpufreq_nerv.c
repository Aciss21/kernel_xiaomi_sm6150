// SPDX-License-Identifier: GPL-2.0
/*
 * CPUFreq governor "nerv"
 *
 * Skeleton governor based on the schedutil design, but fully
 * self-contained: it does not depend on external symbols that may
 * not exist in your tree (task_is_booster, cpu_perf_mask,
 * cpu_lp_mask, etc). Extend from here.
 */

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/sched/cpufreq.h>
#include <linux/slab.h>
#include <linux/module.h>

struct nerv_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int boost_pct; /* 0-100 extra freq target while boosted */
};

struct nerv_policy {
	struct cpufreq_policy *policy;

	struct nerv_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;

	unsigned int next_freq;
	unsigned int cached_raw_freq;

	/* set to true whenever the update thread should run */
	bool work_in_progress;

	bool limits_changed;
	bool need_freq_update;
};

struct nerv_cpu {
	struct update_util_data update_util;
	struct nerv_policy *np;

	unsigned int iowait_boost;
	u64 last_update;

	/* per-cpu userspace/task boost hint, self-contained (no
	 * dependency on an external task_is_booster()).
	 * Set e.g. from a sysfs node or a scheduler hook you own.
	 */
	bool task_boosted;

	unsigned long util;
	unsigned long max;
};

static DEFINE_PER_CPU(struct nerv_cpu, nerv_cpu_data);

/* ---------------- utilization -> frequency ---------------- */

static unsigned int get_next_freq(struct nerv_policy *np,
				   unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = np->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	freq = map_util_freq(util, freq, max);

	if (freq == np->cached_raw_freq && !np->need_freq_update)
		return np->next_freq;

	np->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void nerv_apply_boost(struct nerv_cpu *nc, unsigned long *util,
			      unsigned long max)
{
	struct nerv_tunables *tunables = nc->np->tunables;
	unsigned long boosted;

	if (!nc->task_boosted || !tunables->boost_pct)
		return;

	boosted = *util + (max * tunables->boost_pct) / 100;
	*util = min(boosted, max);
}

/* ---------------- update_util callback ---------------- */

static void nerv_update_util(struct update_util_data *hook, u64 time,
			      unsigned int flags)
{
	struct nerv_cpu *nc = container_of(hook, struct nerv_cpu, update_util);
	struct nerv_policy *np = nc->np;
	unsigned long util, max;
	unsigned int next_f;
	unsigned long irq_flags;

	if (!raw_spin_trylock_irqsave(&np->update_lock, irq_flags))
		return;

	if (!np->need_freq_update &&
	    time - np->last_freq_update_time < np->min_rate_limit_ns)
		goto unlock;

	max = arch_scale_cpu_capacity(smp_processor_id());
	util = cpu_util_cfs(smp_processor_id());
	nerv_apply_boost(nc, &util, max);

	next_f = get_next_freq(np, util, max);

	if (next_f != np->next_freq) {
		np->next_freq = next_f;
		np->last_freq_update_time = time;
		__cpufreq_driver_target(np->policy, next_f, CPUFREQ_RELATION_L);
	}

unlock:
	raw_spin_unlock_irqrestore(&np->update_lock, irq_flags);
}

/* ---------------- sysfs tunables ---------------- */

static struct nerv_tunables *to_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct nerv_tunables, attr_set);
}

static ssize_t boost_pct_show(struct gov_attr_set *attr_set, char *buf)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->boost_pct);
}

static ssize_t boost_pct_store(struct gov_attr_set *attr_set,
				const char *buf, size_t count)
{
	struct nerv_tunables *tunables = to_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val > 100)
		return -EINVAL;

	tunables->boost_pct = val;
	return count;
}

static struct governor_attr boost_pct = __ATTR_RW(boost_pct);

static struct attribute *nerv_attrs[] = {
	&boost_pct.attr,
	NULL
};
ATTRIBUTE_GROUPS(nerv);

static struct kobj_type nerv_tunables_ktype = {
	.default_groups = nerv_groups,
	.sysfs_ops = &governor_sysfs_ops,
};

/* ---------------- governor start/stop ---------------- */

static int nerv_init(struct cpufreq_policy *policy)
{
	struct nerv_policy *np;
	struct nerv_tunables *tunables;
	int ret;

	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np)
		return -ENOMEM;

	np->policy = policy;
	raw_spin_lock_init(&np->update_lock);

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables) {
		kfree(np);
		return -ENOMEM;
	}

	tunables->up_rate_limit_us = 500;
	tunables->down_rate_limit_us = 2000;
	tunables->boost_pct = 20;

	gov_attr_set_init(&tunables->attr_set, &np->tunables_hook);

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				    &nerv_tunables_ktype,
				    get_governor_parent_kobj(policy),
				    "nerv");
	if (ret) {
		kfree(tunables);
		kfree(np);
		return ret;
	}

	np->tunables = tunables;
	np->min_rate_limit_ns = tunables->up_rate_limit_us * NSEC_PER_USEC;
	policy->governor_data = np;

	return 0;
}

static void nerv_exit(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;

	kobject_put(&np->tunables->attr_set.kobj);
	kfree(np->tunables);
	kfree(np);
	policy->governor_data = NULL;
}

static int nerv_start(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;
	unsigned int cpu;

	np->next_freq = 0;
	np->cached_raw_freq = 0;
	np->need_freq_update = true;
	np->last_freq_update_time = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct nerv_cpu *nc = &per_cpu(nerv_cpu_data, cpu);

		memset(nc, 0, sizeof(*nc));
		nc->np = np;
		cpufreq_add_update_util_hook(cpu, &nc->update_util,
					      nerv_update_util);
	}

	return 0;
}

static void nerv_stop(struct cpufreq_policy *policy)
{
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();
}

static void nerv_limits(struct cpufreq_policy *policy)
{
	struct nerv_policy *np = policy->governor_data;

	np->limits_changed = true;
}

static struct cpufreq_governor nerv_gov = {
	.name = "NeRv",
	.owner = THIS_MODULE,
	.flags = CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init = nerv_init,
	.exit = nerv_exit,
	.start = nerv_start,
	.stop = nerv_stop,
	.limits = nerv_limits,
};

cpufreq_governor_init(nerv_gov);

MODULE_AUTHOR("Aciss21");
MODULE_DESCRIPTION("'nerv' - self-contained cpufreq governor");
MODULE_LICENSE("GPL");
