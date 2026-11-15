// SPDX-License-Identifier: GPL-2.0-only
/* One boot decision and an independent, one-use SukiSU menu grant. */
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/crc32.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/pid_namespace.h>
#include <linux/proc_fs.h>
#include <linux/qweovo_bootmenu.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <uapi/linux/qweovo_bootmenu.h>
#include "qweovo_sukisu_grant.h"

#define BOOT_COMMITTED 0x20U
#define SUKISU_GRANT_FILE "qweovo-sukisu-next-signed-v1"
#define BOOT_FEATURES_FILE "qweovo-boot-features-v1"
#define BOOT_NEXT_FILE "qweovo-boot-next-v1"
#define SUKISU_STORE_TIMEOUT_MS 2000

static atomic_t boot_state = ATOMIC_INIT(0);
static atomic_t decision_issued = ATOMIC_INIT(0);
static atomic_t sukisu_prepare_requested = ATOMIC_INIT(0);
static char status_marker;
static char manager_api_marker;
static char security_status_marker;
static char selinux_lock_marker;
static char sukisu_next_marker;
static char boot_settings_marker;
static char boot_tee_marker;
static DEFINE_MUTEX(sukisu_store_lock);
static DEFINE_MUTEX(boot_meta_lock);
static DECLARE_COMPLETION(sukisu_store_prepared);
static struct file *sukisu_store;
static struct file *boot_meta_dir;
static struct file *boot_features_file;
static struct file *boot_next_file;
static struct sukisu_grant sukisu_current_grant;
static struct path sukisu_root;
static const struct cred *sukisu_early_cred;
static bool sukisu_consume_ok;
static bool sukisu_allowed_this_boot;
static bool sukisu_consume_requested;
static bool sukisu_node_created;
static struct qweovo_boot_features boot_features = {
	.magic = QWEOVO_FEATURES_MAGIC,
	.version = QWEOVO_FEATURES_VERSION,
	.size = sizeof(struct qweovo_boot_features),
	.features = QWEOVO_BOOT_STATUS_SPOOFER | QWEOVO_BOOT_SECURITY,
	.timeout_sec = QWEOVO_MENU_TIMEOUT_DEFAULT,
};
static struct qweovo_boot_request boot_request;
static bool boot_request_ready;
static unsigned int boot_last_target;
static unsigned int boot_last_menu;
static char boot_diag_marker;
static DEFINE_MUTEX(boot_diag_lock);
static struct qweovo_boot_diag_line boot_diag_lines[64];
static unsigned int boot_diag_next;
static unsigned int boot_diag_count;
static void sukisu_expose_next_node(void);

static bool sukisu_grant_file_usable(struct file *store, struct file *dir)
{
	struct inode *inode = file_inode(store);

	return store->f_path.mnt == dir->f_path.mnt &&
	       S_ISREG(inode->i_mode) && inode->i_nlink == 1 &&
	       uid_eq(inode->i_uid, GLOBAL_ROOT_UID) &&
	       (inode->i_mode & 0777) == 0600 &&
	       i_size_read(inode) <= sizeof(struct sukisu_grant);
}

/*
 * A malformed entry must never lock the menu out: whatever owns the grant
 * name (stale signature, wrong size/owner/mode/link count, symlink, stray
 * directory) is removed so boot can persist a fresh challenge instead of
 * leaving /proc/qweovo_sukisu_next permanently unusable.
 */
static int sukisu_drop_grant(struct file *dir)
{
	struct file *victim;
	struct dentry *dentry;
	struct inode *parent;
	int ret;

	victim = file_open_root(sukisu_root.dentry, sukisu_root.mnt,
				"metadata/" SUKISU_GRANT_FILE,
				O_PATH | O_NOFOLLOW, 0);
	if (IS_ERR(victim))
		return PTR_ERR(victim) == -ENOENT ? 0 : PTR_ERR(victim);

	dentry = file_dentry(victim);
	parent = d_inode(dentry->d_parent);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}
	ret = mnt_want_write_file(dir);
	if (ret)
		goto out;
	ret = d_is_dir(dentry) ? vfs_rmdir(parent, dentry)
			       : vfs_unlink(parent, dentry, NULL);
	mnt_drop_write_file(dir);
out:
	fput(victim);
	return ret;
}

static struct file *sukisu_open_grant(struct file *dir, int *error)
{
	struct file *store;
	int ret = 0;

