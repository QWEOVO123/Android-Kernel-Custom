// SPDX-License-Identifier: GPL-2.0-only
/* Public task metadata and explicitly delegated userspace nice control. */

#include <linux/qweovo_bootmenu.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/proc_fs.h>
#include <linux/process_public_control.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <uapi/linux/sched/types.h>

#define PROCESS_CTL_MAX_INPUT 128
#define PROCESS_CTL_MAX_THREADS 4096

static char process_info_marker;
static char process_ctl_marker;

struct process_public_context {
	struct pid_namespace *ns;
	struct mutex lock;
	unsigned int updated;
	int error;
};

bool process_public_control_inode(const struct inode *inode)
{
	void *marker;

	marker = proc_inode_data_or_null(inode);
	return marker == &process_info_marker || marker == &process_ctl_marker;
}

static struct task_struct *process_find_ge(struct pid_namespace *ns, loff_t *pos)
{
	struct task_struct *task = NULL;
	struct pid *pid;
	int nr;

	if (*pos > INT_MAX)
		return NULL;
	nr = *pos;
	rcu_read_lock();
	while ((pid = find_ge_pid(nr, ns))) {
		nr = pid_nr_ns(pid, ns);
		task = pid_task(pid, PIDTYPE_PID);
		if (task) {
			get_task_struct(task);
			*pos = nr;
			break;
		}
		nr++;
	}
	rcu_read_unlock();
	return task;
}

static void *process_info_start(struct seq_file *m, loff_t *pos)
{
	struct process_public_context *ctx = m->private;

	return *pos ? process_find_ge(ctx->ns, pos) : SEQ_START_TOKEN;
}

static void *process_info_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct process_public_context *ctx = m->private;

	if (v != SEQ_START_TOKEN)
		put_task_struct(v);
	++*pos;
	return process_find_ge(ctx->ns, pos);
}

static void process_info_stop(struct seq_file *m, void *v)
{
	if (v && v != SEQ_START_TOKEN)
		put_task_struct(v);
}

static void process_read_argv0(struct task_struct *task, char *buffer, int size)
{
	struct mm_struct *mm = get_task_mm(task);
	unsigned long start, end;
	int length;

	if (!mm)
		return;
	spin_lock(&mm->arg_lock);
	start = mm->arg_start;
	end = mm->arg_end;
	spin_unlock(&mm->arg_lock);
	if (end > start) {
		/* Pin the same mm across exec; never fall back into the environment. */
		length = access_remote_vm(mm, start, buffer,
					 min_t(unsigned long, end - start, size - 1), 0);
		if (length > 0)
			buffer[length] = '\0';
	}
	mmput(mm);
}

static int process_info_show(struct seq_file *m, void *v)
{
	struct process_public_context *ctx = m->private;
	struct task_struct *task = v;
	char comm[TASK_COMM_LEN], argv0[257] = { 0 };
	pid_t tid, tgid, ppid;
	u64 group_start;
	uid_t uid;

	if (v == SEQ_START_TOKEN) {
		seq_puts(m, "tid\ttgid\tppid\tuid\tstate\tpolicy\tnice\trt_priority\tcpu\tstart_ns\tgroup_start_ns\tkernel\tcomm\targv0\n");
		return 0;
	}
	rcu_read_lock();
	tid = task_pid_nr_ns(task, ctx->ns);
	tgid = task_tgid_nr_ns(task, ctx->ns);
	ppid = task_ppid_nr_ns(task, ctx->ns);
	uid = from_kuid_munged(&init_user_ns, task_uid(task));
	group_start = READ_ONCE(task->group_leader->start_boottime);
	rcu_read_unlock();
	if (!tid)
		return 0;
	get_task_comm(comm, task);
	/* Only argv[0] is exported, never the remaining arguments or environment. */
	process_read_argv0(task, argv0, sizeof(argv0));
	seq_printf(m, "%d\t%d\t%d\t%u\t%c\t%u\t%d\t%u\t%d\t%llu\t%llu\t%u\t",
		   tid, tgid, ppid, uid, task_state_to_char(task),
		   READ_ONCE(task->policy), task_nice(task),
		   READ_ONCE(task->rt_priority), task_cpu(task),
		   READ_ONCE(task->start_boottime), group_start,
		   !!(READ_ONCE(task->flags) & PF_KTHREAD));
	seq_escape(m, comm, "\t\r\n\\");
	seq_putc(m, '\t');
	seq_escape(m, argv0, "\t\r\n\\");
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations process_info_seq_ops = {
	.start = process_info_start,
	.next = process_info_next,
	.stop = process_info_stop,
	.show = process_info_show,
};

static int process_info_open(struct inode *inode, struct file *file)
{
	struct process_public_context *ctx;
	struct seq_file *m;
	int ret;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	ret = seq_open_private(file, &process_info_seq_ops, sizeof(*ctx));
	if (ret)
		return ret;
	m = file->private_data;
	ctx = m->private;
	ctx->ns = get_pid_ns(task_active_pid_ns(current));
	return 0;
}

static int process_info_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct process_public_context *ctx = m->private;

	put_pid_ns(ctx->ns);
	return seq_release_private(inode, file);
}

