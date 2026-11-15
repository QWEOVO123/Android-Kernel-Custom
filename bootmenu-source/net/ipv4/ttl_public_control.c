// SPDX-License-Identifier: GPL-2.0-only
/* Public alias for this kernel's per-netns IPv4 forwarding TTL policy. */

#include <linux/qweovo_bootmenu.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/nsproxy.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ttl_public_control.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>

#define TTL_CTL_MAX_INPUT 32

static char ttl_ctl_inode_marker;

bool ttl_public_control_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &ttl_ctl_inode_marker;
}

static int ttl_ctl_show(struct seq_file *m, void *unused)
{
	struct net *net = m->private;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	seq_printf(m, "%d\n", READ_ONCE(net->ipv4.sysctl_ip_forward_ttl_mode));
	return 0;
}

static ssize_t ttl_ctl_write(struct file *file, const char __user *input,
			     size_t count, loff_t *position)
{
	struct seq_file *m = file->private_data;
	struct net *net = m->private;
	char *buffer, *value;
	int mode, ret = -EINVAL;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > TTL_CTL_MAX_INPUT)
		return -E2BIG;
	buffer = memdup_user_nul(input, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (memchr(buffer, '\0', count))
		goto out;
	value = strim(buffer);
	if (!strcmp(value, "0"))
		mode = 0;
	else if (!strcmp(value, "1"))
		mode = 1;
	else
		goto out;

	WRITE_ONCE(net->ipv4.sysctl_ip_forward_ttl_mode, mode);
	ret = 0;
out:
	kfree(buffer);
	if (ret)
		return ret;
	return count;
}

static int ttl_ctl_open(struct inode *inode, struct file *file)
{
	struct net *net = get_net(current->nsproxy->net_ns);
	int ret;

	ret = single_open(file, ttl_ctl_show, net);
	if (ret)
		put_net(net);
	return ret;
}

static int ttl_ctl_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct net *net = m->private;
	int ret;

	ret = single_release(inode, file);
	put_net(net);
	return ret;
}

static const struct proc_ops ttl_ctl_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = ttl_ctl_open,
	.proc_read = seq_read,
	.proc_write = ttl_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = ttl_ctl_release,
};

static int __init ttl_ctl_init(void)
{
	if (!qweovo_manager_proc_create("ttl_ctl", 0666, &ttl_ctl_ops,
					&ttl_ctl_inode_marker))
		return -ENOMEM;
	return 0;
}
device_initcall(ttl_ctl_init);
