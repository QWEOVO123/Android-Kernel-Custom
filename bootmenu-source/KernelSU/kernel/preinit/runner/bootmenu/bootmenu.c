// SPDX-License-Identifier: GPL-2.0-only
/* Included after runner helpers; one static executable and one PID-1 handoff. */
#ifdef QWEOVO_BOOT_MENU
#include <stdarg.h>
static void bm_diag(const char *format, ...);
#include "drm.h"
#include "hardware.h"
#define BM_INPUT_DIAG(...) bm_diag(__VA_ARGS__)
#include "input.h"
#undef BM_INPUT_DIAG
#include "render.h"
#include "tee.h"
#include "manager.h"

static int bm_diag_fd=-1;

static void bm_diag(const char *format, ...)
{
    struct qweovo_boot_diag_line line={0};
    va_list args;
    int offset, saved_errno=errno;

    if (bm_diag_fd<0) return;
    offset=snprintf(line.text,sizeof(line.text),"t=%ldms ",now_ms());
    if (offset<0 || offset>=(int)sizeof(line.text)) return;
    va_start(args, format);
    vsnprintf(line.text+offset,sizeof(line.text)-offset,format,args);
    va_end(args);
    syscall(__NR_ioctl,bm_diag_fd,QWEOVO_BOOT_DIAG_APPEND,&line);
    errno=saved_errno;
}

static int bm_brightness(int *old)
{
    const char *base="/sys/class/backlight/panel0-backlight/";
    char path[128], buf[48], value[32];
    int fd,max,n;
    snprintf(path,sizeof(path),"%sbrightness",base);
    if (bm_read_text(path,buf,sizeof(buf))<0) return -1;
    *old=atoi(buf);
    snprintf(path,sizeof(path),"%smax_brightness",base);
    if (bm_read_text(path,buf,sizeof(buf))<0 || (max=atoi(buf))<=0) return -1;
    snprintf(path,sizeof(path),"%sbrightness",base);
    fd=open(path,O_WRONLY|O_CLOEXEC);
    if (fd<0) return -1;
    n=snprintf(value,sizeof(value),"%d\n",max/2 ? max/2 : 1);
    write(fd,value,n); close(fd);
    return 0;
}

static void bm_brightness_restore(int old)
{
    char text[32];
    int fd=open("/sys/class/backlight/panel0-backlight/brightness",O_WRONLY|O_CLOEXEC);
    int n=snprintf(text,sizeof(text),"%d\n",old);
    if (fd>=0) { write(fd,text,n); close(fd); }
}

static struct bm_result bm_select(int sukisu_allowed, unsigned initial_features,
                                  unsigned timeout_sec)
{
    struct bm_result result={QWEOVO_BOOT_CHOICE_MAGIC,BM_START,initial_features,0};
    struct bm_display display;
    struct bm_inputs inputs;
    struct bm_model model={.features=initial_features,.focus=BM_START_TARGET,
        .sukisu_allowed=sukisu_allowed,.tee_available=BM_TEE_AVAILABLE};
    long hardware_deadline, deadline=0, next_scan;
    int old_brightness=-1, brightness_changed=0, last_second=0, action=-1, redraw=1;
    int touch_log, touch_ready=0, timeout_active=1, frame_logged=0;

    if (timeout_sec<BM_MENU_TIMEOUT_MIN) timeout_sec=BM_MENU_TIMEOUT_MIN;
    if (timeout_sec>BM_MENU_TIMEOUT_MAX) timeout_sec=BM_MENU_TIMEOUT_MAX;
    bm_diag("runner-v2 select timeout_sec=%u",timeout_sec);
    bm_prepare_hardware();
    hardware_deadline=now_ms()+BM_HARDWARE_TIMEOUT_MS;
    for (;;) {
        if (!bm_display_open(&display)) break;
        if (now_ms()>=hardware_deadline) {
            bm_diag("display unavailable errno=%d",errno);
            klog("bootmenu: DRM unavailable; all features remain disabled");
            return result;
        }
        sleep_ms(100);
    }
    memset(&inputs,0,sizeof(inputs));
    for (;;) {
        bm_inputs_scan(&inputs);
        if (inputs.has_touch) break;
        if (now_ms()>=hardware_deadline) {
            if (inputs.count) break;
            bm_diag("no usable input device");
            klog("bootmenu: no usable input; all features remain disabled");
            bm_display_close(&display);
            return result;
        }
        sleep_ms(100);
    }
    klog("bootmenu: landscape %dx%d, inputs=%d",display.height,display.width,inputs.count);
    bm_diag("display ok %dx%d input=%d touch=%d",display.height,display.width,inputs.count,inputs.has_touch);
    touch_log=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    brightness_changed=!bm_brightness(&old_brightness);
    deadline=now_ms()+(long)timeout_sec*1000;
    next_scan=now_ms();
    bm_diag("input loop count=%d keys=%d touch=%d",inputs.count,inputs.keys,inputs.has_touch);
    while (action<0) {
        long now=now_ms();
        int changed=0, remaining=timeout_active ? (int)((deadline-now+999)/1000) : -1;
        if (timeout_active && now>=deadline) {
            bm_diag("exit reason=timeout");
            model.features=initial_features;
            action=BM_START;
            break;
        }
        if (now>=next_scan) {
            int previous_keys=inputs.keys, previous_touch=inputs.has_touch;
            bm_inputs_scan(&inputs);
            if (previous_keys!=inputs.keys || previous_touch!=inputs.has_touch)
                redraw=1;
            if (!touch_ready && bm_touch_firmware_ready(touch_log)) {
                touch_ready=1;
                bm_diag("touch firmware ready");
                bm_touch_irq_sample("firmware-ready");
                redraw=1;
            }
            next_scan=now+150;
        }
        if (redraw || remaining!=last_second) {
            bm_render(&display,&model,inputs.keys,
                      inputs.has_touch && (touch_ready || inputs.touch_reported),remaining);
            if (bm_display_present(&display)) {
                bm_diag("present fail %d",errno);
                klog("bootmenu: DRM flip failed: %d",errno);
                model.features=initial_features; action=BM_START; break;
            }
            if (!frame_logged) {
                bm_diag("frame submitted");
                bm_touch_irq_sample("first-frame");
                frame_logged=1;
            }
            last_second=remaining;
            redraw=0;
        }
        action=bm_inputs_poll(&inputs,&model,&changed);
        if (action==-2) {
            bm_diag("poll fail %d",errno);
            model.features=initial_features; action=BM_START; break;
        }
        if (timeout_active && inputs.user_active) {
            timeout_active=0;
            bm_diag("countdown cancelled by input");
            redraw=1;
        }
        if (changed)
            redraw=1;
    }
    result.action=action; result.features=model.features;
    bm_diag("input summary keys=%u contacts=%u syn=%u reads=%u errors=%u errno=%d",
            inputs.key_events,inputs.touch_contacts,inputs.syn_frames,
            inputs.read_batches,inputs.read_errors,inputs.last_read_errno);
    bm_diag("select exit action=%d",action);
    bm_touch_irq_sample("exit");
    if (touch_log>=0) close(touch_log);
    bm_inputs_close(&inputs);
    bm_display_close(&display);
    if (brightness_changed) bm_brightness_restore(old_brightness);
    return result;
}

