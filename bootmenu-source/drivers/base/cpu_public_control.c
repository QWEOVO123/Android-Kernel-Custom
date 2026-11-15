// SPDX-License-Identifier: GPL-2.0-only
/* Deliberately public CPU hotplug control for this custom kernel. */

#include <linux/qweovo_bootmenu.h>
#include <linux/cpu.h>
#include <linux/cpu_public_control.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define CPU_CTL_MAX_INPUT 4095

/* The marker identifies our inode independently of its path or SELinux label. */
static char cpu_ctl_inode_marker;

bool cpu_public_control_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &cpu_ctl_inode_marker;
}

static int cpu_ctl_show(struct seq_file *m, void *unused)
{
	cpumask_var_t hotpluggable;
	unsigned int cpu;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!zalloc_cpumask_var(&hotpluggable, GFP_KERNEL))
		return -ENOMEM;

	cpus_read_lock();
	for_each_present_cpu(cpu) {
		if (cpu_is_hotpluggable(cpu))
			cpumask_set_cpu(cpu, hotpluggable);
	}
	seq_printf(m, "possible: %*pbl\n", cpumask_pr_args(cpu_possible_mask));
	seq_printf(m, "present: %*pbl\n", cpumask_pr_args(cpu_present_mask));
	seq_printf(m, "online: %*pbl\n", cpumask_pr_args(cpu_online_mask));
	seq_printf(m, "active: %*pbl\n", cpumask_pr_args(cpu_active_mask));
	seq_printf(m, "hotpluggable: %*pbl\n", cpumask_pr_args(hotpluggable));
	cpus_read_unlock();
	free_cpumask_var(hotpluggable);

	return 0;
}

/* Called with device_hotplug_lock held, just like the standard sysfs path. */
static int cpu_ctl_change_cpu(unsigned int cpu, bool online)
{
	struct device *dev;
	int ret;

	if (cpu >= nr_cpu_ids || !cpu_present(cpu))
		return -ENODEV;
	dev = get_cpu_device(cpu);
	if (!dev)
		return -ENODEV;
	if (!device_supports_offline(dev))
		return -EOPNOTSUPP;

	/* device_offline() reaches the core last-active-CPU check. */
	ret = online ? device_online(dev) : device_offline(dev);
	return ret < 0 ? ret : 0;
}

static int cpu_ctl_set_mask(const struct cpumask *target)
{
	unsigned int cpu;
	int ret;

	if (cpumask_empty(target) || !cpumask_subset(target, cpu_present_mask))
		return -EINVAL;

	/* Reject removal of protected CPUs before making any changes. */
	for_each_online_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!cpumask_test_cpu(cpu, target) &&
		    (!dev || dev->offline_disabled))
			return -EPERM;
	}

	/* Bring the replacement CPUs up before taking any existing CPU down. */
	for_each_cpu(cpu, target) {
		if (cpu_online(cpu))
			continue;
		ret = cpu_ctl_change_cpu(cpu, true);
		if (ret)
			return ret;
	}
	for_each_online_cpu(cpu) {
		if (cpumask_test_cpu(cpu, target))
			continue;
		ret = cpu_ctl_change_cpu(cpu, false);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t cpu_ctl_write(struct file *file, const char __user *input,
			     size_t count, loff_t *position)
{
	cpumask_var_t target;
	char *buffer, *command, *argument;
	unsigned int cpu = 0;
	bool set_mask, online;
	int ret;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > CPU_CTL_MAX_INPUT)
		return -E2BIG;
	buffer = memdup_user_nul(input, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	ret = -EINVAL;
	if (memchr(buffer, '\0', count))
		goto out_buffer;
	command = strim(buffer);
	if (strpbrk(command, "\r\n"))
		goto out_buffer;
	argument = strpbrk(command, " \t");
	if (!argument)
		goto out_buffer;
	*argument++ = '\0';
	argument = skip_spaces(argument);
	if (!*argument)
		goto out_buffer;

	set_mask = !strcmp(command, "set");
	online = !strcmp(command, "online");
	if (!set_mask && !online && strcmp(command, "offline"))
		goto out_buffer;
	if (set_mask && strspn(argument, "0123456789,-") != strlen(argument))
		goto out_buffer;
	if (!zalloc_cpumask_var(&target, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out_buffer;
	}
	ret = set_mask ? cpulist_parse(argument, target) :
		kstrtouint(argument, 10, &cpu);
	if (ret)
		goto out_mask;

	ret = lock_device_hotplug_sysfs();
	if (ret)
		goto out_mask;
	ret = set_mask ? cpu_ctl_set_mask(target) : cpu_ctl_change_cpu(cpu, online);
	unlock_device_hotplug();

out_mask:
	free_cpumask_var(target);
out_buffer:
	kfree(buffer);
	if (ret)
		return ret;
	return count;
}

static int cpu_ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpu_ctl_show, NULL);
}

static const struct proc_ops cpu_ctl_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = cpu_ctl_open,
	.proc_read = seq_read,
	.proc_write = cpu_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init cpu_ctl_init(void)
{
	if (!qweovo_manager_proc_create("cpu_ctl", 0666, &cpu_ctl_ops,
					&cpu_ctl_inode_marker))
		return -ENOMEM;
	return 0;
}
device_initcall(cpu_ctl_init);