	store = file_open_root(dir->f_path.dentry, dir->f_path.mnt,
			       SUKISU_GRANT_FILE,
			       O_RDWR | O_NOFOLLOW | O_NONBLOCK, 0);
	if (!IS_ERR(store)) {
		if (sukisu_grant_file_usable(store, dir)) {
			*error = 0;
			return store;
		}
		fput(store);
	} else if (PTR_ERR(store) != -ENOENT) {
		pr_info("qweovo_bootmenu: unusable SukiSU grant entry: %ld\n",
			PTR_ERR(store));
	}

	ret = sukisu_drop_grant(dir);
	if (ret && ret != -ENOENT) {
		*error = ret;
		return NULL;
	}
	pr_info("qweovo_bootmenu: reinitializing SukiSU grant file\n");
	store = file_open_root(dir->f_path.dentry, dir->f_path.mnt,
			       SUKISU_GRANT_FILE,
			       O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK,
			       0600);
	if (IS_ERR(store)) {
		*error = PTR_ERR(store);
		return NULL;
	}
	if (!sukisu_grant_file_usable(store, dir)) {
		fput(store);
		*error = -EINVAL;
		return NULL;
	}
	*error = 0;
	return store;
}

static void sukisu_prepare_store(struct work_struct *work)
{
	const struct cred *old = override_creds(sukisu_early_cred);
	struct file *dir, *store = NULL;
	int ret;

	ret = -ENOKEY;
	if (!sizeof(sukisu_public_key))
		goto out;
	dir = file_open_root(sukisu_root.dentry, sukisu_root.mnt, "metadata",
			     O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
	if (IS_ERR(dir)) {
		ret = PTR_ERR(dir);
		goto out;
	}
	ret = -ENODEV;
	if (dir->f_path.dentry != dir->f_path.mnt->mnt_root ||
	    file_inode(dir)->i_sb->s_magic != EXT4_SUPER_MAGIC)
		goto close_dir;
	store = sukisu_open_grant(dir, &ret);
	if (!store)
		goto close_dir;
	/* Persist a newly created name before accepting any grant. */
	ret = vfs_fsync(dir, 0);
	if (ret)
		goto close_dir;
	if (READ_ONCE(sukisu_consume_requested))
		ret = sukisu_rotate_grant(store, &sukisu_current_grant,
					  &sukisu_consume_ok);
	else
		ret = sukisu_ensure_grant(store, &sukisu_current_grant,
					  &sukisu_consume_ok);
	if (ret)
		goto close_dir;
	mutex_lock(&sukisu_store_lock);
	WRITE_ONCE(sukisu_store, store);
	mutex_unlock(&sukisu_store_lock);
	store = NULL;
	ret = 0;
close_dir:
	if (store)
		fput(store);
	fput(dir);
out:
	if (ret)
		pr_warn("qweovo_bootmenu: SukiSU grant storage unavailable: %d\n", ret);
	revert_creds(old);
	put_cred(sukisu_early_cred);
	path_put(&sukisu_root);
	complete(&sukisu_store_prepared);
}
static DECLARE_WORK(sukisu_store_work, sukisu_prepare_store);

bool qweovo_bootmenu_module_lock_requested(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_LOCK_MODULES;
}

bool qweovo_bootmenu_selinux_lock_requested(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_LOCK_SELINUX;
}

bool qweovo_bootmenu_audit_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_SECURITY_AUDIT;
}

bool qweovo_bootmenu_committed(void)
{
	return atomic_read(&boot_state) & BOOT_COMMITTED;
}

bool qweovo_bootmenu_sukisu_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_SUKISU;
}

bool qweovo_bootmenu_meow_su_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_MEOW_SU;
}

bool qweovo_bootmenu_custom_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_CUSTOM;
}

bool qweovo_bootmenu_status_spoofer_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_STATUS_SPOOFER;
}

bool qweovo_bootmenu_tee_enabled(void)
{
	return atomic_read(&boot_state) & QWEOVO_BOOT_TEE;
}

static bool is_boot_init(void)
{
	return task_pid_nr(current) == 1 &&
		task_active_pid_ns(current) == &init_pid_ns &&
		uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

bool qweovo_bootmenu_selinux_lock_writer(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &selinux_lock_marker &&
		qweovo_bootmenu_selinux_lock_requested() && is_boot_init();
}

#ifdef CONFIG_SECURITY_SELINUX
static ssize_t selinux_lock_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *pos)
{
	char command;
	int ret;

	if (!qweovo_bootmenu_selinux_lock_writer(file_inode(file)))
		return -EPERM;
	if (*pos || count != 1)
		return -EINVAL;
	if (get_user(command, buffer))
		return -EFAULT;
	if (command != '1')
		return -EINVAL;
	ret = qweovo_selinux_activate_boot_lock();
	qweovo_security_audit("selinux-lock-activate", ret);
	return ret ? ret : count;
}

