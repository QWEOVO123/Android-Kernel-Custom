/* Replay physical keys and repeated Protocol B taps through the menu handler. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/model.h"

static long clock_ms=1000;
static long now_ms(void) { return clock_ms; }
static int bm_ioctl(int fd, unsigned long request, void *value)
{
    (void)fd;
    (void)request;
    (void)value;
    return -1;
}
static void bm_ensure_node(const char *source, const char *dest)
{
    (void)source;
    (void)dest;
}
#define klog(...) ((void)0)
#include "../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/input.h"

static int send_event(struct bm_inputs *inputs, struct bm_input *device,
                      struct bm_model *model, int type, int code, int value)
{
    struct input_event event={.type=type,.code=code,.value=value};
    int changed=0;
    return bm_input_event(inputs,device,&event,model,&changed);
}

static int key_press(struct bm_inputs *inputs, struct bm_input *device,
                     struct bm_model *model, int code, long duration)
{
    assert(send_event(inputs,device,model,EV_KEY,code,1)==-1);
    clock_ms+=duration;
    return send_event(inputs,device,model,EV_KEY,code,0);
}

int main(void)
{
    struct bm_inputs inputs={.keys=BM_KEY_VOLUME|BM_KEY_POWER};
    struct bm_input volume={.keys=BM_KEY_VOLUME}, power={.keys=BM_KEY_POWER};
    struct bm_model model={.focus=BM_START_TARGET,.sukisu_allowed=1,.tee_available=1};
    assert(!inputs.user_active);
    assert(key_press(&inputs,&volume,&model,KEY_VOLUMEDOWN,100)==-1 && model.focus==BM_BOOTLOADER_TARGET);
    assert(key_press(&inputs,&volume,&model,KEY_VOLUMEUP,100)==-1 && model.focus==BM_START_TARGET);
    assert(key_press(&inputs,&power,&model,KEY_POWER,100)==BM_START);

    model.focus=BM_TAB_SU;
    assert(key_press(&inputs,&power,&model,KEY_POWER,100)==-1 && model.page==BM_TAB_SU);
    assert(key_press(&inputs,&volume,&model,KEY_VOLUMEDOWN,100)==-1 && model.focus==BM_TAB_SECURITY);
    assert(key_press(&inputs,&power,&model,KEY_POWER,100)==-1 && model.page==BM_TAB_SECURITY);
    assert(inputs.user_active);
    inputs.user_active=0;

    /* A repeated touch in the same slot can contain only the tracking ID. */
    struct bm_input touch={.touch=1,.slots=1,.primary=-1,.down_target=-1,
                          .xmax=799,.ymax=1279,.points={{.id=-1}}};
    for (int id=1; id<=2; id++) {
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_TRACKING_ID,id);
        if (id==1) {
            send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_POSITION_X,480);
            send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_POSITION_Y,500);
        }
        assert(send_event(&inputs,&touch,&model,EV_SYN,SYN_REPORT,0)==-1);
        assert(inputs.touch_reported);
        assert(inputs.user_active);
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_TRACKING_ID,-1);
        assert(send_event(&inputs,&touch,&model,EV_SYN,SYN_REPORT,0)==-1);
        assert(model.features==(id==1 ? BM_MODULE_LOCK : 0));
    }
    for (int page=BM_TAB_BOOT; page<=BM_TAB_SECURITY; page++) {
        model.page=page;
        model.features=BM_SELINUX_LOCK;
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_TRACKING_ID,10+page);
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_POSITION_X,99);
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_POSITION_Y,760);
        assert(send_event(&inputs,&touch,&model,EV_SYN,SYN_REPORT,0)==-1);
        send_event(&inputs,&touch,&model,EV_ABS,ABS_MT_TRACKING_ID,-1);
        assert(send_event(&inputs,&touch,&model,EV_SYN,SYN_REPORT,0)==BM_START);
        assert(model.features==BM_SELINUX_LOCK);
    }
    assert(inputs.key_events==12);
    assert(inputs.touch_contacts==5);
    assert(inputs.syn_frames==10);
    puts("Input replay passed: physical keys, tabs, repeated Security touch");
    return 0;
}
