/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_MODEL_H
#define QWEOVO_BOOTMENU_MODEL_H

#define BM_WIDTH 1280
#define BM_HEIGHT 800
#define BM_MENU_TIMEOUT_MIN 5
#define BM_MENU_TIMEOUT_MAX 120
#define BM_HARDWARE_TIMEOUT_MS 15000
#define BM_START 0
#define BM_POWEROFF 1
#define BM_BOOTLOADER 2
#define BM_KEY_VOLUME 1
#define BM_KEY_POWER 2
#define BM_ROOT_SUKISU 1
#define BM_ROOT_MEOW_SU 16
#define BM_MODULE_LOCK 64
#define BM_SELINUX_LOCK 128
#define BM_SECURITY_AUDIT 256
#define BM_SECURITY_MASK 448
#define BM_ROOT_MASK 17
#define BM_TAB_BOOT 0
#define BM_TAB_SU 1
#define BM_TAB_SECURITY 2
#define BM_FIRST_ROW 3
#define BM_START_TARGET 3
#define BM_BOOTLOADER_TARGET 4
#define BM_POWEROFF_TARGET 5
#define BM_CUSTOM_TARGET 6
#define BM_STATUS_TARGET 7
#define BM_TEE_TARGET 8
#define BM_TARGETS 9

struct bm_rect { int x, y, w, h; };
struct bm_model {
    unsigned features;
    int focus;
    int sukisu_allowed;
    int tee_available;
    int page;
};

static const struct bm_rect bm_targets[BM_TARGETS] = {
    {72, 184, 228, 62}, {72, 258, 228, 62}, {72, 332, 228, 62},
    {336, 662, 872, 78},
    {352, 211, 840, 62}, {352, 289, 840, 62},
    {352, 367, 840, 62}, {352, 445, 840, 62}, {352, 523, 840, 62}
};

static int bm_visible(const struct bm_model *m, int target)
{
    return m->page == BM_TAB_BOOT || target <= BM_CUSTOM_TARGET;
}

static int bm_hit(const struct bm_model *m, int x, int y)
{
    int i;
    for (i = 0; i < BM_TARGETS; i++) {
        const struct bm_rect *r = &bm_targets[i];
        if (bm_visible(m,i) && x >= r->x && y >= r->y &&
            x < r->x+r->w && y < r->y+r->h)
            return i;
    }
    return -1;
}

static int bm_locked(const struct bm_model *m, int target)
{
    if (target <= BM_START_TARGET) return 0;
    if (m->page == BM_TAB_SU && target > BM_BOOTLOADER_TARGET)
        return !m->sukisu_allowed;
    if (m->page == BM_TAB_SECURITY)
        return m->features & BM_ROOT_MASK;
    return m->page == BM_TAB_BOOT && target == BM_TEE_TARGET && !m->tee_available;
}

static unsigned bm_row_flag(const struct bm_model *m, int target)
{
    if (m->page == BM_TAB_SU) {
        if (target == BM_POWEROFF_TARGET) return BM_ROOT_SUKISU;
        if (target == BM_CUSTOM_TARGET) return BM_ROOT_MEOW_SU;
        return 0;
    }
    if (m->page == BM_TAB_SECURITY) {
        if (target == BM_BOOTLOADER_TARGET) return BM_SELINUX_LOCK;
        if (target == BM_POWEROFF_TARGET) return BM_MODULE_LOCK;
        return BM_SECURITY_AUDIT;
    }
    if (target == BM_CUSTOM_TARGET) return 2;
    if (target == BM_STATUS_TARGET) return 4;
    if (target == BM_TEE_TARGET) return 8;
    return 0;
}

static int bm_activate(struct bm_model *m, int target)
{
    unsigned flag;

    if (target < 0 || target >= BM_TARGETS || !bm_visible(m,target) || bm_locked(m,target))
        return -1;
    m->focus = target;
    if (target < BM_FIRST_ROW) {
        m->page = target;
        return -1;
    }
    if (target == BM_START_TARGET) return BM_START;
    flag = bm_row_flag(m,target);
    if (m->page == BM_TAB_SU) {
        m->features &= ~BM_ROOT_MASK;
        m->features |= flag;
        /* Root and the security locks never share a boot. */
        if (flag) m->features &= ~BM_SECURITY_MASK;
    } else if (flag) {
        m->features ^= flag;
    } else if (target == BM_BOOTLOADER_TARGET) {
        return BM_BOOTLOADER;
    } else if (target == BM_POWEROFF_TARGET) {
        return BM_POWEROFF;
    }
    return -1;
}

static void bm_move_focus(struct bm_model *m, int direction)
{
    do {
        m->focus=(m->focus+BM_TARGETS+direction)%BM_TARGETS;
    } while (!bm_visible(m,m->focus) || bm_locked(m,m->focus));
}

static void bm_touch_map(int x, int y, int xmin, int xmax, int ymin, int ymax,
                         int *out_x, int *out_y)
{
    long nx = x < xmin ? 0 : x > xmax ? xmax-xmin : x-xmin;
    long ny = y < ymin ? 0 : y > ymax ? ymax-ymin : y-ymin;
    *out_x = ymax > ymin ? ny * (BM_WIDTH-1) / (ymax-ymin) : 0;
    *out_y = xmax > xmin ? (BM_HEIGHT-1) - nx * (BM_HEIGHT-1) / (xmax-xmin) : 0;
}
#endif