static const struct proc_ops selinux_lock_ops = {
	.proc_write = selinux_lock_write,
};
#endif

/* ---- persistent boot settings ---------------------------------------- */

static u32 boot_crc(const void *data, size_t length)
{
	return crc32_le(~0U, data, length) ^ ~0U;
}

static bool boot_features_valid(const struct qweovo_boot_features *features)
{
	return features->magic == QWEOVO_FEATURES_MAGIC &&
	       features->version == QWEOVO_FEATURES_VERSION &&
	       features->size == sizeof(*features) && !features->reserved &&
	       !(features->features & ~QWEOVO_FEATURES_PERSIST) &&
	       features->timeout_sec >= QWEOVO_MENU_TIMEOUT_MIN &&
	       features->timeout_sec <= QWEOVO_MENU_TIMEOUT_MAX &&
	       features->crc32 ==
		       boot_crc(features,
				offsetof(struct qweovo_boot_features, crc32));
}

static bool boot_next_valid(const struct qweovo_boot_next *next)
{
	return next->magic == QWEOVO_NEXT_MAGIC &&
	       next->version == QWEOVO_NEXT_VERSION &&
	       next->size == sizeof(*next) && !next->reserved &&
	       next->target <= QWEOVO_NEXT_BOOTLOADER &&
	       next->crc32 == boot_crc(next, offsetof(struct qweovo_boot_next, crc32));
}

/* These descriptors are opened by PID 1 before policy load, exactly like the
 * audit store, so later writes do not depend on the caller's SELinux domain.
 */
static int boot_file_read(struct file *file, void *buffer, size_t length)
{
	loff_t position = 0;
	ssize_t read;

	if (!file)
		return -ENODEV;
	read = __kernel_read(file, buffer, length, &position);
	return read == length ? 0 : -EIO;
}

static int boot_file_write(struct file *file, const void *buffer, size_t length)
{
	loff_t position = 0;
	ssize_t written;

	if (!file)
		return -ENODEV;
	file_start_write(file);
	written = __kernel_write(file, buffer, length, &position);
	file_end_write(file);
	if (written != length)
		return -EIO;
	return vfs_fsync(file, 0);
}

static int boot_meta_unlink(const char *name)
{
	struct file *victim;
	struct dentry *dentry;
	struct inode *parent;
	int ret;

	if (!READ_ONCE(boot_meta_dir))
		return -ENODEV;
	victim = file_open_root(boot_meta_dir->f_path.dentry,
				boot_meta_dir->f_path.mnt, name,
				O_PATH | O_NOFOLLOW, 0);
	if (IS_ERR(victim))
		return PTR_ERR(victim) == -ENOENT ? 0 : PTR_ERR(victim);
	dentry = file_dentry(victim);
	parent = d_inode(dentry->d_parent);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}
	ret = mnt_want_write_file(boot_meta_dir);
	if (ret)
		goto out;
	ret = d_is_dir(dentry) ? vfs_rmdir(parent, dentry)
			       : vfs_unlink(parent, dentry, NULL);
	mnt_drop_write_file(boot_meta_dir);
out:
	fput(victim);
	return ret;
}

static struct file *boot_open_file(const char *name)
{
	struct file *file;
	struct inode *inode;
	int attempt;

	if (!READ_ONCE(boot_meta_dir))
		return NULL;
	for (attempt = 0; attempt < 2; attempt++) {
		file = file_open_root(boot_meta_dir->f_path.dentry,
				      boot_meta_dir->f_path.mnt, name,
				      O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK,
				      0600);
		if (!IS_ERR(file)) {
			inode = file_inode(file);
			if (S_ISREG(inode->i_mode) && inode->i_nlink == 1 &&
			    uid_eq(inode->i_uid, GLOBAL_ROOT_UID) &&
			    (inode->i_mode & 0777) == 0600)
				return file;
			fput(file);
		}
		if (boot_meta_unlink(name) && attempt)
			break;
	}
	return NULL;
}

static int boot_store_features(struct qweovo_boot_features *features)
{
	features->magic = QWEOVO_FEATURES_MAGIC;
	features->version = QWEOVO_FEATURES_VERSION;
	features->size = sizeof(*features);
	features->reserved = 0;
	features->crc32 = boot_crc(features,
				   offsetof(struct qweovo_boot_features, crc32));
	return boot_file_write(boot_features_file, features, sizeof(*features));
}

