// SPDX-License-Identifier: GPL-2.0-only
/* Boot-scoped authorization for the kernel-side MeowSU exec redirect. */
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/qweovo_bootmenu.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define GRANT_MAX 256
#define IDENTITY_SIZE 32
#define APPID_MIN 10000
#define APPID_MAX 19999
#define APPID_COUNT (APPID_MAX - APPID_MIN + 1)

struct meow_su_grant {
	u32 uid;
	u8 identity[IDENTITY_SIZE];
};

static struct meow_su_grant grants[GRANT_MAX];
/* Lock-free lookup for the syscall hook and task marking paths. */
static DECLARE_BITMAP(allowed_appids, APPID_COUNT);
static DEFINE_MUTEX(grants_lock);
static char grants_marker;

bool qweovo_meow_su_grants_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &grants_marker;
}

static int appid_index(u32 uid)
{
	u32 appid = uid % 100000;

	if (appid < APPID_MIN || appid > APPID_MAX)
		return -1;
	return appid - APPID_MIN;
}

bool qweovo_meow_su_uid_allowed(u32 uid)
{
	int index;

	if (!qweovo_bootmenu_meow_su_enabled())
		return false;
	index = appid_index(uid);
	return index >= 0 && test_bit(index, allowed_appids);
}

#if IS_ENABLED(CONFIG_KSU)
static void meow_su_mark_task(struct task_struct *task)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	set_task_syscall_work(task, SYSCALL_TRACEPOINT);
#else
	set_tsk_thread_flag(task, TIF_SYSCALL_TRACEPOINT);
#endif
}

/*
 * A grant can be added while the app is already running. Mark every live
 * thread of that app so the sys_enter tracepoint starts firing for it.
 */
static void meow_su_refresh_task_marks(void)
{
	struct task_struct *process, *thread;

	read_lock(&tasklist_lock);
	for_each_process_thread(process, thread) {
		if (!thread->mm)
			continue;
		if (qweovo_meow_su_uid_allowed(task_uid(thread).val))
			meow_su_mark_task(thread);
	}
	read_unlock(&tasklist_lock);
}
#else
static void meow_su_refresh_task_marks(void)
{
}
#endif

static int parse_grant(char *line, struct meow_su_grant *grant)
{
	char *uid = strsep(&line, " ");
	u32 appid;

	if (kstrtou32(uid, 10, &grant->uid) || !line ||
	    strlen(line) != IDENTITY_SIZE * 2)
		return -EINVAL;
	appid = grant->uid % 100000;
	if (appid < APPID_MIN || appid > APPID_MAX)
		return -EINVAL;
	return hex2bin(grant->identity, line, IDENTITY_SIZE);
}

static int find_uid(u32 uid)
{
	int i;

	for (i = 0; i < GRANT_MAX; ++i)
		if (grants[i].uid == uid)
			return i;
	return -ENOENT;
}

static int grants_show(struct seq_file *m, void *unused)
{
	int i;

	if (!qweovo_bootmenu_meow_su_enabled())
		return -EOPNOTSUPP;
	mutex_lock(&grants_lock);
	for (i = 0; i < GRANT_MAX; ++i)
		if (grants[i].uid)
			seq_printf(m, "%u %*phN\n", grants[i].uid,
				   IDENTITY_SIZE, grants[i].identity);
	mutex_unlock(&grants_lock);
	return 0;
}

static int grants_open(struct inode *inode, struct file *file)
{
	return single_open(file, grants_show, NULL);
}

static int read_grant(char *line, size_t size, const char __user *buffer, size_t count)
{
	if (count < 2 || count >= size)
		return -EINVAL;
	if (copy_from_user(line, buffer, count))
		return -EFAULT;
	if (line[count - 1] != '\n' || memchr(line, '\0', count))
		return -EINVAL;
	line[count - 1] = '\0';
	return 0;
}

static ssize_t grants_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *position)
{
	char line[88];
	struct meow_su_grant grant;
	int ret, index;

	if (!qweovo_bootmenu_meow_su_enabled())
		return -EOPNOTSUPP;
	ret = read_grant(line, sizeof(line), buffer, count);
	if (ret)
		return ret;
	if (line[1] != ' ' || (line[0] != '0' && line[0] != '1'))
		return -EINVAL;
	ret = parse_grant(line + 2, &grant);
	if (ret)
		return ret;
	mutex_lock(&grants_lock);
	index = find_uid(grant.uid);
	if (line[0] == '1') {
		if (index < 0)
			index = find_uid(0);
		if (index >= 0) {
			grants[index] = grant;
			set_bit(appid_index(grant.uid), allowed_appids);
		} else {
			ret = -ENOSPC;
		}
	} else if (index >= 0 &&
		   !memcmp(grants[index].identity, grant.identity, IDENTITY_SIZE)) {
		clear_bit(appid_index(grants[index].uid), allowed_appids);
		memset(&grants[index], 0, sizeof(grants[index]));
	}
	mutex_unlock(&grants_lock);
	if (!ret)
		meow_su_refresh_task_marks();
	return ret ? ret : count;
}

static const struct proc_ops grant_ops = {
	.proc_open = grants_open,
	.proc_read = seq_read,
	.proc_write = grants_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int qweovo_meow_su_create_nodes(void)
{
	return qweovo_manager_proc_create("qweovo_meow_su", 0600, &grant_ops,
					  &grants_marker) ? 0 : -ENOMEM;
}
