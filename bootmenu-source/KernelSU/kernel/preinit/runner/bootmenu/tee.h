/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_TEE_H
#define QWEOVO_BOOTMENU_TEE_H

#ifdef QWEOVO_TEE_PAYLOAD
#define BM_TEE_AVAILABLE 1
#define BM_TEE_DIR "/dev/.meow-tee"
#define BM_TEE_ARCHIVE BM_TEE_DIR "/payload.tar.gz"
#define BM_TEE_LAUNCHER BM_TEE_DIR "/start.sh"

__asm__(
    ".pushsection .rodata.meow_tee,\"a\",%progbits\n"
    ".balign 8\n"
    "meow_tee_payload_start:\n"
    ".incbin \"" QWEOVO_TEE_PAYLOAD "\"\n"
    "meow_tee_payload_end:\n"
    "meow_tee_launcher_start:\n"
    ".incbin \"" QWEOVO_TEE_LAUNCHER "\"\n"
    "meow_tee_launcher_end:\n"
    ".popsection\n"
);

extern const unsigned char meow_tee_payload_start[], meow_tee_payload_end[];
extern const unsigned char meow_tee_launcher_start[], meow_tee_launcher_end[];

static int bm_stage_tee_file(const char *path, const unsigned char *data, size_t size)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_CREAT|O_EXCL|O_NOFOLLOW,0600);

    if (fd<0) return -1;
    while (size) {
        ssize_t n=write(fd,data,size);

        if (n<0 && errno==EINTR) continue;
        if (n<=0) {
            close(fd);
            return -1;
        }
        data+=n;
        size-=n;
    }
    return close(fd);
}

static int bm_stage_tee(void)
{
    if (syscall(__NR_mkdirat,AT_FDCWD,BM_TEE_DIR,0700)<0) return -1;
    if (bm_stage_tee_file(BM_TEE_ARCHIVE,meow_tee_payload_start,
            meow_tee_payload_end-meow_tee_payload_start)<0 ||
        bm_stage_tee_file(BM_TEE_LAUNCHER,meow_tee_launcher_start,
            meow_tee_launcher_end-meow_tee_launcher_start)<0) {
        unlink(BM_TEE_ARCHIVE);
        unlink(BM_TEE_LAUNCHER);
        syscall(__NR_unlinkat,AT_FDCWD,BM_TEE_DIR,AT_REMOVEDIR);
        return -1;
    }
    return 0;
}
#else
#define BM_TEE_AVAILABLE 0
static int bm_stage_tee(void)
{
    return -1;
}
#endif

#endif