static void boot_meta_init(void)
{
	struct file *dir;

	if (READ_ONCE(boot_meta_dir))
		return;
	dir = file_open_root(sukisu_root.dentry, sukisu_root.mnt, "metadata",
			     O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
	if (IS_ERR(dir)) {
		pr_warn("qweovo_bootmenu: metadata unavailable: %ld\n",
			PTR_ERR(dir));
		return;
	}
	if (dir->f_path.dentry != dir->f_path.mnt->mnt_root ||
	    file_inode(dir)->i_sb->s_magic != EXT4_SUPER_MAGIC) {
		pr_warn("qweovo_bootmenu: unexpected metadata mount\n");
		fput(dir);
		return;
	}
	WRITE_ONCE(boot_meta_dir, dir);
	boot_features_file = boot_open_file(BOOT_FEATURES_FILE);
	boot_next_file = boot_open_file(BOOT_NEXT_FILE);
	if (!boot_features_file || !boot_next_file)
		pr_warn("qweovo_bootmenu: boot settings storage unavailable\n");
}

static void boot_load_features(void)
{
	struct qweovo_boot_features stored;
	int ret;

	ret = boot_file_read(boot_features_file, &stored, sizeof(stored));
	if (!ret && boot_features_valid(&stored)) {
		boot_features = stored;
		return;
	}
	pr_info("qweovo_bootmenu: resetting boot settings (%d)\n", ret);
	memset(&stored, 0, sizeof(stored));
	stored.features = QWEOVO_BOOT_STATUS_SPOOFER | QWEOVO_BOOT_SECURITY;
	stored.timeout_sec = QWEOVO_MENU_TIMEOUT_DEFAULT;
	if (!boot_store_features(&stored))
		boot_features = stored;
	else
		pr_warn("qweovo_bootmenu: cannot persist default boot settings\n");
}

static unsigned int boot_consume_next(void)
{
	struct qweovo_boot_next next;
	struct qweovo_boot_next clear = {};
	unsigned int target = QWEOVO_NEXT_NORMAL;
	int ret;

	ret = boot_file_read(boot_next_file, &next, sizeof(next));
	if (!ret && boot_next_valid(&next))
		target = next.target;
	/* Single use: blank the entry through the descriptor opened by PID 1. */
	boot_file_write(boot_next_file, &clear, sizeof(clear));
	return target;
}

/* ---- boot request / decision ----------------------------------------- */

static ssize_t choice_read(struct file *file, char __user *buffer,
			   size_t count, loff_t *position)
{
	struct qweovo_boot_request request = {
		.magic = QWEOVO_BOOT_REQUEST_MAGIC,
		.version = QWEOVO_BOOT_REQUEST_VERSION,
		.size = sizeof(request),
		.features = QWEOVO_BOOT_CUSTOM,
		.timeout_sec = QWEOVO_MENU_TIMEOUT_DEFAULT,
	};
	unsigned int target = QWEOVO_NEXT_NORMAL;
	bool menu;

	(void)position;
	if (!is_boot_init() || qweovo_bootmenu_committed())
		return -EPERM;
	if (!count)
		return 0;
	if (count < sizeof(request))
		return -EINVAL;
	/* The request is cached, so a retried or interrupted read can never be
	 * mistaken for an empty/short answer by the preinit runner.
	 */
	if (READ_ONCE(boot_request_ready))
		goto copy;
	if (atomic_cmpxchg(&sukisu_prepare_requested, 0, 1))
		return 0;
	get_fs_root(current->fs, &sukisu_root);
	boot_meta_init();
	if (boot_meta_dir) {
		boot_load_features();
		target = boot_consume_next();
	}
	/* Normal boots skip the menu; without metadata the menu stays the
	 * only way to configure, so keep showing it.
	 */
	menu = !boot_meta_dir || target == QWEOVO_NEXT_MENU;
	if (target == QWEOVO_NEXT_BOOTLOADER)
		request.flags |= QWEOVO_BOOT_REQUEST_BOOTLOADER;
	if (menu)
		request.flags |= QWEOVO_BOOT_REQUEST_SHOW_MENU;
	/* Always prepare the one-time grant: the Manager needs a challenge on
	 * every boot, and an armed signature is consumed by a menu boot.
	 */
	sukisu_early_cred = get_current_cred();
	WRITE_ONCE(sukisu_consume_requested, menu);
	schedule_work(&sukisu_store_work);
	if (wait_for_completion_timeout(&sukisu_store_prepared,
			msecs_to_jiffies(SUKISU_STORE_TIMEOUT_MS)) &&
	    sukisu_consume_ok) {
		WRITE_ONCE(sukisu_allowed_this_boot, true);
		request.flags |= QWEOVO_BOOT_REQUEST_GRANT;
	}
	request.features = boot_features.features | QWEOVO_BOOT_CUSTOM;
	request.timeout_sec = boot_features.timeout_sec;
	WRITE_ONCE(boot_last_target, target);
	WRITE_ONCE(boot_last_menu, menu);
	boot_request = request;
	WRITE_ONCE(boot_request_ready, true);
	pr_info("qweovo_bootmenu: request target=%u menu=%u features=%u timeout=%u\n",
		target, menu, request.features, request.timeout_sec);
copy:
	return copy_to_user(buffer, &boot_request, sizeof(boot_request)) ?
		-EFAULT : sizeof(boot_request);
}

void qweovo_bootmenu_seal_disabled(void)
{
	/* A missing, failed or bypassed menu leaves all features off. */
	if (is_boot_init() && !atomic_cmpxchg(&boot_state, 0, BOOT_COMMITTED))
		pr_info("qweovo_bootmenu: sealed fallback (all features off)\n");
}

static ssize_t choice_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *position)
{
	struct qweovo_boot_choice choice;
	unsigned int state;

	if (!is_boot_init())
		return -EPERM;
	if (count != sizeof(choice))
		return -EINVAL;
	if (copy_from_user(&choice, buffer, sizeof(choice)))
		return -EFAULT;
	if (choice.magic != QWEOVO_BOOT_CHOICE_MAGIC ||
	    choice.version != QWEOVO_BOOT_CHOICE_VERSION ||
	    choice.size != sizeof(choice) || choice.reserved ||
	    (choice.features & ~QWEOVO_BOOT_FEATURES))
		return -EINVAL;
	if ((choice.features & QWEOVO_BOOT_ROOT_MODES) == QWEOVO_BOOT_ROOT_MODES)
		return -EINVAL;
	if ((choice.features & QWEOVO_BOOT_ROOT_MODES) &&
	    (choice.features & QWEOVO_BOOT_SECURITY))
		return -EINVAL;
	if ((choice.features & QWEOVO_BOOT_ROOT_MODES) && !READ_ONCE(sukisu_allowed_this_boot))
		return -EPERM;
	/* Root sessions are temporary by design: never write their feature
	 * changes back. Root-off sessions persist everything except SU/Custom.
	 */
	if (!(choice.features & QWEOVO_BOOT_ROOT_MODES)) {
		struct qweovo_boot_features persisted = boot_features;

		persisted.features = choice.features & QWEOVO_FEATURES_PERSIST;
		persisted.timeout_sec = boot_features.timeout_sec;
		if (!boot_store_features(&persisted))
			boot_features = persisted;
		else
			pr_warn("qweovo_bootmenu: cannot persist boot settings\n");
	}
	state = BOOT_COMMITTED | choice.features;
	if (atomic_cmpxchg(&boot_state, 0, state))
		return -EPERM;
	qweovo_security_audit_prepare();
	qweovo_security_audit("audit-enabled", 0);
	if (state & QWEOVO_BOOT_CUSTOM)
		sukisu_expose_next_node();
	if ((state & QWEOVO_BOOT_MEOW_SU) && qweovo_meow_su_create_nodes())
		pr_warn("qweovo_bootmenu: MeowSU grant store unavailable\n");
	pr_info("qweovo_bootmenu: committed sukisu=%u custom=%u status_spoofer=%u tee=%u\n",
		!!(state & QWEOVO_BOOT_SUKISU), !!(state & QWEOVO_BOOT_CUSTOM),
		!!(state & QWEOVO_BOOT_STATUS_SPOOFER), !!(state & QWEOVO_BOOT_TEE));
	return count;
}

