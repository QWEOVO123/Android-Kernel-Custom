/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_MANAGER_H
#define QWEOVO_BOOTMENU_MANAGER_H

#ifdef QWEOVO_MANAGER_PAYLOAD
#define BM_MANAGER_DIR "/dev/.meow-manager"
#define BM_MANAGER_JAR BM_MANAGER_DIR "/service.jar"
#define BM_MANAGER_SOCKET_DIR BM_MANAGER_DIR "/socket"
#define BM_SU_BIN_DIR BM_MANAGER_DIR "/su-bin"
#define BM_SU_BIN BM_SU_BIN_DIR "/su"

__asm__(
    ".pushsection .rodata.meow_manager,\"a\",%progbits\n"
    ".balign 8\n"
    "meow_manager_payload_start:\n"
    ".incbin \"" QWEOVO_MANAGER_PAYLOAD "\"\n"
    "meow_manager_payload_end:\n"
    ".popsection\n"
);

extern const unsigned char meow_manager_payload_start[], meow_manager_payload_end[];

static int bm_stage_manager_file(const char *path, const unsigned char *data,
                                 size_t remaining, unsigned mode, int gid)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_CREAT|O_EXCL|O_NOFOLLOW,mode);
    if (fd<0) return -1;
    while (remaining) {
        ssize_t n=write(fd,data,remaining);

        if (n<0 && errno==EINTR) continue;
        if (n<=0) goto fail;
        data+=n;
        remaining-=n;
    }
    if (syscall(__NR_fchown,fd,0,gid)<0 || syscall(__NR_fchmod,fd,mode)<0) goto fail;
    return close(fd);
fail:
    close(fd);
    unlink(path);
    return -1;
}

static int bm_stage_manager(void)
{
    if (syscall(__NR_mkdirat,AT_FDCWD,BM_MANAGER_DIR,0751)<0) return -1;
    if (syscall(__NR_mkdirat,AT_FDCWD,BM_MANAGER_SOCKET_DIR,0771)<0 ||
        syscall(__NR_fchownat,AT_FDCWD,BM_MANAGER_DIR,0,5998,0)<0 ||
        syscall(__NR_fchownat,AT_FDCWD,BM_MANAGER_SOCKET_DIR,0,5998,0)<0 ||
        syscall(__NR_fchmodat,AT_FDCWD,BM_MANAGER_DIR,0751)<0 ||
        syscall(__NR_fchmodat,AT_FDCWD,BM_MANAGER_SOCKET_DIR,0771)<0) return -1;
    return bm_stage_manager_file(BM_MANAGER_JAR,meow_manager_payload_start,
                                 meow_manager_payload_end-meow_manager_payload_start,0440,5998);
}

#else
static int bm_stage_manager(void)
{
    return -1;
}
#endif

#endif
