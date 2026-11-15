/* Host test: execute the real static loader with simulated kernel syscalls. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>

static char fixture[256], opened[1024][80], loaded[128][80];
static int loaded_count, fail_leaf;
static const char *command_line="msm-drm.dsi_display0=\"panel: value\" ignored.foo=1 msm_drm.test=2";
static const char *expected_parameters="dsi_display0=\"panel: value\" test=2";
static long now_ms(void) { return 100; }
static void klog(const char *fmt, ...) { (void)fmt; }
static void bm_diag(const char *fmt, ...) { (void)fmt; }
static int bm_read_text(const char *path,char *out,int size)
{
    assert(!strcmp(path,"/proc/cmdline"));
    assert((int)strlen(command_line)<size);
    strcpy(out,command_line); return strlen(out);
}
static int simulated_open(const char *path,int flags,...)
{
    char real[512];
    const char *base=strrchr(path,'/');
    snprintf(real,sizeof(real),"%s/%s",fixture,base ? base+1 : path);
    int fd=open(real,flags,0600);
    if (fd>=0) { assert(fd<1024); snprintf(opened[fd],80,"%s",base+1); }
    return fd;
}
static int simulated_lstat(const char *path,struct stat *st)
{
    (void)st;
    if (!strcmp(path,"/sys/module/already_loaded")) return 0;
    errno=ENOENT; return -1;
}
static long simulated_syscall(long number,...)
{
    va_list args; va_start(args,number);
    int fd=va_arg(args,int);
    if (number==__NR_fsync) { va_end(args); return 0; }
    const char *params=va_arg(args,const char *); int flags=va_arg(args,int); va_end(args);
    assert(number==__NR_finit_module && !flags);
    assert(!strcmp(params,!strcmp(opened[fd],"msm_drm.ko") ? expected_parameters : ""));
    assert(loaded_count<128);
    snprintf(loaded[loaded_count++],80,"%s",opened[fd]);
    if (fail_leaf && !strcmp(opened[fd],"leaf.ko")) { errno=EINVAL; return -1; }
    return 0;
}
#define open simulated_open
#define lstat simulated_lstat
#define syscall simulated_syscall
#include "../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/hardware.h"
#undef open
#undef lstat
#undef syscall

static void dependencies(const char *text)
{
    char path[512];
    snprintf(path,sizeof(path),"%s/modules.dep",fixture);
    FILE *f=fopen(path,"w"); assert(f); fputs(text,f); fclose(f);
    bm_load_count=loaded_count=fail_leaf=0; bm_load_deadline=30000;
    assert(!bm_read_dependencies());
}
int main(void)
{
    snprintf(fixture,sizeof(fixture),"/tmp/qweovo-hw-test-XXXXXX"); assert(mkdtemp(fixture));
    const char *files[]={"leaf.ko","dep.ko","msm_drm.ko"};
    char path[512];
    for (int i=0;i<3;i++) {
        snprintf(path,sizeof(path),"%s/%s",fixture,files[i]);
        int fd=open(path,O_WRONLY|O_CREAT,0600); assert(fd>=0); close(fd);
    }
    const char *normal="/vendor/lib/modules/msm_drm.ko: /vendor/lib/modules/dep.ko /vendor/lib/modules/leaf.ko\n\n"
        "/vendor/lib/modules/dep.ko: /vendor/lib/modules/leaf.ko\n/vendor/lib/modules/leaf.ko:\n";
    dependencies(normal);
    assert(!bm_load_hardware_module("msm_drm.ko",0));
    assert(loaded_count==3);
    for (int i=0;i<3;i++) assert(!strcmp(loaded[i],files[i]));
    assert(!bm_load_hardware_module("msm_drm.ko",0) && loaded_count==3);
    assert(!bm_load_hardware_module("already-loaded.ko",0) && loaded_count==3);
    dependencies(normal); fail_leaf=1;
    assert(bm_load_hardware_module("msm_drm.ko",0)<0 && loaded_count==1);
    dependencies("/vendor/lib/modules/dep.ko: /vendor/lib/modules/msm_drm.ko\n"
                 "/vendor/lib/modules/msm_drm.ko: /vendor/lib/modules/dep.ko\n");
    assert(bm_load_hardware_module("msm_drm.ko",0)<0 && !loaded_count);
    dependencies("/data/local/tmp/msm_drm.ko:\n");
    assert(bm_load_hardware_module("msm_drm.ko",0)<0 && !loaded_count);
    dependencies(normal); bm_load_deadline=50;
    assert(bm_load_hardware_module("msm_drm.ko",0)<0 && !loaded_count);
    snprintf(path,sizeof(path),"%s/path",fixture);
    int fd=open(path,O_RDWR|O_CREAT|O_TRUNC,0600); assert(fd>=0); close(fd);
    bm_set_firmware_path("/vendor/firmware");
    fd=open(path,O_RDONLY); assert(fd>=0); char value[80]={0};
    assert(read(fd,value,sizeof(value))==sizeof("/vendor/firmware")-1); close(fd);
    assert(!strcmp(value,"/vendor/firmware"));
    bm_set_firmware_path("");
    fd=open(path,O_RDONLY); assert(fd>=0); value[0]=1;
    assert(read(fd,value,1)==1 && value[0]==0); close(fd); unlink(path);
    for (int i=0;i<3;i++) { snprintf(path,sizeof(path),"%s/%s",fixture,files[i]); unlink(path); }
    snprintf(path,sizeof(path),"%s/modules.dep",fixture); unlink(path); rmdir(fixture);
    puts("Hardware loader: dependency order, duplicates, loaded modules, errors, cycles, path restriction and deadline passed");
    return 0;
}