static int choice_release(struct inode *inode, struct file *file)
{
	/* Releasing the final inherited reference also closes a failed menu. */
	atomic_cmpxchg(&boot_state, 0, BOOT_COMMITTED);
	return 0;
}

static long choice_diag_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qweovo_boot_diag_line line;
	bool child;
	int i;

	if (cmd != QWEOVO_BOOT_DIAG_APPEND)
		return -ENOTTY;
	rcu_read_lock();
	child = task_pid_nr(rcu_dereference(current->real_parent)) == 1;
	rcu_read_unlock();
	if (qweovo_bootmenu_committed() ||
	    (!is_boot_init() && !(child && task_active_pid_ns(current) == &init_pid_ns &&
				   uid_eq(current_euid(), GLOBAL_ROOT_UID))))
		return -EPERM;
	if (copy_from_user(&line, (void __user *)arg, sizeof(line)))
		return -EFAULT;
	line.text[sizeof(line.text) - 1] = '\0';
	for (i = 0; line.text[i]; i++)
		if (line.text[i] < ' ' || line.text[i] > '~')
			line.text[i] = '?';
	if (!i)
		return -EINVAL;

	mutex_lock(&boot_diag_lock);
	boot_diag_lines[boot_diag_next] = line;
	boot_diag_next = (boot_diag_next + 1) % ARRAY_SIZE(boot_diag_lines);
	if (boot_diag_count < ARRAY_SIZE(boot_diag_lines))
		boot_diag_count++;
	mutex_unlock(&boot_diag_lock);
	return 0;
}