static int process_set_nice(struct task_struct *task, int nice)
{
	int policy, ret;
	struct sched_attr attr = {
		.size = sizeof(attr),
		/* Internal SETPARAM_POLICY: preserve policy and reset-on-fork. */
		.sched_policy = (u32)-1,
		.sched_nice = nice,
	};

	if (READ_ONCE(task->flags) & PF_KTHREAD)
		return -EOPNOTSUPP;
	if (READ_ONCE(task->exit_state))
		return -ESRCH;
	policy = READ_ONCE(task->policy);
	if (policy != SCHED_NORMAL && policy != SCHED_BATCH)
		return -EOPNOTSUPP;
	/* RT/DL parameters are intentionally absent; the scheduler rejects them. */
	ret = sched_setattr_nocheck(task, &attr);
	if (!ret && task_nice(task) != nice)
		return -EAGAIN;
	return ret;
}

static int process_apply_group(struct task_struct *leader, int nice,
			       unsigned int *updated)
{
	struct task_struct **tasks, *task;
	unsigned int count = 0, capacity, i;
	int ret = 0;

	capacity = min_t(unsigned int, READ_ONCE(leader->signal->nr_threads) + 8,
			 PROCESS_CTL_MAX_THREADS);
	tasks = kvmalloc_array(capacity, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;
	/* Never hold tasklist/RCU locks across a scheduler operation. */
	read_lock(&tasklist_lock);
	rcu_read_lock();
	for_each_thread(leader, task) {
		if (READ_ONCE(task->exit_state))
			continue;
		if (count == capacity) {
			ret = -E2BIG;
			break;
		}
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();
	read_unlock(&tasklist_lock);
	if (!count && !ret)
		ret = -ESRCH;
	if (!ret) {
		for (i = 0; i < count; i++) {
			ret = process_set_nice(tasks[i], nice);
			if (ret)
				break;
			(*updated)++;
		}
	}
	for (i = 0; i < count; i++)
		put_task_struct(tasks[i]);
	kvfree(tasks);
	return ret;
}

static int process_apply(struct process_public_context *ctx, bool group,
			 pid_t id, u64 start, int nice)
{
	struct task_struct *task = NULL;
	struct pid *pid;
	int ret;

	rcu_read_lock();
	pid = find_pid_ns(id, ctx->ns);
	if (pid)
		task = pid_task(pid, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;
	if (READ_ONCE(task->start_boottime) != start) {
		ret = -ESTALE;
		goto out;
	}
	if (group && !thread_group_leader(task)) {
		ret = -EINVAL;
		goto out;
	}
	if (group) {
		ret = process_apply_group(task, nice, &ctx->updated);
	} else {
		ret = process_set_nice(task, nice);
		if (!ret)
			ctx->updated = 1;
	}
out:
	put_task_struct(task);
	return ret;
}

static int process_ctl_show(struct seq_file *m, void *unused)
{
	struct process_public_context *ctx = m->private;

	mutex_lock(&ctx->lock);
	seq_printf(m, "updated=%u error=%d\n", ctx->updated, ctx->error);
	mutex_unlock(&ctx->lock);
	return 0;
}

static ssize_t process_ctl_write(struct file *file, const char __user *input,
				 size_t count, loff_t *position)
{
	struct seq_file *m = file->private_data;
	struct process_public_context *ctx = m->private;
	char *buffer, *cursor, *token, *words[4];
	unsigned int n = 0;
	u64 start;
	int id, nice, ret = -EINVAL;
	bool group;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > PROCESS_CTL_MAX_INPUT)
		return -E2BIG;
	buffer = memdup_user_nul(input, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (memchr(buffer, '\0', count))
		goto out;
	cursor = strim(buffer);
	if (strpbrk(cursor, "\r\n"))
		goto out;
	while ((token = strsep(&cursor, " \t"))) {
		if (!*token)
			continue;
		if (n == ARRAY_SIZE(words))
			goto out;
		words[n++] = token;
	}
	if (n != ARRAY_SIZE(words))
		goto out;
	group = !strcmp(words[0], "process");
	if (!group && strcmp(words[0], "thread"))
		goto out;
	if (kstrtoint(words[1], 10, &id) || id <= 0 ||
	    kstrtoull(words[2], 10, &start) || !start ||
	    kstrtoint(words[3], 10, &nice) || nice < MIN_NICE || nice > MAX_NICE)
		goto out;
	mutex_lock(&ctx->lock);
	ctx->updated = 0;
	ret = process_apply(ctx, group, id, start, nice);
	ctx->error = ret;
	mutex_unlock(&ctx->lock);
out:
	kfree(buffer);
	if (ret)
		return ret;
	return count;
}

static int process_ctl_open(struct inode *inode, struct file *file)
{
	struct process_public_context *ctx;
	int ret;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->ns = get_pid_ns(task_active_pid_ns(current));
	mutex_init(&ctx->lock);
	ret = single_open(file, process_ctl_show, ctx);
	if (ret) {
		put_pid_ns(ctx->ns);
		kfree(ctx);
	}
	return ret;
}

static int process_ctl_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct process_public_context *ctx = m->private;

	put_pid_ns(ctx->ns);
	kfree(ctx);
	return single_release(inode, file);
}

static const struct proc_ops process_info_ops = {
	.proc_open = process_info_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = process_info_release,
};

static const struct proc_ops process_ctl_ops = {
	.proc_open = process_ctl_open,
	.proc_read = seq_read,
	.proc_write = process_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = process_ctl_release,
};

static int __init process_public_init(void)
{
	struct proc_dir_entry *info;

	info = qweovo_manager_proc_create("process_info", 0444, &process_info_ops,
					  &process_info_marker);
	if (!info)
		return -ENOMEM;
	if (!qweovo_manager_proc_create("process_ctl", 0666, &process_ctl_ops,
					&process_ctl_marker)) {
		proc_remove(info);
		return -ENOMEM;
	}
	return 0;
}
device_initcall(process_public_init);
