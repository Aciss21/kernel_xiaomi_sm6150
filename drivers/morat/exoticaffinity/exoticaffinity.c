// SPDX-License-Identifier: GPL-2.0
/*
 * ExoticAffinity - part of Morat Engine
 * Locks specific critical tasks to LITTLE cores for improved jitter and latency stability
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

#define AFFINITY_INTERVAL (60 * HZ) 

static struct task_struct *affinity_task;
static DECLARE_WAIT_QUEUE_HEAD(affinity_wq);
static bool affinity_should_stop;

static const char *target_tasks[] = {
	"surfaceflinger",
	"audioserver",
	"mediaserver",
	"hwcomposer",
	"vendor.mediaserver",
	"vendor.audio-hal",
	"vendor.audio",
	NULL
};

static int get_little_cores(cpumask_var_t mask)
{
	int cpu;
	unsigned int min_freq = ~0U;

	cpumask_clear(mask);

	for_each_online_cpu(cpu) {
		unsigned int freq = cpu_khz;
		if (freq && freq < min_freq)
			min_freq = freq;
	}

	for_each_online_cpu(cpu) {
		if (cpu_khz <= min_freq + 100000)
			cpumask_set_cpu(cpu, mask);
	}

	return cpumask_weight(mask);
}

static void apply_affinity(void)
{
	struct task_struct *p;
	cpumask_var_t little_mask;

	if (!alloc_cpumask_var(&little_mask, GFP_KERNEL))
		return;

	if (get_little_cores(little_mask) == 0)
		goto out;

	rcu_read_lock();
	for_each_process(p) {
		const char **tname = target_tasks;
		while (*tname) {
			if (strnstr(p->comm, *tname, TASK_COMM_LEN)) {
				set_cpus_allowed_ptr(p, little_mask);
				break;
			}
			tname++;
		}
	}
	rcu_read_unlock();

out:
	free_cpumask_var(little_mask);
}

static int affinity_thread(void *data)
{
	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(affinity_wq,
			affinity_should_stop, AFFINITY_INTERVAL);

		if (affinity_should_stop)
			break;

		apply_affinity();
	}

	return 0;
}

static int __init exoticaffinity_init(void)
{
	pr_info("ExoticAffinity: Module loaded and active\n");

	affinity_task = kthread_run(affinity_thread, NULL, "exoticaffinity");
	if (IS_ERR(affinity_task)) {
		pr_err("ExoticAffinity: Failed to start thread\n");
		return PTR_ERR(affinity_task);
	}

	return 0;
}

static void __exit exoticaffinity_exit(void)
{
	pr_info("ExoticAffinity: Module unloaded\n");
	affinity_should_stop = true;
	wake_up(&affinity_wq);
	if (affinity_task)
		kthread_stop(affinity_task);
}

module_init(exoticaffinity_init);
module_exit(exoticaffinity_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mr. Morat");
MODULE_DESCRIPTION("ExoticAffinity: Lock critical tasks to LITTLE cores");