static const struct file_operations choice_ops = {
	.read = choice_read,
	.write = choice_write,
	.unlocked_ioctl = choice_diag_ioctl,
	.release = choice_release,
	.llseek = no_llseek,
};

int qweovo_bootmenu_install_fd(void)
{
	int fd;

	if (!is_boot_init() || qweovo_bootmenu_committed())
		return -EPERM;
	if (atomic_cmpxchg(&decision_issued, 0, 1))
		return -EPERM;
	/* Intentionally survive exec into the static preinit runner. */
	fd = anon_inode_getfd("[qweovo_boot_choice]", &choice_ops, NULL, O_RDWR);
	if (fd < 0)
		qweovo_bootmenu_seal_disabled();
	return fd;
}

bool qweovo_bootmenu_status_inode(const struct inode *inode)
{
	void *marker = proc_inode_data_or_null(inode);

	return marker == &status_marker ||
		(qweovo_bootmenu_custom_enabled() && marker == &sukisu_next_marker);
}

bool qweovo_sukisu_next_supported(void)
{
	return qweovo_bootmenu_custom_enabled() &&
		READ_ONCE(sukisu_node_created) && READ_ONCE(sukisu_store);
}

bool qweovo_bootmenu_api_inode(const struct inode *inode)
{
	void *marker = proc_inode_data_or_null(inode);

	return marker == &manager_api_marker || marker == &security_status_marker ||
		marker == &boot_diag_marker;
}

static int sukisu_next_show(struct seq_file *m, void *unused)
{
	const struct sukisu_grant *grant = &sukisu_current_grant;
	int ret = -EOPNOTSUPP;

	mutex_lock(&sukisu_store_lock);
	if (sukisu_store) {
		seq_printf(m, "challenge=MEOW-SU1:%*phN%*phN\narmed=%u\n",
			   (int)sizeof(grant->key_id), grant->key_id,
			   (int)sizeof(grant->nonce), grant->nonce,
			   !!memchr_inv(grant->signature, 0, sizeof(grant->signature)));
		ret = 0;
	}
	mutex_unlock(&sukisu_store_lock);
	return ret;
}

static int sukisu_next_open(struct inode *inode, struct file *file)
{
	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	return single_open(file, sukisu_next_show, NULL);
}

static ssize_t sukisu_next_write(struct file *file, const char __user *buffer,
				 size_t count, loff_t *position)
{
	struct sukisu_grant grant;
	const size_t prefix = sizeof(SUKISU_RESPONSE_PREFIX) - 1;
	char value[sizeof(SUKISU_RESPONSE_PREFIX) - 1 + SUKISU_SIGNATURE_SIZE * 2 + 1];
	int ret = -EOPNOTSUPP;

	if (count != sizeof(value) && count != sizeof(value) - 1)
		return -EINVAL;
	if (copy_from_user(value, buffer, count))
		return -EFAULT;
	if (memcmp(value, SUKISU_RESPONSE_PREFIX, prefix) ||
	    (count == sizeof(value) && value[count - 1] != '\n'))
		return -EINVAL;
	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;

	mutex_lock(&sukisu_store_lock);
	if (!sukisu_store)
		goto unlock;
	grant = sukisu_current_grant;
	ret = hex2bin(grant.signature, value + prefix, sizeof(grant.signature));
	if (!ret)
		ret = sukisu_verify_grant(&grant);
	if (!ret)
		ret = sukisu_write_grant(sukisu_store, &grant);
	if (!ret)
		sukisu_current_grant = grant;
unlock:
	mutex_unlock(&sukisu_store_lock);
	if (ret)
		return ret;
	return count;
}