static int qweovo_bootmenu_find_fd(void)
{
    int fd;
    for (fd=3; fd<256; fd++) {
        char path[48], link[96];
        long n;
        snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
        n=syscall(__NR_readlinkat,AT_FDCWD,path,link,sizeof(link)-1);
        if (n<=0) continue;
        link[n]=0;
        if (!strcmp(link,"anon_inode:[qweovo_boot_choice]")) return fd;
    }
    return -1;
}

static int bm_action(unsigned action)
{
    syscall(__NR_sync);
    if (action==BM_POWEROFF)
        return syscall(__NR_reboot,LINUX_REBOOT_MAGIC1,LINUX_REBOOT_MAGIC2,LINUX_REBOOT_CMD_POWER_OFF,0);
    if (action==BM_BOOTLOADER)
        return syscall(__NR_reboot,LINUX_REBOOT_MAGIC1,LINUX_REBOOT_MAGIC2,LINUX_REBOOT_CMD_RESTART2,"bootloader");
    return -1;
}

static int qweovo_bootmenu_run(int decision_fd)
{
    struct bm_result result={QWEOVO_BOOT_CHOICE_MAGIC,BM_START,QWEOVO_BOOT_CUSTOM,0};
    struct qweovo_boot_choice choice={
        .magic=QWEOVO_BOOT_CHOICE_MAGIC,.version=QWEOVO_BOOT_CHOICE_VERSION,
        .size=sizeof(choice)
    };
    struct qweovo_boot_request request={0};
    int pipefd[2], status=0, old_brightness=-1, i, firmware_saved=0;
    int sukisu_allowed=0, show_menu=1, request_ok=0;
    char old_firmware[256];
    pid_t worker;

    bm_diag_fd=decision_fd;
    if (is_charger_mode()) {
        show_menu=0;
        request.features=QWEOVO_BOOT_CUSTOM;
        request.timeout_sec=QWEOVO_MENU_TIMEOUT_DEFAULT;
        result.features=QWEOVO_BOOT_CUSTOM;
    } else if (read(decision_fd,&request,sizeof(request))==sizeof(request) &&
        request.magic==QWEOVO_BOOT_REQUEST_MAGIC &&
        request.version==QWEOVO_BOOT_REQUEST_VERSION &&
        request.size==sizeof(request) && !request.reserved) {
        request_ok=1;
        sukisu_allowed=!!(request.flags&QWEOVO_BOOT_REQUEST_GRANT);
        show_menu=!!(request.flags&QWEOVO_BOOT_REQUEST_SHOW_MENU);
        result.features=request.features;
        klog("bootmenu: request menu=%d grant=%d features=%u timeout=%u",
             show_menu,sukisu_allowed,request.features,request.timeout_sec);
        if (request.flags&QWEOVO_BOOT_REQUEST_BOOTLOADER) {
            klog("bootmenu: next boot target is bootloader");
            syscall(__NR_sync);
            if (bm_action(BM_BOOTLOADER)<0)
                klog("bootmenu: bootloader reboot failed: %d",errno);
            show_menu=0;
        }
    } else {
        klog("bootmenu: invalid boot request; showing menu with defaults");
        request.features=QWEOVO_BOOT_CUSTOM;
        request.timeout_sec=QWEOVO_MENU_TIMEOUT_DEFAULT;
        result.features=QWEOVO_BOOT_CUSTOM;
    }
    bm_diag("request charger=%d valid=%d menu=%d timeout_sec=%u",
            is_charger_mode(),request_ok,show_menu,request.timeout_sec);

    if (!is_charger_mode() && show_menu && !pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)) {
        char value[32];
        bm_diag("menu begin");
        if (bm_read_text("/sys/class/backlight/panel0-backlight/brightness",value,sizeof(value))>=0)
            old_brightness=atoi(value);
        if (bm_read_text(BM_FW_PATH,old_firmware,sizeof(old_firmware))>=0) {
            char *end=strchr(old_firmware,'\n');
            if (end) *end=0;
            firmware_saved=1;
            bm_set_firmware_path("/vendor/firmware");
        }
        worker=fork();
        if (!worker) {
            struct bm_result selected;
            setpgid(0,0); close(pipefd[0]);
            selected=bm_select(sukisu_allowed,request.features,request.timeout_sec);
            if (write(pipefd[1],&selected,sizeof(selected))!=sizeof(selected)) _exit(1);
            _exit(0);
        }
        close(pipefd[1]);
        if (worker>0) {
            /* Input cancels the countdown, so the UI may wait indefinitely. */
            long end=now_ms()+3600000L;
            pid_t waited=0;
            setpgid(worker,worker);
            while (now_ms()<end) {
                waited=waitpid(worker,&status,WNOHANG);
                if (waited==worker || (waited<0 && errno!=EINTR)) break;
                sleep_ms(20);
            }
            if (waited==worker && WIFEXITED(status) && !WEXITSTATUS(status)) {
                struct bm_result selected;
                if (read(pipefd[0],&selected,sizeof(selected))==sizeof(selected) &&
                    selected.magic==QWEOVO_BOOT_CHOICE_MAGIC && !selected.reserved &&
                    selected.action<=BM_BOOTLOADER && !(selected.features&~QWEOVO_BOOT_FEATURES)) result=selected;
                else bm_diag("worker result invalid");
            } else {
                bm_diag("worker failed wait=%d status=%d",(int)waited,status);
                kill(-worker,SIGKILL); kill(worker,SIGKILL);
                waitpid(worker,&status,WNOHANG);
                klog("bootmenu: worker failed; using persisted features");
                result.features=request.features;
            }
        }
        if (worker<0) bm_diag("worker fork failed");
        close(pipefd[0]);
        if (old_brightness>=0) bm_brightness_restore(old_brightness);
    }
    if (!show_menu) result.features|=QWEOVO_BOOT_CUSTOM;
    bm_diag("result action=%u",result.action);
    /* Parent cleanup also runs after a failed or killed UI worker. Only our
     * private names are removed; Android owns /dev/dri and /dev/input. */
    unlink(BM_DRM_NODE);
    if (firmware_saved) bm_set_firmware_path(old_firmware);
    for (i=0;i<BM_INPUTS;i++) {
        char path[64];
        snprintf(path,sizeof(path),"/dev/.qweovo-bootmenu-event%d",i);
        unlink(path);
    }
    klog("bootmenu: selected action=%u features=%u",result.action,result.features);
    bm_save_kernel_log();
    if (result.action!=BM_START) {
        klog("bootmenu: power action %u",result.action);
        if (bm_action(result.action)<0) klog("bootmenu: power action failed: %d; booting with all features off",errno);
        result.features=0;
    }
    choice.features=sukisu_allowed ? result.features : result.features & ~QWEOVO_BOOT_ROOT_MODES;
    if (((choice.features&(QWEOVO_BOOT_CUSTOM|QWEOVO_BOOT_MEOW_SU|QWEOVO_BOOT_SECURITY_AUDIT)) ||
         !access("/metadata/qweovo-security-audit-v1",F_OK)) && bm_stage_manager()<0) {
        klog("bootmenu: Manager payload staging failed; custom controls stay disabled");
        choice.features&=~(QWEOVO_BOOT_CUSTOM|QWEOVO_BOOT_MEOW_SU);
    }
    if ((choice.features&QWEOVO_BOOT_TEE) && bm_stage_tee()<0) {
        klog("bootmenu: TEE payload staging failed; continuing without it");
        choice.features&=~QWEOVO_BOOT_TEE;
    }
    if (write(decision_fd,&choice,sizeof(choice))!=sizeof(choice)) {
        klog("bootmenu: decision rejected: %d; leaving features disabled",errno);
        close(decision_fd);
        return 0;
    }
    close(decision_fd);
    bm_diag("submitting boot choice");
    return !!(choice.features&QWEOVO_BOOT_SUKISU);
}
#endif
