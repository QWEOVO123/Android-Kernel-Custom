/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_HARDWARE_H
#define QWEOVO_BOOTMENU_HARDWARE_H

/* TB320FC loads its display in Android early-init. Only load the required
 * AVB-mounted vendor hardware here; never read KSU module lists or /data.
 * A static loader is needed because the APEX dynamic linker is not ready. */
#define BM_DEP_BYTES (256 * 1024)
#define BM_DEP_COUNT 1024
#define BM_FW_PATH "/sys/module/firmware_class/parameters/path"
struct bm_dependency { char *name, *deps; unsigned state; };
static char bm_dependency_text[BM_DEP_BYTES];
static struct bm_dependency bm_dependencies[BM_DEP_COUNT];
static int bm_dependency_count, bm_load_count;
static long bm_load_deadline;
static const char *bm_module_basename(const char *path);

static int bm_module_parameters(const char *name, char *out, int capacity)
{
    char command[8192], module[128];
    const char *base=bm_module_basename(name), *p, *start;
    int n=0, used=0, quoted, len, i;
    while (base[n] && strcmp(base+n,".ko") && n<(int)sizeof(module)-1) {
        module[n]=base[n]=='-' ? '_' : base[n]; n++;
    }
    module[n]=0; out[0]=0;
    if (bm_read_text("/proc/cmdline",command,sizeof(command))<0) return -1;
    p=command;
    while (*p) {
        while (*p==' ' || *p=='\t' || *p=='\n') p++;
        if (!*p) break;
        start=p; quoted=0;
        while (*p && (quoted || (*p!=' ' && *p!='\t' && *p!='\n'))) {
            if (*p=='"') quoted=!quoted;
            p++;
        }
        len=p-start;
        if (len<=n || start[n]!='.') continue;
        for (i=0;i<n;i++) if ((start[i]=='-' ? '_' : start[i])!=module[i]) break;
        if (i!=n) continue;
        len-=n+1; start+=n+1;
        if (quoted || len<=0 || used+len+2>=capacity) return -1;
        if (used) out[used++]=' ';
        memcpy(out+used,start,len); used+=len; out[used]=0;
    }
    return 0;
}

static const char *bm_module_basename(const char *path)
{
    const char *base=path;
    while (*path) { if (*path=='/') base=path+1; path++; }
    return base;
}

static int bm_module_present(const char *name)
{
    char path[160];
    const char *base=bm_module_basename(name);
    int n=snprintf(path,sizeof(path),"/sys/module/"), i=0;
    struct stat st;
    while (base[i] && strcmp(base+i,".ko") && n<(int)sizeof(path)-1) {
        path[n++]=base[i]=='-' ? '_' : base[i]; i++;
    }
    path[n]=0;
    return !lstat(path,&st);
}

static int bm_read_dependencies(void)
{
    const char *path="/vendor/lib/modules/modules.dep";
    char *line, *next, *colon;
    struct stat st;
    int fd=open(path,O_RDONLY|O_CLOEXEC), n, used=0;
    if (fd<0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>=BM_DEP_BYTES) {
        close(fd); return -1;
    }
    while (used<BM_DEP_BYTES-1 && (n=read(fd,bm_dependency_text+used,BM_DEP_BYTES-1-used))>0)
        used+=n;
    close(fd);
    if (n<0 || used!=st.st_size) return -1;
    bm_dependency_text[used]=0;
    bm_dependency_count=0;
    for (line=bm_dependency_text; line<bm_dependency_text+used; line=next) {
        next=strchr(line,'\n');
        if (next) *next++=0; else next=line+strlen(line);
        colon=strchr(line,':');
        if (!colon) continue;
        if (bm_dependency_count==BM_DEP_COUNT) return -1;
        *colon++=0;
        bm_dependencies[bm_dependency_count++]=(struct bm_dependency){line,colon,0};
    }
    return 0;
}