static const struct proc_ops sukisu_next_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = sukisu_next_open,
	.proc_read = seq_read,
	.proc_write = sukisu_next_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void sukisu_expose_next_node(void)
{
	bool created;

	created = !!qweovo_manager_proc_create("qweovo_sukisu_next", 0666,
					      &sukisu_next_ops, &sukisu_next_marker);
	WRITE_ONCE(sukisu_node_created, created);
	if (!created)
		pr_warn("qweovo_bootmenu: cannot expose SukiSU next-boot switch\n");
}

static int status_show(struct seq_file *m, void *unused)
{
	unsigned int state = atomic_read(&boot_state);

	seq_printf(m, "schema_version=1\ncommitted=%u\nsukisu=%u\ncustom=%u\nstatus_spoofer=%u\ntee=%u\n",
		!!(state & BOOT_COMMITTED), !!(state & QWEOVO_BOOT_SUKISU),
		!!(state & QWEOVO_BOOT_CUSTOM), !!(state & QWEOVO_BOOT_STATUS_SPOOFER),
		!!(state & QWEOVO_BOOT_TEE));
	seq_puts(m, "persistence=none\nmutable=false\n");
	seq_printf(m, "meow_su=%u\n", !!(state & QWEOVO_BOOT_MEOW_SU));
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, NULL);
}

static const struct proc_ops status_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = status_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int manager_api_show(struct seq_file *m, void *unused)
{
	seq_puts(m, "1\n");
	return 0;
}

static int security_status_show(struct seq_file *m, void *unused)
{
	bool locked = !IS_ENABLED(CONFIG_MODULES);

#ifdef CONFIG_MODULES
	locked = READ_ONCE(modules_disabled);
#endif
	seq_printf(m, "module_lock_requested=%u\nmodules_locked=%u\n",
		   qweovo_bootmenu_module_lock_requested(), locked);
	seq_printf(m, "selinux_lock_requested=%u\nselinux_lock=%u\naudit_enabled=%u\naudit_storage=%u\n",
		   qweovo_bootmenu_selinux_lock_requested(), qweovo_bootmenu_selinux_locked(),
		   qweovo_bootmenu_audit_enabled(),
		   qweovo_security_audit_available());
	seq_printf(m, "pipe_merge_blocked=%llu\n",
		   (unsigned long long)qweovo_security_pipe_merge_count());
	return 0;
}

static int boot_diag_show(struct seq_file *m, void *unused)
{
	unsigned int i, index;

	seq_printf(m, "target=%u\nmenu=%u\n", READ_ONCE(boot_last_target),
		   READ_ONCE(boot_last_menu));
	mutex_lock(&boot_diag_lock);
	index = (boot_diag_next + ARRAY_SIZE(boot_diag_lines) - boot_diag_count) %
		ARRAY_SIZE(boot_diag_lines);
	for (i = 0; i < boot_diag_count; i++) {
		seq_printf(m, "%s\n", boot_diag_lines[index].text);
		index = (index + 1) % ARRAY_SIZE(boot_diag_lines);
	}
	mutex_unlock(&boot_diag_lock);
	return 0;
}

static int boot_settings_show(struct seq_file *m, void *unused)
{
	struct qweovo_boot_next next;
	unsigned int target = QWEOVO_NEXT_NORMAL;
	int ret;

	mutex_lock(&boot_meta_lock);
	ret = boot_file_read(boot_next_file, &next, sizeof(next));
	if (!ret && boot_next_valid(&next))
		target = next.target;
	seq_printf(m, "schema_version=1\ntarget=%u\ntimeout=%u\n",
		   target, READ_ONCE(boot_features.timeout_sec));
	mutex_unlock(&boot_meta_lock);
	return 0;
}

static int boot_settings_open(struct inode *inode, struct file *file)
{
	return single_open(file, boot_settings_show, NULL);
}

