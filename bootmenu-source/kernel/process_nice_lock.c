// SPDX-License-Identifier: GPL-2.0-only
/* Public UID-scoped nice rules. No task_struct or vendor ABI changes. */
#include <linux/qweovo_bootmenu.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/process_nice_lock.h>
#include <linux/proc_fs.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>

#define UID_LOCK_MAX_INPUT 128
#define ANDROID_UID_RANGE 100000U
#define ANDROID_APP_MIN 10000U
#define ANDROID_APP_MAX 19999U

struct uid_nice_rule {
	struct hlist_node node;
	struct rcu_head rcu;
	uid_t uid;
	int nice;
	u64 updated_ns;
};

static DEFINE_HASHTABLE(uid_rules, 8);
static DEFINE_MUTEX(uid_rules_mutex);
static DEFINE_STATIC_KEY_FALSE(uid_rules_active);
static unsigned int rule_count;
static bool enabled = true;
static char uid_lock_marker;

static int __init uid_nice_lock_setup(char *value)
{
	return value ? kstrtobool(value, &enabled) : -EINVAL;
}
early_param("qweovo_uid_nice_lock", uid_nice_lock_setup);

bool process_nice_lock_supported(void)
{
	return enabled && qweovo_bootmenu_custom_enabled();
}

unsigned int process_nice_lock_count(void)
{
	return READ_ONCE(rule_count);
}

bool process_nice_lock_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &uid_lock_marker;
}

/* Called with scheduler locks held: RCU lookup never allocates or sleeps. */
bool process_uid_nice_value(const struct task_struct *task, int *nice)
{
	struct uid_nice_rule *rule;
	uid_t uid;

	if (!qweovo_bootmenu_custom_enabled() ||
	    !static_branch_unlikely(&uid_rules_active) ||
	    (READ_ONCE(task->flags) & PF_KTHREAD))
		return false;

	rcu_read_lock();
	uid = from_kuid_munged(&init_user_ns, __task_cred(task)->uid);
	hash_for_each_possible_rcu(uid_rules, rule, node, uid) {
		if (rule->uid == uid) {
			*nice = READ_ONCE(rule->nice);
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();
	return false;
}

void process_uid_nice_apply(struct task_struct *task)
{
	int nice, policy = READ_ONCE(task->policy);

	if ((policy == SCHED_NORMAL || policy == SCHED_BATCH) &&
	    process_uid_nice_value(task, &nice))
		/* set_user_nice rechecks the current rule under the runqueue lock. */
		set_user_nice(task, task_nice(task));
}

/* Visit one referenced task at a time, never scheduling under tasklist/RCU. */
static void apply_uid_rule(uid_t uid)
{
	struct task_struct *task;
	struct pid *pid;
	int nr = 1;

	for (;;) {
		rcu_read_lock();
		pid = find_ge_pid(nr, &init_pid_ns);
		if (!pid) {
			rcu_read_unlock();
			break;
		}
		nr = pid_nr(pid) + 1;
		task = pid_task(pid, PIDTYPE_PID);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (task) {
			if (from_kuid_munged(&init_user_ns, task_uid(task)) == uid)
				process_uid_nice_apply(task);
			put_task_struct(task);
		}
		cond_resched();
	}
}

static bool application_uid(uid_t uid)
{
	uid_t appid = uid % ANDROID_UID_RANGE;

	return uid_valid(make_kuid(&init_user_ns, uid)) &&
		appid >= ANDROID_APP_MIN && appid <= ANDROID_APP_MAX;
}

static int uid_lock_show(struct seq_file *m, void *unused)
{
	struct uid_nice_rule *rule;
	unsigned int bucket;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	seq_puts(m, "uid\tnice\tupdated_ns\n");
	rcu_read_lock();
	hash_for_each_rcu(uid_rules, bucket, rule, node)
		seq_printf(m, "%u\t%d\t%llu\n", rule->uid,
			   READ_ONCE(rule->nice), READ_ONCE(rule->updated_ns));
	rcu_read_unlock();
	return 0;
}

static int uid_lock_open(struct inode *inode, struct file *file)
{
	return single_open(file, uid_lock_show, NULL);
}

static ssize_t uid_lock_write(struct file *file, const char __user *input,
			      size_t count, loff_t *position)
{
	struct uid_nice_rule *rule, *found = NULL;
	char *buffer, *cursor, *token, *words[3];
	unsigned int n = 0, uid;
	int nice = 0, ret = -EINVAL;
	bool lock;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!enabled)
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > UID_LOCK_MAX_INPUT)
		return -E2BIG;
	buffer = memdup_user_nul(input, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (memchr(buffer, '\0', count))
		goto free;
	cursor = strim(buffer);
	if (strpbrk(cursor, "\r\n"))
		goto free;
	while ((token = strsep(&cursor, " \t"))) {
		if (!*token)
			continue;
		if (n == ARRAY_SIZE(words))
			goto free;
		words[n++] = token;
	}
	if (n < 2 || kstrtouint(words[1], 10, &uid) || !application_uid(uid))
		goto free;
	lock = !strcmp(words[0], "lock");
	if (lock) {
		if (n != 3 || kstrtoint(words[2], 10, &nice) ||
		    nice < MIN_NICE || nice > MAX_NICE)
			goto free;
	} else if (strcmp(words[0], "unlock") || n != 2) {
		goto free;
	}

	mutex_lock(&uid_rules_mutex);
	hash_for_each_possible(uid_rules, rule, node, uid) {
		if (rule->uid == uid) {
			found = rule;
			break;
		}
	}
	if (lock) {
		if (!found) {
			if (rule_count >= PROCESS_NICE_LOCK_MAX_RULES) {
				ret = -ENOSPC;
				goto unlock;
			}
			found = kzalloc(sizeof(*found), GFP_KERNEL);
			if (!found) {
				ret = -ENOMEM;
				goto unlock;
			}
			found->uid = uid;
			found->nice = nice;
			found->updated_ns = ktime_get_boottime_ns();
			hash_add_rcu(uid_rules, &found->node, uid);
			if (!rule_count++)
				static_branch_enable(&uid_rules_active);
		} else {
			WRITE_ONCE(found->nice, nice);
			WRITE_ONCE(found->updated_ns, ktime_get_boottime_ns());
		}
		apply_uid_rule(uid);
	} else if (found) {
		hash_del_rcu(&found->node);
		if (!--rule_count)
			static_branch_disable(&uid_rules_active);
		kfree_rcu(found, rcu);
	}
	ret = 0;
unlock:
	mutex_unlock(&uid_rules_mutex);
free:
	kfree(buffer);
	return ret ? ret : count;
}

static const struct proc_ops uid_lock_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = uid_lock_open,
	.proc_read = seq_read,
	.proc_write = uid_lock_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init uid_nice_lock_init(void)
{
	return qweovo_manager_proc_create("priority_locks", 0666, &uid_lock_ops,
					 &uid_lock_marker) ? 0 : -ENOMEM;
}
late_initcall(uid_nice_lock_init);