static int bm_load_hardware_module(const char *name, int depth)
{
    struct bm_dependency *entry=NULL;
    const char *p;
    char token[512], parameters[4096];
    int i, len, fd, ret, error;
    if (bm_module_present(name)) return 0;
    if (depth>64 || now_ms()>=bm_load_deadline) return -1;
    for (i=0;i<bm_dependency_count;i++)
        if (!strcmp(bm_module_basename(name),bm_module_basename(bm_dependencies[i].name))) {
            entry=&bm_dependencies[i]; break;
        }
    if (!entry || entry->state==1 || entry->state==3) return -1;
    if (entry->state==2) return 0;
    if (++bm_load_count>128) return -1;
    entry->state=1;
    p=entry->deps;
    while (*p) {
        while (*p==' ' || *p=='\t' || *p=='\r') p++;
        if (!*p) break;
        len=0;
        while (*p && *p!=' ' && *p!='\t' && *p!='\r') {
            if (len==(int)sizeof(token)-1) goto fail;
            token[len++]=*p++;
        }
        token[len]=0;
        if (bm_load_hardware_module(token,depth+1)) goto fail;
    }
    /* modules.dep belongs to the mounted read-only vendor image. Restrict
     * paths nonetheless: this loader cannot load user-provided modules. */
    if ((strncmp(entry->name,"/vendor/lib/modules/",20) &&
         strncmp(entry->name,"/vendor_dlkm/lib/modules/",25)) || strstr(entry->name,"..")) goto fail;
    /* finit_module does not automatically inherit module.* boot arguments.
     * In particular msm_drm requires the bootloader's dsi_display0 selection. */
    if (bm_module_parameters(entry->name,parameters,sizeof(parameters))) goto fail;
    fd=open(entry->name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) goto fail;
    klog("bootmenu: loading hardware %s",bm_module_basename(entry->name));
    ret=syscall(__NR_finit_module,fd,parameters,0);
    error=errno;
    close(fd);
    if (ret && error!=EEXIST) {
        klog("bootmenu: hardware %s failed: %d",name,error);
        goto fail;
    }
    entry->state=2;
    return 0;
fail:
    entry->state=3;
    return -1;
}

static void bm_prepare_hardware(void)
{
    static const char * const modules[]={
        "pinctrl-spmi-gpio.ko", "qcom-pon.ko", "pm8941-pwrkey.ko", "pwm-qti-lpg.ko",
        "i2c-msm-geni.ko", "spi-msm-geni.ko", "msm_drm.ko"
    };
    int i;
    if (bm_read_dependencies()) {
        klog("bootmenu: vendor hardware dependencies unavailable"); return;
    }
    bm_load_deadline=now_ms()+30000;
    for (i=0;i<(int)(sizeof(modules)/sizeof(modules[0]));i++) {
        int ret=bm_load_hardware_module(modules[i],0);
        klog("bootmenu: hardware root %s result=%d",modules[i],ret);
        bm_diag("hardware %s result=%d",modules[i],ret);
    }
}

static int bm_touch_firmware_ready(int fd)
{
    char record[8193];
    int n, attempts;

    if (fd<0) return 0;
    for (attempts=0; attempts<128; attempts++) {
        n=read(fd,record,sizeof(record)-1);
        if (n<0 && errno==EPIPE) continue;
        if (n<=0) break;
        record[n]=0;
        if (strstr(record,"[NVT-ts] nvt_update_firmware") &&
            strstr(record,"Update firmware success!")) return 1;
    }
    return 0;
}

static void bm_touch_irq_sample(const char *stage)
{
    char chunk[512], line[320];
    int fd=open("/proc/interrupts",O_RDONLY|O_CLOEXEC);
    int size, i, used=0;

    if (fd<0) {
        bm_diag("touch IRQ %s unavailable errno=%d",stage,errno);
        return;
    }
    while ((size=read(fd,chunk,sizeof(chunk)))>0) {
        for (i=0; i<size; i++) {
            if (chunk[i]=='\n') {
                line[used]=0;
                if (strstr(line,"NVT-ts")) {
                    bm_diag("touch IRQ %s: %s",stage,line);
                    close(fd);
                    return;
                }
                used=0;
            } else if (used<(int)sizeof(line)-1) {
                line[used++]=chunk[i];
            }
        }
    }
    close(fd);
    bm_diag("touch IRQ %s not found",stage);
}

static void bm_set_firmware_path(const char *path)
{
    int fd=open(BM_FW_PATH,O_WRONLY|O_CLOEXEC), n=strlen(path);
    if (fd<0) { klog("bootmenu: firmware path open failed: %d",errno); return; }
    /* param_set_copystring preserves newlines. Empty reset needs one NUL
     * byte so sysfs actually invokes the setter instead of a zero write. */
    if (write(fd,path,n ? n : 1)!=(n ? n : 1))
        klog("bootmenu: firmware path write failed: %d",errno);
    close(fd);
}

static void bm_save_kernel_log(void)
{
    char record[4096];
    int source=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    int dest, n, total=0, retries=0;
    if (source<0) return;
    dest=open("/metadata/qweovo-bootmenu-kernel.log",
              O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    if (dest>=0) {
        while (total<2*1024*1024 && retries++<4096) {
            n=read(source,record,sizeof(record));
            if (n<0 && errno==EPIPE) continue;
            if (n<=0) break;
            if (write(dest,record,n)!=n) break;
            total+=n;
        }
        syscall(__NR_fsync,dest); close(dest);
    }
    close(source);
}
#endif
