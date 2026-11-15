// SPDX-License-Identifier: GPL-2.0-only
/* TB320FC/cape diagnostic hook; no block-device or filesystem writes. */
#define pr_fmt(fmt) "QWEOVO ramdump: " fmt

#include <linux/qweovo_bootmenu.h>
#include <linux/process_nice_lock.h>
#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/qweovo_ramdump.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <generated/utsrelease.h>

/* Same ARMv8 SCM wire ABI used by the Qualcomm vendor SCM driver. */
#define SCM_IO_WRITE		0x0502
#define SCM_INFO_IS_CALL_AVAIL	0x0601
#define SCM_FULL_DUMP		0x10
#define SCM_INTERRUPTED		1
#define SCM_RESUME_LIMIT		16
#define PANIC_CTL_MAX_INPUT	128

enum qweovo_panic_action {
	QWEOVO_PANIC_REBOOT,
	QWEOVO_PANIC_RAMDUMP,
};

static int panic_action = QWEOVO_PANIC_REBOOT;
static DEFINE_MUTEX(panic_ctl_lock);
static char panic_ctl_marker;
static char kernel_info_marker;
static bool vendor_prepared;
static bool vendor_live;
static int last_dump_request = -1;

static bool enabled = true;
module_param(enabled, bool, 0444);
MODULE_PARM_DESC(enabled, "Enable the cape panic-to-USB-ramdump hook");

static bool ready;
module_param(ready, bool, 0444);
MODULE_PARM_DESC(ready, "Required DT resources and secure IO call are available");

static int last_error = -ENODEV;
module_param(last_error, int, 0444);
MODULE_PARM_DESC(last_error, "Last firmware/interface setup status");

static unsigned long tcsr_address;
module_param(tcsr_address, ulong, 0444);
MODULE_PARM_DESC(tcsr_address, "Download-mode register derived from Device Tree");

static void __iomem *dump_destination;

static bool qweovo_ramdump_available(void)
{
	return enabled && READ_ONCE(ready) && READ_ONCE(vendor_prepared) &&
		READ_ONCE(vendor_live);
}

bool qweovo_kernel_info_inode(const struct inode *inode)
{
	return proc_inode_data_or_null(inode) == &kernel_info_marker;
}

bool qweovo_public_debug_inode(const struct inode *inode)
{
	void *marker = proc_inode_data_or_null(inode);

	return marker == &panic_ctl_marker || marker == &kernel_info_marker;
}

static int __init qweovo_panic_action_setup(char *value)
{
	if (!value)
		return -EINVAL;
	if (!strcmp(value, "reboot"))
		panic_action = QWEOVO_PANIC_REBOOT;
	else if (!strcmp(value, "ramdump"))
		panic_action = QWEOVO_PANIC_RAMDUMP;
	else
		return -EINVAL;
	return 0;
}
early_param("qweovo_panic_action", qweovo_panic_action_setup);

static int __init qweovo_oops_continue_setup(char *value)
{
	bool attempt;
	int ret;

	if (!value)
		return -EINVAL;
	ret = kstrtobool(value, &attempt);
	if (!ret)
		WRITE_ONCE(panic_on_oops, !attempt);
	return ret;
}
early_param("qweovo_oops_continue", qweovo_oops_continue_setup);

static int __init qweovo_ramdump_setup(char *value)
{
	return value ? kstrtobool(value, &enabled) : -EINVAL;
}
early_param("qweovo_ramdump", qweovo_ramdump_setup);

/*
 * Panic context cannot allocate memory, take a sleeping lock, or use a
 * deferred SCM waitqueue. Both calls here fit in registers. Interrupted
 * calls follow the QCOM a6 resume convention, with a bounded retry count.
 */
