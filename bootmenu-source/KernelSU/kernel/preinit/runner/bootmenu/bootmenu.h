/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_PREINIT_BOOTMENU_H
#define QWEOVO_PREINIT_BOOTMENU_H
#ifdef QWEOVO_BOOT_MENU
#include "uapi/qweovo_bootmenu.h"
#include <linux/reboot.h>
#include "model.h"

struct bm_result { unsigned magic, action, features, reserved; };
static int qweovo_bootmenu_run(int decision_fd);
static int qweovo_bootmenu_find_fd(void);
#endif
#endif
