// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/qweovo_bootmenu.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define AUDIT_FILE "qweovo-security-audit-v1"
#define AUDIT_SLOT 128
#define AUDIT_LIMIT (128 * AUDIT_SLOT)

static DEFINE_MUTEX(audit_lock);
static DEFINE_RATELIMIT_STATE(audit_rate, HZ, 8);
static struct file *audit_store;
static char audit_marker;

bool qweovo_security_audit_available(void)
{
	return READ_ONCE(audit_store) != NULL;
}

bool qweovo_security_audit_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &audit_marker;
}

void qweovo_security_audit_prepare(void)
{
	struct file *dir, *store;
	struct inode *inode;

	dir = filp_open("/metadata", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
	if (IS_ERR(dir))
		return;
	if (dir->f_path.dentry != dir->f_path.mnt->mnt_root ||
	    file_inode(dir)->i_sb->s_magic != EXT4_SUPER_MAGIC)
		goto close_dir;
	store = file_open_root(dir->f_path.dentry, dir->f_path.mnt, AUDIT_FILE,
			       O_RDWR | O_NOFOLLOW | O_NONBLOCK |
			       (qweovo_bootmenu_audit_enabled() ? O_CREAT : 0), 0600);
	if (IS_ERR(store))
		goto close_dir;
	inode = file_inode(store);
	if (store->f_path.mnt != dir->f_path.mnt || !S_ISREG(inode->i_mode) ||
	    inode->i_nlink != 1 || !uid_eq(inode->i_uid, GLOBAL_ROOT_UID) ||
	    (inode->i_mode & 07777) != 0600 || i_size_read(inode) > AUDIT_LIMIT ||
	    i_size_read(inode) % AUDIT_SLOT ||
	    (qweovo_bootmenu_audit_enabled() && vfs_fsync(dir, 0))) {
		fput(store);
		goto close_dir;
	}
	mutex_lock(&audit_lock);
	WRITE_ONCE(audit_store, store);
	mutex_unlock(&audit_lock);
close_dir:
	fput(dir);
}

/* The only backing file is opened and validated by PID 1 before policy load. */
static int audit_write_slot(const char *slot, loff_t offset)
{
	ssize_t written;

	file_start_write(audit_store);
	written = __kernel_write(audit_store, slot, AUDIT_SLOT, &offset);
	file_end_write(audit_store);
	return written == AUDIT_SLOT ? 0 : written < 0 ? written : -EIO;
}

static int audit_clear_locked(void)
{
	const char zero[AUDIT_SLOT] = {};
	loff_t end = i_size_read(file_inode(audit_store)), pos;
	int ret;

	for (pos = 0; pos < end; pos += AUDIT_SLOT) {
		ret = audit_write_slot(zero, pos);
		if (ret)
			return ret;
	}
	return vfs_fsync(audit_store, 0);
}

static loff_t audit_next_offset;
static bool audit_offset_loaded;
static atomic64_t pipe_merge_blocked = ATOMIC64_INIT(0);
static u64 pipe_merge_reported;

/* audit_lock serializes writes, reads and clearing. */
static int audit_append_locked(const char *slot)
{
	loff_t pos;
	int ret;

	if (!audit_store)
		return 0;
	if (!audit_offset_loaded) {
		char previous[AUDIT_SLOT];

		audit_next_offset = i_size_read(file_inode(audit_store));
		while (audit_next_offset > 0) {
			loff_t last = audit_next_offset - AUDIT_SLOT;

			if (__kernel_read(audit_store, previous, AUDIT_SLOT, &last) != AUDIT_SLOT ||
			    previous[0])
				break;
			audit_next_offset -= AUDIT_SLOT;
		}
		audit_offset_loaded = true;
	}
	pos = audit_next_offset;
	if (pos >= AUDIT_LIMIT) {
		ret = audit_clear_locked();
		if (ret)
			return ret;
		pos = 0;
	}
	ret = audit_write_slot(slot, pos);
	if (!ret) {
		audit_next_offset = pos + AUDIT_SLOT;
		ret = vfs_fsync(audit_store, 0);
	}
	return ret;
}

static void report_pipe_merges(struct work_struct *work)
{
	char slot[AUDIT_SLOT] = {};
	u64 total, delta;
	int ret = 0;

	mutex_lock(&audit_lock);
	total = atomic64_read(&pipe_merge_blocked);
	delta = total - pipe_merge_reported;
	if (delta) {
		scnprintf(slot, sizeof(slot), "%lld boot=%llu pipe-merge-blocked count=%llu\n",
			  (long long)ktime_get_real_seconds(),
			  (unsigned long long)ktime_get_boottime_seconds(),
			  (unsigned long long)delta);
		ret = audit_append_locked(slot);
		pipe_merge_reported = total;
	}
	mutex_unlock(&audit_lock);
	if (ret)
		pr_warn_ratelimited("qweovo: pipe audit write failed: %d\n", ret);
}

static DECLARE_DELAYED_WORK(pipe_merge_work, report_pipe_merges);

void qweovo_security_pipe_merge_blocked(void)
{
	atomic64_inc(&pipe_merge_blocked);
	if (qweovo_bootmenu_audit_enabled() && qweovo_security_audit_available())
		schedule_delayed_work(&pipe_merge_work, HZ);
}

u64 qweovo_security_pipe_merge_count(void)
{
	return atomic64_read(&pipe_merge_blocked);
}

void qweovo_security_audit(const char *event, int result)
{
	char slot[AUDIT_SLOT] = {}, comm[TASK_COMM_LEN];
	u32 sid;
	int i, ret;

	if (!qweovo_bootmenu_audit_enabled() || !__ratelimit(&audit_rate))
		return;
	get_task_comm(comm, current);
	for (i = 0; comm[i]; i++)
		if (comm[i] < ' ' || comm[i] > '~')
			comm[i] = '_';
	security_task_getsecid(current, &sid);
	scnprintf(slot, sizeof(slot), "%lld boot=%llu uid=%u sid=%u pid=%d comm=%s %s rc=%d\n",
		  (long long)ktime_get_real_seconds(), (unsigned long long)ktime_get_boottime_seconds(),
		  from_kuid(&init_user_ns, current_uid()), sid, task_pid_nr(current),
		  comm, event, result);
	mutex_lock(&audit_lock);
	ret = audit_append_locked(slot);
	mutex_unlock(&audit_lock);
	if (ret)
		pr_warn_ratelimited("qweovo: security audit write failed: %d\n", ret);
}

static int audit_show(struct seq_file *m, void *unused)
{
	char slot[AUDIT_SLOT];
	loff_t pos = 0, end;
	int ret = 0;

	mutex_lock(&audit_lock);
	if (!audit_store)
		goto out;
	end = i_size_read(file_inode(audit_store));
	while (pos < end) {
		ssize_t count = __kernel_read(audit_store, slot, sizeof(slot), &pos);

		if (count != sizeof(slot)) {
			ret = count < 0 ? count : -EIO;
			break;
		}
		if (slot[0])
			seq_write(m, slot, strnlen(slot, sizeof(slot)));
	}
out:
	mutex_unlock(&audit_lock);
	return ret;
}

static int audit_open(struct inode *inode, struct file *file)
{
	return single_open(file, audit_show, NULL);
}

static ssize_t audit_clear(struct file *file, const char __user *buffer,
			   size_t count, loff_t *pos)
{
	char command[6];
	u64 clear_through;
	int ret = 0;

	if (*pos || count != sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buffer, count))
		return -EFAULT;
	if (memcmp(command, "clear\n", count))
		return -EINVAL;
	mutex_lock(&audit_lock);
	/* Keep events after this clear separate from the previous batch. */
	clear_through = atomic64_read(&pipe_merge_blocked);
	if (audit_store)
		ret = audit_clear_locked();
	if (!ret) {
		audit_next_offset = 0;
		audit_offset_loaded = true;
		pipe_merge_reported = clear_through;
	}
	mutex_unlock(&audit_lock);
	return ret ? ret : count;
}

static const struct proc_ops audit_ops = {
	.proc_open = audit_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = audit_clear,
};

static int __init audit_init(void)
{
	return qweovo_manager_proc_create("qweovo_security_audit", 0600,
					  &audit_ops, &audit_marker) ? 0 : -ENOMEM;
}
device_initcall(audit_init);