static long qweovo_scm_atomic(u32 function, u32 nargs, unsigned long arg0,
			      unsigned long arg1, struct arm_smccc_res *res)
{
	struct arm_smccc_quirk quirk = { .id = ARM_SMCCC_QUIRK_QCOM_A6 };
	unsigned long call;
	unsigned int attempt;

	call = ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				 ARM_SMCCC_OWNER_SIP, function);
	for (attempt = 0; attempt < SCM_RESUME_LIMIT; attempt++) {
		arm_smccc_smc_quirk(call, nargs, arg0, arg1, 0, 0,
				    quirk.state.a6, 0, res, &quirk);
		if (res->a0 != SCM_INTERRUPTED)
			return (long)res->a0;
		call = res->a0;
	}

	return -ETIMEDOUT;
}

static int qweovo_set_dump_request(u32 mode)
{
	struct arm_smccc_res res;
	long ret;

	if (!enabled || !READ_ONCE(ready))
		return -ENODEV;

	ret = qweovo_scm_atomic(SCM_IO_WRITE, 2, tcsr_address, mode, &res);
	WRITE_ONCE(last_error, (int)ret);
	if (!ret)
		WRITE_ONCE(last_dump_request, mode);
	return ret;
}

/* Called after vendor panic notifiers; never return from panic itself. */
void qweovo_panic_prepare(void)
{
	int action = READ_ONCE(panic_action);
	int ret;

	/* Both exposed actions reboot; retain any positive native delay. */
	if (!READ_ONCE(panic_timeout))
		WRITE_ONCE(panic_timeout, -1);
	if (action == QWEOVO_PANIC_RAMDUMP && enabled && READ_ONCE(ready)) {
		/* QCOM_DOWNLOAD_DEST_QPST = 0: preserve RAM for USB collection. */
		writel_relaxed(0, dump_destination);
		/* Publish the USB destination before arming the firmware request. */
		wmb();
		ret = qweovo_set_dump_request(SCM_FULL_DUMP);
		if (!ret) {
			panic_reboot_mode = REBOOT_WARM;
			pr_emerg("full dump requested over USB; warm reset to XBL\n");
			return;
		}
		pr_emerg("cannot arm full dump: firmware status %d\n", ret);
	}

	/* The downstream notifier may have re-armed DLOAD: clear it last. */
	if (enabled && READ_ONCE(ready)) {
		ret = qweovo_set_dump_request(0);
		if (ret)
			pr_emerg("cannot clear download mode: status %d\n", ret);
	}
	panic_reboot_mode = REBOOT_COLD;
	pr_emerg("normal restart selected\n");
}

/*
 * GKI loads this downstream driver from the vendor ramdisk. Set its
 * existing parameter before probe, so probe does not disable SDI and so
 * its normal reboot notifier can still clear dump mode on clean reboots.
 * This does not replace or register another qcom_scm/dload platform driver.
 */
static int qweovo_ramdump_module(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct module *mod = data;
	unsigned int i;
	char *args;

	if (!enabled || !READ_ONCE(ready) ||
	    strcmp(mod->name, "qcom_dload_mode"))
		return NOTIFY_DONE;
	if (action == MODULE_STATE_GOING) {
		WRITE_ONCE(vendor_live, false);
		return NOTIFY_DONE;
	}
	if (action == MODULE_STATE_LIVE) {
		WRITE_ONCE(vendor_live, true);
		/* Preserve SDI availability, but only arm a dump in panic(). */
		if (qweovo_set_dump_request(0))
			pr_warn("cannot clear probe-time download request\n");
		return NOTIFY_DONE;
	}
	if (action != MODULE_STATE_COMING)
		return NOTIFY_DONE;

	for (i = 0; i < mod->num_kp; i++) {
		if (strcmp(mod->kp[i].name, "download_mode"))
			continue;

		/* COMING runs before parse_args() in this 5.10 module loader. */
		args = kasprintf(GFP_KERNEL, "%s download_mode=1",
				 mod->args ? mod->args : "");
		if (!args) {
			pr_warn("cannot append vendor download_mode argument\n");
			return NOTIFY_DONE;
		}
		kfree(mod->args);
		mod->args = args;
		WRITE_ONCE(vendor_prepared, true);
		pr_info("vendor qcom_dload_mode.download_mode=1 requested\n");
		return NOTIFY_DONE;
	}

	pr_warn("vendor download_mode parameter was not found\n");
	return NOTIFY_DONE;
}

