/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_INPUT_H
#define QWEOVO_BOOTMENU_INPUT_H
#include <linux/input.h>
#include <asm-generic/poll.h>

#define BM_INPUTS 32
#define BM_SLOTS 16
#ifndef BM_INPUT_DIAG
#define BM_INPUT_DIAG(...) ((void)0)
#endif
struct bm_slot { int id, x, y, seen_x, seen_y; };
struct bm_input {
    int fd, event_index, touch, keys, slot, primary, primary_id, blocked, dropped, down_target, moved;
    int start_x, start_y, xmin, xmax, ymin, ymax, slots;
    int power_down, ignore_power, volume_down;
    long power_at, volume_at;
    struct bm_slot points[BM_SLOTS];
};
struct bm_inputs {
    struct bm_input devices[BM_INPUTS];
    int count, keys, has_touch, touch_reported, user_active;
    unsigned key_events, touch_contacts, syn_frames, read_batches, read_errors;
    int last_read_errno;
    long power_single_at;
};

static int bm_bit(const unsigned long *bits, unsigned bit)
{
    return !!(bits[bit/(8*sizeof(*bits))] & (1UL << (bit%(8*sizeof(*bits)))));
}

static void bm_touch_resync(struct bm_input *d)
{
    int ids[BM_SLOTS+1], xs[BM_SLOTS+1], ys[BM_SLOTS+1], i;
    int size=(d->slots+1)*sizeof(int), has_x, has_y;
    struct input_absinfo current_slot;
    memset(ids,0xff,sizeof(ids)); ids[0]=ABS_MT_TRACKING_ID;
    xs[0]=ABS_MT_POSITION_X; ys[0]=ABS_MT_POSITION_Y;
    d->blocked=0; d->primary=-1; d->down_target=-1;
    if (bm_ioctl(d->fd,EVIOCGABS(ABS_MT_SLOT),&current_slot)>=0)
        d->slot=current_slot.value>=0 && current_slot.value<d->slots ? current_slot.value : -1;
    if (bm_ioctl(d->fd,EVIOCGMTSLOTS(size),ids)<0) d->blocked=1;
    has_x=bm_ioctl(d->fd,EVIOCGMTSLOTS(size),xs)>=0;
    has_y=bm_ioctl(d->fd,EVIOCGMTSLOTS(size),ys)>=0;
    for (i=0; i<d->slots; i++) {
        struct bm_slot *point=&d->points[i];
        point->id=ids[i+1];
        point->seen_x=has_x;
        point->seen_y=has_y;
        if (has_x) point->x=xs[i+1];
        if (has_y) point->y=ys[i+1];
        if (point->id>=0) d->blocked=1;
    }
}

static void bm_keys_resync(struct bm_input *d)
{
    unsigned long held[16]={0};
    d->power_down=0;
    d->volume_down=0;
    d->ignore_power=bm_ioctl(d->fd,EVIOCGKEY(sizeof(held)),held)>=0 && bm_bit(held,KEY_POWER);
}

static void bm_input_close(struct bm_input *d)
{
    syscall(__NR_ioctl,d->fd,EVIOCGRAB,0);
    close(d->fd);
}

static void bm_inputs_update(struct bm_inputs *inputs)
{
    int i;
    inputs->keys=0;
    inputs->has_touch=0;
    for (i=0; i<inputs->count; i++) {
        inputs->keys|=inputs->devices[i].keys;
        inputs->has_touch|=inputs->devices[i].touch;
    }
    if (inputs->keys!=BM_KEY_POWER) inputs->power_single_at=0;
}

static void bm_inputs_close(struct bm_inputs *inputs)
{
    int i;
    for (i=0; i<inputs->count; i++) bm_input_close(&inputs->devices[i]);
    inputs->count=0;
}

