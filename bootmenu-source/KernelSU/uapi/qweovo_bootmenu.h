/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_QWEOVO_BOOTMENU_H
#define _UAPI_LINUX_QWEOVO_BOOTMENU_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* A bounded diagnostic record on the inherited preinit decision descriptor. */
struct qweovo_boot_diag_line {
	char text[192];
};
#define QWEOVO_BOOT_DIAG_APPEND _IOW('Q', 0xd1, struct qweovo_boot_diag_line)

#define QWEOVO_BOOT_CHOICE_MAGIC 0x51424d31U
#define QWEOVO_BOOT_CHOICE_VERSION 1U
#define QWEOVO_BOOT_SUKISU 0x1U
#define QWEOVO_BOOT_CUSTOM 0x2U
#define QWEOVO_BOOT_STATUS_SPOOFER 0x4U
#define QWEOVO_BOOT_TEE 0x8U
#define QWEOVO_BOOT_MEOW_SU 0x10U
#define QWEOVO_BOOT_LOCK_MODULES 0x40U
#define QWEOVO_BOOT_LOCK_SELINUX 0x80U
#define QWEOVO_BOOT_SECURITY_AUDIT 0x100U
#define QWEOVO_BOOT_SECURITY (QWEOVO_BOOT_LOCK_MODULES | QWEOVO_BOOT_LOCK_SELINUX | \
			      QWEOVO_BOOT_SECURITY_AUDIT)
#define QWEOVO_BOOT_ROOT_MODES (QWEOVO_BOOT_SUKISU | QWEOVO_BOOT_MEOW_SU)
#define QWEOVO_BOOT_FEATURES (QWEOVO_BOOT_SUKISU | QWEOVO_BOOT_CUSTOM | \
			     QWEOVO_BOOT_STATUS_SPOOFER | QWEOVO_BOOT_TEE | \
			     QWEOVO_BOOT_MEOW_SU | QWEOVO_BOOT_SECURITY)

/* One PID 1 read returns the boot request for this boot. */
#define QWEOVO_BOOT_REQUEST_MAGIC 0x51425231U
#define QWEOVO_BOOT_REQUEST_VERSION 1U
#define QWEOVO_BOOT_REQUEST_SHOW_MENU 0x1U
#define QWEOVO_BOOT_REQUEST_GRANT 0x2U
#define QWEOVO_BOOT_REQUEST_BOOTLOADER 0x4U
struct qweovo_boot_request {
	__u32 magic;
	__u16 version;
	__u16 size;
	__u32 flags;
	__u32 features;
	__u32 timeout_sec;
	__u32 reserved;
};

/* One complete write commits the selected features for this boot. */
struct qweovo_boot_choice {
	__u32 magic;
	__u16 version;
	__u16 size;
	__u32 features;
	__u32 reserved;
};

/* Persistent boot settings, stored on the metadata partition. */
#define QWEOVO_FEATURES_MAGIC 0x51424631U
#define QWEOVO_FEATURES_VERSION 1U
#define QWEOVO_FEATURES_PERSIST (QWEOVO_BOOT_STATUS_SPOOFER | QWEOVO_BOOT_TEE | \
				 QWEOVO_BOOT_SECURITY)
#define QWEOVO_MENU_TIMEOUT_MIN 5U
#define QWEOVO_MENU_TIMEOUT_MAX 120U
#define QWEOVO_MENU_TIMEOUT_DEFAULT 60U
struct qweovo_boot_features {
	__u32 magic;
	__u16 version;
	__u16 size;
	__u32 features;
	__u32 timeout_sec;
	__u32 crc32;
	__u32 reserved;
};

/* Single-use next-boot target. */
#define QWEOVO_NEXT_MAGIC 0x51424e31U
#define QWEOVO_NEXT_VERSION 1U
#define QWEOVO_NEXT_NORMAL 0U
#define QWEOVO_NEXT_MENU 1U
#define QWEOVO_NEXT_BOOTLOADER 2U
struct qweovo_boot_next {
	__u32 magic;
	__u16 version;
	__u16 size;
	__u32 target;
	__u32 crc32;
	__u32 reserved;
};

#endif