static struct notifier_block qweovo_ramdump_module_nb = {
	.notifier_call = qweovo_ramdump_module,
};

static int __init qweovo_ramdump_init(void)
{
	struct device_node *scm, *tcsr, *destination;
	struct arm_smccc_res result;
	struct resource resource;
	u32 offset;
	long ret;

	/* This hook has only been prepared for the user's verified cape DT. */
	if (!enabled || !of_machine_is_compatible("qcom,cape"))
		return 0;

	scm = of_find_compatible_node(NULL, NULL, "qcom,scm");
	if (!scm)
		goto no_interface;
	tcsr = of_parse_phandle(scm, "qcom,dload-mode", 0);
	ret = of_property_read_u32_index(scm, "qcom,dload-mode", 1, &offset);
	of_node_put(scm);
	if (!tcsr)
		goto no_interface;
	if (ret || of_address_to_resource(tcsr, 0, &resource)) {
		of_node_put(tcsr);
		goto no_interface;
	}
	of_node_put(tcsr);
	if ((offset & 3) || resource_size(&resource) < sizeof(u32) ||
	    offset > resource_size(&resource) - sizeof(u32))
		goto no_interface;
	tcsr_address = resource.start + offset;

	destination = of_find_compatible_node(NULL, NULL,
					      "qcom,msm-imem-dload-type");
	if (!destination)
		goto no_interface;
	if (of_address_to_resource(destination, 0, &resource) ||
	    resource_size(&resource) < sizeof(u32)) {
		of_node_put(destination);
		goto no_interface;
	}
	dump_destination = of_iomap(destination, 0);
	of_node_put(destination);
	if (!dump_destination)
		goto no_interface;

	ret = qweovo_scm_atomic(SCM_INFO_IS_CALL_AVAIL, 1,
		(ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT) | SCM_IO_WRITE,
		0, &result);
	if (ret || !result.a1) {
		last_error = ret ? (int)ret : -EOPNOTSUPP;
		iounmap(dump_destination);
		dump_destination = NULL;
		pr_warn("secure IO write unavailable: %d; hook disabled\n",
			last_error);
		return 0;
	}

	ret = register_module_notifier(&qweovo_ramdump_module_nb);
	if (ret) {
		last_error = (int)ret;
		iounmap(dump_destination);
		dump_destination = NULL;
		return 0;
	}
	last_error = 0;
	WRITE_ONCE(ready, true);
	pr_info("cape panic policy ready; TCSR=%#lx; default normal reboot\n",
		tcsr_address);
	return 0;

no_interface:
	pr_warn("required Device Tree resources missing; hook disabled\n");
	return 0;
}
core_initcall(qweovo_ramdump_init);

static const char *qweovo_action_name(int action)
{
	return action == QWEOVO_PANIC_RAMDUMP ? "ramdump" : "reboot";
}

static int qweovo_panic_ctl_show(struct seq_file *m, void *unused)
{
	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	mutex_lock(&panic_ctl_lock);
	seq_printf(m, "oops_continue=%d\npanic_action=%s\n",
		   !READ_ONCE(panic_on_oops),
		   qweovo_action_name(READ_ONCE(panic_action)));
	seq_printf(m, "ramdump_ready=%d\nlast_error=%d\nlast_dump_request=%d\n",
		   qweovo_ramdump_available(), READ_ONCE(last_error),
		   READ_ONCE(last_dump_request));
	mutex_unlock(&panic_ctl_lock);
	return 0;
}

static int qweovo_panic_ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, qweovo_panic_ctl_show, NULL);
}

