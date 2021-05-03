// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "qcom-pmu: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/spinlock.h>
#include <linux/perf_event.h>
#include <linux/cpuidle.h>
#include <trace/events/power.h>
#include <trace/hooks/cpuidle.h>
#include <soc/qcom/pmu_lib.h>
#include <soc/qcom/qcom_llcc_pmu.h>

#define MAX_PMU_EVS	QCOM_PMU_MAX_EVS

struct event_data {
	u32			event_id;
	struct perf_event	*pevent;
	int			cpu;
	u64			cached_count;
	u32			ref_cnt;
};

struct cpu_data {
	bool			is_idle;
	bool			is_hp;
	struct event_data	events[MAX_PMU_EVS];
	u32			num_evs;
	atomic_t		read_cnt;
	spinlock_t		read_lock;
};

static DEFINE_PER_CPU(struct cpu_data *, cpu_ev_data);
static bool qcom_pmu_inited;
static int cpuhp_state;
static LIST_HEAD(idle_notif_list);
static DEFINE_SPINLOCK(idle_list_lock);

static struct perf_event_attr *alloc_attr(void)
{
	struct perf_event_attr *attr;

	attr = kzalloc(sizeof(struct perf_event_attr), GFP_KERNEL);
	if (!attr)
		return attr;

	attr->size = sizeof(struct perf_event_attr);
	attr->pinned = 1;

	return attr;
}

static int set_event(struct event_data *ev, int cpu,
			     struct perf_event_attr *attr)
{
	struct perf_event *pevent;
	u32 type = PERF_TYPE_RAW;
	int ret;

	if (!ev->event_id)
		return 0;

	attr->config = ev->event_id;
	if (ev->event_id == QCOM_LLCC_PMU_RD_EV) {
		ret = qcom_llcc_pmu_hw_type(&type);
		if (ret < 0)
			return ret;
	}
	attr->type = type;
	pevent = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(pevent))
		return PTR_ERR(pevent);

	perf_event_enable(pevent);
	ev->pevent = pevent;
	ev->cpu = cpu;

	return 0;
}

static inline void delete_event(struct event_data *event)
{
	perf_event_release_kernel(event->pevent);
	event->pevent = NULL;
}

static inline u64 read_event(struct event_data *event, bool local)
{
	u64 enabled, running, total = 0;

	if (local)
		perf_event_read_local(event->pevent, &total, NULL, NULL);
	else
		total = perf_event_read_value(event->pevent, &enabled,
								&running);
	event->cached_count = total;

	return total;
}

static int __qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data, bool local)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i;
	unsigned long flags;

	if (!qcom_pmu_inited)
		return -ENODEV;

	if (!event_id || !pmu_data || cpu >= num_possible_cpus())
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);
	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (event->event_id == event_id)
			break;
	}
	if (i == cpu_data->num_evs)
		return -ENOENT;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_hp || cpu_data->is_idle) {
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		*pmu_data = event->cached_count;
		return 0;
	}
	atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	*pmu_data = read_event(event, local);
	atomic_dec(&cpu_data->read_cnt);

	return 0;
}

int __qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data, bool local)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i, cnt = 0;
	bool use_cache = false;
	unsigned long flags;

	if (!qcom_pmu_inited)
		return -ENODEV;

	if (!data || cpu >= num_possible_cpus())
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);
	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_hp || cpu_data->is_idle)
		use_cache = true;
	else
		atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);

	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (!event->event_id)
			continue;
		data->event_ids[cnt] = event->event_id;
		if (use_cache)
			data->ev_data[cnt] = event->cached_count;
		else
			data->ev_data[cnt] = read_event(event, local);
		cnt++;
	}
	data->num_evs = cnt;

	if (!use_cache)
		atomic_dec(&cpu_data->read_cnt);

	return 0;
}

int qcom_pmu_event_supported(u32 event_id, int cpu)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i;

	if (!qcom_pmu_inited)
		return -EPROBE_DEFER;

	if (!event_id || cpu >= num_possible_cpus())
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);
	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (event->event_id == event_id)
			return 0;
	}

	return -ENOENT;
}
EXPORT_SYMBOL(qcom_pmu_event_supported);

int qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data)
{
	return __qcom_pmu_read(cpu, event_id, pmu_data, false);
}
EXPORT_SYMBOL(qcom_pmu_read);

int qcom_pmu_read_local(u32 event_id, u64 *pmu_data)
{
	int this_cpu = smp_processor_id();

	return __qcom_pmu_read(this_cpu, event_id, pmu_data, true);
}
EXPORT_SYMBOL(qcom_pmu_read_local);

int qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data)
{
	return __qcom_pmu_read_all(cpu, data, false);
}
EXPORT_SYMBOL(qcom_pmu_read_all);

int qcom_pmu_read_all_local(struct qcom_pmu_data *data)
{
	int this_cpu = smp_processor_id();

	return __qcom_pmu_read_all(this_cpu, data, true);
}
EXPORT_SYMBOL(qcom_pmu_read_all_local);