/* Keep existing descriptors and touch state when late devices are registered. */
static void bm_inputs_scan(struct bm_inputs *inputs)
{
    int i;
    for (i=0; i<BM_INPUTS && inputs->count<BM_INPUTS; i++) {
        char path[64], sysfs[96], name[128];
        unsigned long keys[16]={0}, props[1]={0};
        struct input_absinfo x,y,slots;
        struct bm_input *d=&inputs->devices[inputs->count];
        int fd,j;
        for (j=0; j<inputs->count; j++)
            if (inputs->devices[j].event_index==i) break;
        if (j<inputs->count) continue;
        snprintf(path,sizeof(path),"/dev/.qweovo-bootmenu-event%d",i);
        snprintf(sysfs,sizeof(sysfs),"/sys/class/input/event%d/dev",i);
        bm_ensure_node(sysfs,path);
        fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        memset(name,0,sizeof(name));
        if (bm_ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)<0 ||
            !strcmp(name,"hbtp_vm")) { close(fd); continue; }
        memset(d,0,sizeof(*d)); d->fd=fd; d->event_index=i;
        d->primary=-1; d->down_target=-1;
        if (bm_ioctl(fd,EVIOCGPROP(sizeof(props)),props)>=0 &&
            bm_bit(props,INPUT_PROP_DIRECT) &&
            bm_ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&x)>=0 &&
            bm_ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&y)>=0 &&
            bm_ioctl(fd,EVIOCGABS(ABS_MT_SLOT),&slots)>=0 &&
            x.maximum>x.minimum && y.maximum>y.minimum && slots.minimum==0 &&
            slots.maximum>=0 && slots.maximum<BM_SLOTS) {
            d->touch=1; d->xmin=x.minimum; d->xmax=x.maximum;
            d->ymin=y.minimum; d->ymax=y.maximum; d->slots=slots.maximum+1;
            bm_touch_resync(d);
        }
        if ((!strcmp(name,"gpio-keys") || !strcmp(name,"pmic_pwrkey") || !strcmp(name,"pmic_resin")) &&
            bm_ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys)>=0) {
            if (bm_bit(keys,KEY_VOLUMEUP)||bm_bit(keys,KEY_VOLUMEDOWN)) d->keys|=BM_KEY_VOLUME;
            if (bm_bit(keys,KEY_POWER)) d->keys|=BM_KEY_POWER;
            if (d->keys) bm_keys_resync(d);
        }
        if (!d->touch && !d->keys) { close(fd); continue; }
        /* Exclusive only for the menu lifetime, never into Android startup. */
        {
            int grab=syscall(__NR_ioctl,fd,EVIOCGRAB,1);
            if (grab<0) grab=-errno;
            BM_INPUT_DIAG("input device event=%d touch=%d keys=%d grab=%d",i,d->touch,d->keys,grab);
        }
        inputs->count++;
        klog("bootmenu: input %s (%s), touch=%d up=%d down=%d power=%d",
             path,name,d->touch,bm_bit(keys,KEY_VOLUMEUP),bm_bit(keys,KEY_VOLUMEDOWN),bm_bit(keys,KEY_POWER));
    }
    bm_inputs_update(inputs);
}

static int bm_touch_report(struct bm_input *d, struct bm_model *model, int *changed)
{
    int i, active=0, first=-1, x,y,target;
    struct bm_slot *p;
    for (i=0; i<d->slots; i++) if (d->points[i].id>=0) {
        active++; if (first<0) first=i;
    }
    if (d->blocked) {
        if (!active) { d->blocked=0; d->primary=-1; }
        return -1;
    }
    if (d->primary<0) {
        if (!active) return -1;
        p=&d->points[first];
        if (!p->seen_x || !p->seen_y) return -1;
        d->primary=first; d->primary_id=p->id; d->moved=active>1;
        bm_touch_map(p->x,p->y,d->xmin,d->xmax,d->ymin,d->ymax,&x,&y);
        d->start_x=x; d->start_y=y; d->down_target=bm_hit(model,x,y);
        if (d->down_target>=0 && !bm_locked(model,d->down_target)) {
            model->focus=d->down_target;
            *changed=1;
        }
        return -1;
    }
    p=&d->points[d->primary];
    if (active>1) d->moved=1;
    if (p->id<0) {
        target=d->down_target;
        d->primary=-1; d->down_target=-1;
        if (active) { d->blocked=1; return -1; }
        if (d->moved || target<0 || bm_locked(model,target)) return -1;
        *changed=1;
        return bm_activate(model,target);
    }
    if (p->id!=d->primary_id) { d->blocked=1; d->down_target=-1; return -1; }
    bm_touch_map(p->x,p->y,d->xmin,d->xmax,d->ymin,d->ymax,&x,&y);
    if (x-d->start_x>32 || d->start_x-x>32 || y-d->start_y>32 || d->start_y-y>32 ||
        bm_hit(model,x,y)!=d->down_target) d->moved=1;
    return -1;
}