static ssize_t boot_settings_write(struct file *file, const char __user *buffer,
				   size_t count, loff_t *position)
{
	struct qweovo_boot_next next = {
		.magic = QWEOVO_NEXT_MAGIC,
		.version = QWEOVO_NEXT_VERSION,
		.size = sizeof(next),
	};
	char command[64];
	unsigned int value;
	int ret;

	if (*position || !count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buffer, count))
		return -EFAULT;
	command[count] = 0;
	if (command[count - 1] == '\n')
		command[count - 1] = 0;
	mutex_lock(&boot_meta_lock);
	if (!strncmp(command, "target=", 7)) {
		const char *name = command + 7;

		if (!strcmp(name, "normal"))
			next.target = QWEOVO_NEXT_NORMAL;
		else if (!strcmp(name, "menu"))
			next.target = QWEOVO_NEXT_MENU;
		else if (!strcmp(name, "bootloader"))
			next.target = QWEOVO_NEXT_BOOTLOADER;
		else {
			ret = -EINVAL;
			goto out;
		}
		next.crc32 = boot_crc(&next,
				      offsetof(struct qweovo_boot_next, crc32));
		ret = boot_file_write(boot_next_file, &next, sizeof(next));
	} else if (!strncmp(command, "timeout=", 8)) {
		struct qweovo_boot_features persisted = boot_features;

		if (kstrtouint(command + 8, 10, &value) ||
		    value < QWEOVO_MENU_TIMEOUT_MIN ||
		    value > QWEOVO_MENU_TIMEOUT_MAX) {
			ret = -EINVAL;
			goto out;
		}
		persisted.timeout_sec = value;
		ret = boot_store_features(&persisted);
		if (!ret)
			boot_features = persisted;
	} else {
		ret = -EINVAL;
	}
out:
	mutex_unlock(&boot_meta_lock);
	if (ret)
		return ret;
	*position = count;
	return count;
}

static const struct proc_ops boot_settings_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = boot_settings_open,
	.proc_read = seq_read,
	.proc_write = boot_settings_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int boot_tee_show(struct seq_file *m, void *unused)
{
	const char *keybox = "unknown";
	bool enabled = qweovo_bootmenu_tee_enabled();
	struct cred *root = prepare_kernel_cred(NULL);

	if (root) {
		const struct cred *old = override_creds(root);
		struct path path;
		int ret = kern_path("/data/keymaster/keybox.xml", 0, &path);

		if (!ret) {
			struct inode *inode = d_backing_inode(path.dentry);

			if (d_is_symlink(path.dentry))
				keybox = "symlink";
			else if (!inode || !S_ISREG(inode->i_mode))
				keybox = "not_regular";
			else if (!uid_eq(inode->i_uid, GLOBAL_ROOT_UID) ||
				 !gid_eq(inode->i_gid, GLOBAL_ROOT_GID))
				keybox = "owner";
			else if ((inode->i_mode & 0777) != 0600)
				keybox = "mode";
			else
				keybox = "ok";
			path_put(&path);
		} else {
			keybox = ret == -ENOENT ? "missing" : "error";
		}
		revert_creds(old);
		put_cred(root);
	}
	seq_printf(m, "schema_version=1\nenabled=%u\nkeybox=%s\n", enabled,
		   keybox);
	return 0;
}

static int boot_tee_open(struct inode *inode, struct file *file)
{
	return single_open(file, boot_tee_show, NULL);
}

static const struct proc_ops boot_tee_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = boot_tee_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

bool qweovo_bootmenu_manager_inode(const struct inode *inode)
{
	void *marker = proc_inode_data_or_null(inode);

	return marker == &boot_settings_marker || marker == &boot_tee_marker;
}

static int __init bootmenu_status_init(void)
{
#ifdef CONFIG_SECURITY_SELINUX
	if (!proc_create_data("qweovo_selinux_lock", 0200, NULL, &selinux_lock_ops,
			      &selinux_lock_marker))
		pr_warn("qweovo_bootmenu: SELinux lock trigger unavailable\n");
#endif
	if (!proc_create_single_data("qweovo_security_status", 0444, NULL,
				     security_status_show, &security_status_marker))
		pr_warn("qweovo_bootmenu: security status unavailable\n");
	if (!proc_create_single_data("qweovo_manager_api", 0444, NULL,
				     manager_api_show, &manager_api_marker))
		return -ENOMEM;
	if (!qweovo_manager_proc_create("qweovo_boot_settings", 0600,
					&boot_settings_ops, &boot_settings_marker))
		pr_warn("qweovo_bootmenu: boot settings interface unavailable\n");
	if (!qweovo_manager_proc_create("qweovo_tee_status", 0600,
					&boot_tee_ops, &boot_tee_marker))
		pr_warn("qweovo_bootmenu: TEE status interface unavailable\n");
	if (!proc_create_single_data("qweovo_boot_diag", 0444, NULL,
				     boot_diag_show, &boot_diag_marker))
		pr_warn("qweovo_bootmenu: boot diagnostics unavailable\n");
	return qweovo_manager_proc_create("qweovo_boot_status", 0444, &status_ops,
					 &status_marker) ? 0 : -ENOMEM;
}
device_initcall(bootmenu_status_init);