static ssize_t qweovo_panic_ctl_write(struct file *file,
				      const char __user *input, size_t count,
				      loff_t *position)
{
	char *buffer, *cursor, *token, *value;
	unsigned int seen = 0;
	bool attempt;
	int action, ret = -EINVAL;

	if (!qweovo_bootmenu_custom_enabled())
		return -EOPNOTSUPP;
	if (!count)
		return 0;
	if (count > PANIC_CTL_MAX_INPUT)
		return -E2BIG;
	buffer = memdup_user_nul(input, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (memchr(buffer, '\0', count))
		goto free;
	cursor = strim(buffer);
	if (strpbrk(cursor, "\r\n"))
		goto free;
	mutex_lock(&panic_ctl_lock);
	attempt = !READ_ONCE(panic_on_oops);
	action = READ_ONCE(panic_action);
	while ((token = strsep(&cursor, " \t"))) {
		if (!*token)
			continue;
		value = strchr(token, '=');
		if (!value)
			goto unlock;
		*value++ = '\0';
		if (!strcmp(token, "oops_continue")) {
			if (seen & 1)
				goto unlock;
			if (!strcmp(value, "0"))
				attempt = false;
			else if (!strcmp(value, "1"))
				attempt = true;
			else
				goto unlock;
			seen |= 1;
		} else if (!strcmp(token, "panic_action")) {
			if (seen & 2)
				goto unlock;
			if (!strcmp(value, "reboot"))
				action = QWEOVO_PANIC_REBOOT;
			else if (!strcmp(value, "ramdump"))
				action = QWEOVO_PANIC_RAMDUMP;
			else
				goto unlock;
			seen |= 2;
		} else {
			goto unlock;
		}
	}
	if (!seen)
		goto unlock;
	if (action == QWEOVO_PANIC_RAMDUMP && !qweovo_ramdump_available()) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	WRITE_ONCE(panic_on_oops, !attempt);
	WRITE_ONCE(panic_action, action);
	ret = 0;
unlock:
	mutex_unlock(&panic_ctl_lock);
free:
	kfree(buffer);
	return ret ? ret : count;
}

static const struct proc_ops qweovo_panic_ctl_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = qweovo_panic_ctl_open,
	.proc_read = seq_read,
	.proc_write = qweovo_panic_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int qweovo_kernel_info_show(struct seq_file *m, void *unused)
{
	const char *yes = "true", *no = "false";
	bool custom = qweovo_bootmenu_custom_enabled();

	mutex_lock(&panic_ctl_lock);
	seq_puts(m, "{\n  \"schema_version\":1,\n");
	seq_puts(m, "  \"kernel_id\":\"qweovo.secret_kernel\",\n");
	seq_puts(m, "  \"display_name\":\"Bootmenu-byQWEOVO\",\n");
	seq_printf(m, "  \"kernel_release\":\"%s\",\n", UTS_RELEASE);
	seq_puts(m, "  \"requires_root\":false,\n  \"capabilities\":{\n");
	seq_printf(m, "    \"cpu_control\":%s,\n",
		   custom && IS_ENABLED(CONFIG_CPU_PUBLIC_CONTROL) ? yes : no);
	seq_printf(m, "    \"ttl_control\":%s,\n",
		   custom && IS_ENABLED(CONFIG_IP_FORWARD_TTL_PUBLIC_CONTROL) ? yes : no);
	seq_printf(m, "    \"process_info\":%s,\n    \"process_control\":%s,\n",
		   custom && IS_ENABLED(CONFIG_PROCESS_PUBLIC_CONTROL) ? yes : no,
		   custom && IS_ENABLED(CONFIG_PROCESS_PUBLIC_CONTROL) ? yes : no);
	seq_printf(m, "    \"priority_uid_lock\":%s,\n",
		   process_nice_lock_supported() ? yes : no);
	seq_printf(m, "    \"meow_su\":%s,\n",
		   qweovo_bootmenu_meow_su_enabled() ? yes : no);
	seq_printf(m, "    \"sukisu_next_boot\":%s,\n    \"sukisu_signed_boot\":%s,\n",
		   qweovo_sukisu_next_supported() ? yes : no,
		   qweovo_sukisu_next_supported() ? yes : no);
	seq_printf(m, "    \"panic_control\":%s,\n    \"oops_try_continue\":%s,\n",
		   custom ? yes : no, custom ? yes : no);
	seq_printf(m, "    \"panic_reboot\":%s,\n    \"panic_ramdump\":%s,\n",
		   custom ? yes : no, custom ? yes : no);
	seq_printf(m, "    \"kpm\":%s\n  },\n", IS_ENABLED(CONFIG_KPM) ? yes : no);
	seq_puts(m, "  \"interfaces\":{\n");
	seq_puts(m, "    \"kernel_info\":\"/proc/qweovo_info\",\n");
	seq_puts(m, "    \"panic_control\":\"/proc/panic_ctl\",\n");
	seq_puts(m, "    \"cpu_control\":\"/proc/cpu_ctl\",\n");
	seq_puts(m, "    \"ttl_control\":\"/proc/ttl_ctl\",\n");
	seq_puts(m, "    \"process_info\":\"/proc/process_info\",\n");
	seq_puts(m, "    \"process_control\":\"/proc/process_ctl\",\n");
	seq_puts(m, "    \"priority_locks\":\"/proc/priority_locks\",\n");
	seq_puts(m, "    \"sukisu_next_boot\":\"/proc/qweovo_sukisu_next\"\n  },\n");
	seq_printf(m, "  \"boot_policy\":{\"menu\":%s,\"committed\":%s,\"sukisu\":%s,\"custom\":%s,\"status_spoofer\":%s,\"tee\":%s,\"persistence\":\"none\",\"mutable\":false},\n",
		   IS_ENABLED(CONFIG_QWEOVO_BOOT_MENU) ? yes : no,
		   qweovo_bootmenu_committed() ? yes : no,
		   qweovo_bootmenu_sukisu_enabled() ? yes : no, custom ? yes : no,
		   qweovo_bootmenu_status_spoofer_enabled() ? yes : no,
		   qweovo_bootmenu_tee_enabled() ? yes : no);
	seq_puts(m, "  \"panic_configuration\":{\n");
	seq_printf(m, "    \"oops_continue\":{\"type\":\"boolean\",\"default\":false,\"value\":%s},\n",
		   READ_ONCE(panic_on_oops) ? no : yes);
	seq_printf(m, "    \"panic_action\":{\"type\":\"enum\",\"default\":\"reboot\",\"values\":[\"reboot\",\"ramdump\"],\"value\":\"%s\"},\n",
		   qweovo_action_name(READ_ONCE(panic_action)));
	seq_printf(m, "    \"ramdump_ready\":%s,\n", qweovo_ramdump_available() ? yes : no);
	seq_puts(m, "    \"persistence\":\"until_reboot\"\n  },\n");
	seq_printf(m, "  \"priority_lock_configuration\":{\"api_version\":1,\"scope\":\"uid\",\"persistence\":\"until_reboot\",\"supported_policies\":[0,3],\"rule_count\":%u,\"app_id_min\":10000,\"app_id_max\":19999,\"max_rules\":%u},\n",
		   process_nice_lock_count(), PROCESS_NICE_LOCK_MAX_RULES);
	seq_printf(m, "  \"limits\":{\"panic_write_bytes\":%u,\"nice_min\":-20,\"nice_max\":19}\n}\n",
		   PANIC_CTL_MAX_INPUT);
	mutex_unlock(&panic_ctl_lock);
	return 0;
}

static int qweovo_kernel_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, qweovo_kernel_info_show, NULL);
}

static const struct proc_ops qweovo_kernel_info_ops = {
	.proc_flags = PROC_ENTRY_PERMANENT,
	.proc_open = qweovo_kernel_info_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init qweovo_panic_api_init(void)
{
	if (!qweovo_manager_proc_create("panic_ctl", 0666, &qweovo_panic_ctl_ops,
					&panic_ctl_marker))
		return -ENOMEM;
	if (!qweovo_manager_proc_create("qweovo_info", 0444, &qweovo_kernel_info_ops,
					&kernel_info_marker))
		return -ENOMEM;
	return 0;
}
late_initcall(qweovo_panic_api_init);
