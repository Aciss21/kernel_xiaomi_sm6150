// SPDX-License-Identifier: GPL-2.0
/*
 * Morat Engine - ExoticAppBoost
 * Boost slow-starting, user-installed apps to big core under safe thermal and battery conditions.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm_types.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/ktime.h>
#include <linux/power_supply.h>
#include <linux/cpu.h>

#define BOOST_DURATION_MS     2000
#define BOOST_INTERVAL_MS      300
#define UID_APP_START         10000
#define MAX_BOOSTED_TASKS        64
#define TEMP_LIMIT_C           42
#define BATT_MIN_PERCENT       20

static struct task_struct *boost_thread;
static int big_core = -1;

struct boosted_task {
	pid_t pid;
	ktime_t last_boost;
};

static struct boosted_task boosted[MAX_BOOSTED_TASKS];
static int boost_count = 0;

static int cpu_max_freq(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	int freq = 0;
	if (policy) {
		freq = policy->cpuinfo.max_freq;
		cpufreq_cpu_put(policy);
	}
	return freq;
}

static bool is_safe_condition(void)
{
	struct power_supply *psy = power_supply_get_by_name("battery");
	union power_supply_propval val;

	if (!psy)
		return false;

	// Cek suhu
	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val)) {
		if (val.intval >= TEMP_LIMIT_C * 10)
			return false;
	}

	// Cek kapasitas baterai
	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &val)) {
		if (val.intval <= BATT_MIN_PERCENT)
			return false;
	}

	return true;
}

static bool was_recently_boosted(pid_t pid)
{
	ktime_t now = ktime_get();
	for (int i = 0; i < boost_count; i++) {
		if (boosted[i].pid == pid) {
			if (ktime_ms_delta(now, boosted[i].last_boost) < BOOST_DURATION_MS)
				return true;

			boosted[i].last_boost = now;
			return false;
		}
	}
	if (boost_count < MAX_BOOSTED_TASKS) {
		boosted[boost_count].pid = pid;
		boosted[boost_count].last_boost = now;
		boost_count++;
	}
	return false;
}

static bool is_user_installed_app(struct task_struct *p)
{
	const struct cred *cred = p->cred;
	if (!p->mm || !cred)
		return false;

	uid_t uid = from_kuid(&init_user_ns, cred->uid);
	return uid >= UID_APP_START;
}

static bool is_recent_install(struct task_struct *p)
{
	// Simulasi: anggap semua baru diinstall <24 jam
	return true;
}

static bool is_launch_slow(struct task_struct *p)
{
	// Simulasi: anggap semua app lambat launch > 2 detik
	return true;
}

static void boost_task_to_big_core(struct task_struct *p)
{
	struct cpumask mask;

	if (!p || big_core < 0 || big_core >= nr_cpu_ids)
		return;

	cpumask_clear(&mask);
	cpumask_set_cpu(big_core, &mask);

	set_cpus_allowed_ptr(p, &mask);
	pr_info("ExoticAppBoost: Boosted %s (pid %d) to CPU%d\n", p->comm, p->pid, big_core);
}

static int boost_thread_fn(void *data)
{
	while (!kthread_should_stop()) {
		struct task_struct *p;

		if (!is_safe_condition()) {
			msleep(BOOST_INTERVAL_MS);
			continue;
		}

		rcu_read_lock();
		for_each_process(p) {
			if (p->flags & PF_KTHREAD || !p->mm || !p->group_leader || p->exit_state)
				continue;

			if (!is_user_installed_app(p))
				continue;

			if (!is_recent_install(p))
				continue;

			if (!is_launch_slow(p))
				continue;

			if (was_recently_boosted(p->pid))
				continue;

			boost_task_to_big_core(p);
		}
		rcu_read_unlock();

		msleep(BOOST_INTERVAL_MS);
	}
	return 0;
}

static int __init exotic_appboost_init(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu_max_freq(cpu) >= 2000000) {
			big_core = cpu;
			break;
		}
	}

	if (big_core == -1) {
		pr_info("ExoticAppBoost: No big core found\n");
		return -ENODEV;
	}

	boost_thread = kthread_run(boost_thread_fn, NULL, "exotic_appboost");
	if (IS_ERR(boost_thread)) {
		pr_info("ExoticAppBoost: Failed to start thread\n");
		return PTR_ERR(boost_thread);
	}

	pr_info("ExoticAppBoost: Initialized (boosting to CPU%d)\n", big_core);
	return 0;
}

static void __exit exotic_appboost_exit(void)
{
	if (boost_thread)
		kthread_stop(boost_thread);
	pr_info("ExoticAppBoost: Unloaded\n");
}

module_init(exotic_appboost_init);
module_exit(exotic_appboost_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mr. Morat");
MODULE_DESCRIPTION("Morat Engine - Boost installed apps at launch time");