static int bm_input_event(struct bm_inputs *inputs, struct bm_input *d,
                           const struct input_event *event, struct bm_model *model,
                           int *changed)
{
    if (event->type==EV_SYN && event->code==SYN_DROPPED) { d->dropped=1; return -1; }
    if (d->dropped) {
        if (event->type==EV_SYN && event->code==SYN_REPORT) {
            d->dropped=0;
            if (d->touch) bm_touch_resync(d);
            if (d->keys) bm_keys_resync(d);
            inputs->power_single_at=0;
        }
        return -1;
    }
    if (d->touch && event->type==EV_ABS) {
        if (event->code==ABS_MT_SLOT) {
            d->slot=event->value>=0 && event->value<d->slots ? event->value : -1;
        } else if (d->slot>=0) {
            struct bm_slot *p=&d->points[d->slot];
            if (event->code==ABS_MT_TRACKING_ID) {
                p->id=event->value;
                /* Protocol B can omit coordinates unchanged from the previous
                 * contact in this slot. Keep the cached position across IDs. */
            } else if (event->code==ABS_MT_POSITION_X) { p->x=event->value; p->seen_x=1; }
            else if (event->code==ABS_MT_POSITION_Y) { p->y=event->value; p->seen_y=1; }
        }
    }
    if (d->touch && event->type==EV_SYN && event->code==SYN_REPORT) {
        int old_primary=d->primary;
        int result=bm_touch_report(d,model,changed);

        inputs->syn_frames++;
        if (old_primary<0 && d->primary>=0 && !inputs->touch_contacts++)
            BM_INPUT_DIAG("first touch contact event=%d",d->event_index);
        if (d->primary>=0 && !inputs->touch_reported) {
            inputs->touch_reported=1;
            inputs->user_active=1;
            *changed=1;
        }
        return result;
    }
    if (!d->keys || event->type!=EV_KEY || event->value==2) return -1;
    if ((d->keys&BM_KEY_VOLUME) &&
        (event->code==KEY_VOLUMEUP || event->code==KEY_VOLUMEDOWN)) {
        if (!inputs->key_events++)
            BM_INPUT_DIAG("first key event=%d code=%u value=%d",d->event_index,event->code,event->value);
        if (event->value==1 && !d->volume_down) {
            inputs->user_active=1;
            d->volume_down=event->code;
            d->volume_at=now_ms();
            *changed=1;
        } else if (!event->value && d->volume_down==event->code) {
            d->volume_down=0;
            *changed=1;
            if (!(inputs->keys&BM_KEY_POWER) && now_ms()-d->volume_at>=750)
                return bm_activate(model,model->focus);
            bm_move_focus(model,event->code==KEY_VOLUMEUP ? -1 : 1);
        }
        return -1;
    }
    if ((d->keys&BM_KEY_POWER) && event->code==KEY_POWER) {
        long now=now_ms();
        if (!inputs->key_events++)
            BM_INPUT_DIAG("first key event=%d code=%u value=%d",d->event_index,event->code,event->value);
        if (d->ignore_power) { if (!event->value) d->ignore_power=0; return -1; }
        if (event->value==1) {
            inputs->user_active=1;
            d->power_down=1; d->power_at=now; return -1;
        }
        if (!event->value && d->power_down) {
            d->power_down=0;
            if (now-d->power_at>1500) return -1;
            *changed=1;
            if (inputs->keys&BM_KEY_VOLUME) return bm_activate(model,model->focus);
            if (inputs->power_single_at && now-inputs->power_single_at<=400) {
                inputs->power_single_at=0;
                return bm_activate(model,model->focus);
            }
            inputs->power_single_at=now;
        }
    }
    return -1;
}

static int bm_inputs_poll(struct bm_inputs *inputs, struct bm_model *model, int *changed)
{
    struct pollfd fds[BM_INPUTS];
    struct timespec timeout={.tv_nsec=50000000};
    int i,n,kept=0;
    for (i=0; i<inputs->count; i++) {
        fds[i].fd=inputs->devices[i].fd; fds[i].events=POLLIN; fds[i].revents=0;
    }
    n=syscall(__NR_ppoll,fds,inputs->count,&timeout,NULL,0);
    if (n<0 && errno!=EINTR) return -2;
    for (i=0; i<inputs->count; i++) if (fds[i].revents&POLLIN) {
        struct input_event events[32];
        int bytes,j;
        while ((bytes=read(fds[i].fd,events,sizeof(events)))>0) {
            inputs->read_batches++;
            for (j=0; j<bytes/(int)sizeof(events[0]); j++) {
                int result=bm_input_event(inputs,&inputs->devices[i],&events[j],model,changed);
                if (result>=0) return result;
            }
        }
        if (bytes<0 && errno!=EAGAIN && errno!=EINTR) {
            inputs->read_errors++;
            inputs->last_read_errno=errno;
        }
    }
    for (i=0; i<inputs->count; i++) {
        struct bm_input *d=&inputs->devices[i];
        if (fds[i].revents&(POLLERR|POLLHUP|POLLNVAL)) {
            char path[64];
            BM_INPUT_DIAG("input lost event=%d poll_flags=%d",d->event_index,fds[i].revents);
            bm_input_close(d);
            snprintf(path,sizeof(path),"/dev/.qweovo-bootmenu-event%d",d->event_index);
            unlink(path);
            *changed=1;
        } else {
            if (kept!=i) inputs->devices[kept]=*d;
            kept++;
        }
    }
    if (kept!=inputs->count) {
        inputs->count=kept;
        bm_inputs_update(inputs);
    }
    if (inputs->keys==BM_KEY_POWER && inputs->power_single_at && now_ms()-inputs->power_single_at>400) {
        inputs->power_single_at=0;
        bm_move_focus(model,1);
        *changed=1;
    }
    return -1;
}
#endif