int qcom_pmu_idle_register(struct qcom_pmu_notif_node *idle_node)
{
	struct qcom_pmu_notif_node *tmp_node;

	if (!idle_node || !idle_node->idle_cb)
		return -EINVAL;

	spin_lock(&idle_list_lock);
	list_for_each_entry(tmp_node, &idle_notif_list, node)
		if (tmp_node->idle_cb == idle_node->idle_cb)
			goto out;
	list_add_tail(&idle_node->node, &idle_notif_list);
out:
	spin_unlock(&idle_list_lock);
	return 0;
}
EXPORT_SYMBOL(qcom_pmu_idle_register);

int qcom_pmu_idle_unregister(struct qcom_pmu_notif_node *idle_node)
{
	struct qcom_pmu_notif_node *tmp_node;
	int ret = -EINVAL;

	if (!idle_node || !idle_node->idle_cb)
		return ret;

	spin_lock(&idle_list_lock);
	list_for_each_entry(tmp_node, &idle_notif_list, node) {
		if (tmp_node->idle_cb == idle_node->idle_cb) {
			list_del(&tmp_node->node);
			ret = 0;
			break;
		}
	}
	spin_unlock(&idle_list_lock);
	return ret;
}
EXPORT_SYMBOL(qcom_pmu_idle_unregister);

static void qcom_pmu_idle_enter_notif(void *unused, int *state,
				      struct cpuidle_device *dev)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, dev->cpu);
	struct qcom_pmu_data pmu_data;
	struct event_data *ev;
	struct qcom_pmu_notif_node *idle_node;
	int i, cnt = 0;
	unsigned long flags;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_idle || cpu_data->is_hp) {
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		return;
	}
	cpu_data->is_idle = true;
	atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	for (i = 0; i < cpu_data->num_evs; i++) {
		ev = &cpu_data->events[i];
		if (!ev->event_id || !ev->pevent)
			continue;
		ev->cached_count = read_event(ev, true);
		pmu_data.event_ids[cnt] = ev->event_id;
		pmu_data.ev_data[cnt] = ev->cached_count;
		cnt++;
	}
	atomic_dec(&cpu_data->read_cnt);
	pmu_data.num_evs = cnt;

	/* send snapshot of pmu data to all registered idle clients */
	list_for_each_entry(idle_node, &idle_notif_list, node)
		idle_node->idle_cb(&pmu_data, dev->cpu, *state);
}

static void qcom_pmu_idle_exit_notif(void *unused, int state,
				     struct cpuidle_device *dev)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, dev->cpu);

	cpu_data->is_idle = false;
}

#if IS_ENABLED(CONFIG_HOTPLUG_CPU)
static int qcom_pmu_hotplug_coming_up(unsigned int cpu)
{
	struct perf_event_attr *attr = alloc_attr();
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, cpu);
	int i, ret = 0;
	unsigned long flags;

	if (!attr)
		return -ENOMEM;

	if (!qcom_pmu_inited)
		goto out;

	for (i = 0; i < cpu_data->num_evs; i++) {
		ret = set_event(&cpu_data->events[i], cpu, attr);
		if (ret < 0) {
			pr_err("event %d not set for cpu %d ret %d\n",
				cpu_data->events[i].event_id, cpu, ret);
			break;
		}
	}
	spin_lock_irqsave(&cpu_data->read_lock, flags);
	cpu_data->is_hp = false;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);

out:
	kfree(attr);
	return 0;
}

static int qcom_pmu_hotplug_going_down(unsigned int cpu)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, cpu);
	struct event_data *event;
	int i;
	unsigned long flags;

	if (!qcom_pmu_inited)
		return 0;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	cpu_data->is_hp = true;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	while (atomic_read(&cpu_data->read_cnt) > 0)
		udelay(10);
	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (!event->event_id || !event->pevent)
			continue;
		event->cached_count = read_event(event, false);
		delete_event(event);
	}

	return 0;
}

static int qcom_pmu_cpu_hp_init(void)
{
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
				"QCOM_PMU",
				qcom_pmu_hotplug_coming_up,
				qcom_pmu_hotplug_going_down);
	if (ret < 0)
		pr_err("qcom_pmu: CPU hotplug notifier error: %d\n",
		       ret);

	return ret;
}
#else
static int qcom_pmu_cpu_hp_init(void) { return 0; }
#endif

static int configure_pmu_event(u32 event_id, int cpu)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	struct perf_event_attr *attr = alloc_attr();
	int ret = 0;

	if (!attr)
		return -ENOMEM;

	if (!event_id || cpu >= num_possible_cpus()) {
		ret = -EINVAL;
		goto out;
	}

	cpu_data = per_cpu(cpu_ev_data, cpu);
	if (cpu_data->num_evs >= MAX_PMU_EVS) {
		ret = -ENOSPC;
		goto out;
	}
	event = &cpu_data->events[cpu_data->num_evs];
	event->event_id = event_id;
	ret = set_event(event, cpu, attr);
	if (ret < 0)
		event->event_id = 0;
	else
		cpu_data->num_evs++;

out:
	kfree(attr);
	return ret;
}

#define PMU_TBL_PROP	"qcom,pmu-events-tbl"
#define NUM_COLS	2
static int setup_pmu_events(struct device *dev)
{
	struct device_node *of_node = dev->of_node;
	int ret, len, i, j, cpu;
	u32 data, event_id;
	unsigned long cpus;

	if (!of_find_property(of_node, PMU_TBL_PROP, &len))
		return -ENODEV;
	len /= sizeof(data);
	if (len % NUM_COLS || len == 0)
		return -EINVAL;
	len /= NUM_COLS;
	if (len >= MAX_PMU_EVS)
		return -ENOSPC;

	for (i = 0, j = 0; i < len; i++, j += 2) {
		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j,
							&event_id);
		if (ret < 0 || !event_id)
			return -EINVAL;

		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j + 1,
							&data);
		if (ret < 0 || !data)
			return -EINVAL;
		cpus = (unsigned long)data;

		dev_dbg(dev, "entry=%d: ev=%lu, cpus=%lu\n", i, event_id, cpus);
		for_each_cpu(cpu, to_cpumask(&cpus)) {
			ret = configure_pmu_event(event_id, cpu);
			if (!ret)
				continue;
			else if (ret != -EPROBE_DEFER)
				dev_err(dev, "error enabling ev=%d on cpu%d\n",
								event_id, cpu);
			return ret;
		}
	}

	return 0;
}

static int qcom_pmu_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	unsigned int cpu;
	struct cpu_data *cpu_data;

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		cpu_data = devm_kzalloc(dev, sizeof(*cpu_data), GFP_KERNEL);
		if (!cpu_data) {
			ret = -ENOMEM;
			goto out;
		}
		if (!cpumask_test_cpu(cpu, cpu_online_mask))
			cpu_data->is_hp = true;
		spin_lock_init(&cpu_data->read_lock);
		atomic_set(&cpu_data->read_cnt, 0);
		per_cpu(cpu_ev_data, cpu) = cpu_data;
	}

	ret = setup_pmu_events(dev);
	if (ret < 0) {
		dev_err(dev, "failed to setup pmu events: %d\n", ret);
		goto out;
	}

	cpuhp_state = qcom_pmu_cpu_hp_init();
	if (cpuhp_state < 0) {
		ret = cpuhp_state;
		dev_err(dev, "qcom pmu driver failed to probe: %d\n", ret);
		goto out;
	}

	register_trace_android_vh_cpu_idle_enter(qcom_pmu_idle_enter_notif, NULL);
	register_trace_android_vh_cpu_idle_exit(qcom_pmu_idle_exit_notif, NULL);
	qcom_pmu_inited = true;

out:
	put_online_cpus();
	return ret;
}

static int qcom_pmu_driver_remove(struct platform_device *pdev)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int cpu, i;
	unsigned long flags;

	qcom_pmu_inited = false;
	if (cpuhp_state > 0)
		cpuhp_remove_state_nocalls(cpuhp_state);
	unregister_trace_android_vh_cpu_idle_enter(qcom_pmu_idle_enter_notif, NULL);
	unregister_trace_android_vh_cpu_idle_exit(qcom_pmu_idle_exit_notif, NULL);
	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		spin_lock_irqsave(&cpu_data->read_lock, flags);
		cpu_data->is_hp = true;
		cpu_data->is_idle = true;
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	}

	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		while (atomic_read(&cpu_data->read_cnt) > 0)
			udelay(10);
		for (i = 0; i < cpu_data->num_evs; i++) {
			event = &cpu_data->events[i];
			if (!event->event_id || !event->pevent)
				continue;
			event->event_id = 0;
			delete_event(event);
			event->cached_count = 0;
			event->ref_cnt = 0;
		}
		cpu_data->num_evs = 0;
	}

	return 0;
}

static const struct of_device_id pmu_match_table[] = {
	{ .compatible = "qcom,pmu" },
	{}
};

static struct platform_driver qcom_pmu_driver = {
	.probe = qcom_pmu_driver_probe,
	.remove = qcom_pmu_driver_remove,
	.driver = {
		.name = "qcom-pmu",
		.of_match_table = pmu_match_table,
		.suppress_bind_attrs = true,
	},
};

static int __init qcom_pmu_init(void)
{
	return platform_driver_register(&qcom_pmu_driver);
}
module_init(qcom_pmu_init);

static __exit void qcom_pmu_exit(void)
{
	platform_driver_unregister(&qcom_pmu_driver);
}
module_exit(qcom_pmu_exit);

MODULE_DESCRIPTION("QCOM PMU Driver");
MODULE_LICENSE("GPL v2");
