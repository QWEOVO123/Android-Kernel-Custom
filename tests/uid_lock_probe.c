/* On-device scheduler/credential regression probe, reserved unused UID only. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_UID 19999
static int failed;
static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name); fflush(stdout);
    if (!ok) failed = 1;
}
static int command(const char *line) {
    int fd = open("/proc/priority_locks", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = write(fd, line, strlen(line)); close(fd);
    return result == (int)strlen(line) ? 0 : -1;
}
static void *thread_probe(void *unused) {
    (void)unused;
    check("future pthread inherits rule", getpriority(PRIO_PROCESS, 0) == 7);
    check("pthread setpriority accepted", setpriority(PRIO_PROCESS, 0, 13) == 0);
    check("pthread setpriority constrained", getpriority(PRIO_PROCESS, 0) == 7);
    return NULL;
}
struct probe_attr {
    uint32_t size, policy; uint64_t flags;
    int32_t nice; uint32_t priority;
    uint64_t runtime, deadline, period;
};
static int parser_probe(void) {
    const char *invalid[] = {
        "lock 0 5", "lock 1000 5", "lock 99000 5", "lock -1 5",
        "lock 4294967296 5", "lock 19999 -21", "lock 19999 20",
        "lock 19999", "lock 19999 5 extra", "unlock 19999 extra",
        "unknown 19999", "lock 19999 5\nunlock 19999", "   "
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        errno = 0;
        int result = command(invalid[i]);
        char label[100]; snprintf(label, sizeof(label), "reject malformed command %u", i);
        check(label, result == -1 && errno == EINVAL);
    }
    char large[130]; memset(large, 'x', sizeof(large)); large[129] = 0;
    errno = 0;
    int result = command(large);
    check("reject command over 128 bytes", result == -1 && errno == E2BIG);
    int fd = open("/proc/priority_locks", O_WRONLY | O_CLOEXEC);
    const char embedded[] = "lock 19999 5\0extra";
    errno = 0;
    result = fd < 0 ? 0 : write(fd, embedded, sizeof(embedded) - 1);
    check("reject embedded NUL", result == -1 && errno == EINVAL);
    if (fd >= 0) close(fd);
    return failed;
}
int main(int argc, char **argv) {
    if (getuid()) return 2;
    if (argc == 2 && !strcmp(argv[1], "--parser")) return parser_probe();
    int before = getpriority(PRIO_PROCESS, 0), parent_to_child[2], child_to_parent[2];
    if (pipe(parent_to_child) || pipe(child_to_parent)) return 3;
    check("install unused UID rule", command("lock 19999 7\n") == 0);
    pid_t child = fork();
    if (child == 0) {
        check("setuid to test application", setgid(TEST_UID) == 0 && setuid(TEST_UID) == 0);
        check("credential transition applies rule", getpriority(PRIO_PROCESS, 0) == 7);
        check("native setpriority returns success", setpriority(PRIO_PROCESS, 0, 12) == 0);
        check("native setpriority remains locked", getpriority(PRIO_PROCESS, 0) == 7);
        struct probe_attr attr = {.size = sizeof(attr), .policy = SCHED_OTHER, .nice = 11};
        check("sched_setattr returns success", syscall(__NR_sched_setattr, 0, &attr, 0) == 0);
        check("sched_setattr remains locked", getpriority(PRIO_PROCESS, 0) == 7);
        pthread_t thread;
        int created = pthread_create(&thread, NULL, thread_probe, NULL);
        check("pthread created", created == 0);
        if (!created) pthread_join(thread, NULL);
        pid_t grandchild = fork();
        if (!grandchild) _exit(getpriority(PRIO_PROCESS, 0) == 7 ? 0 : 1);
        int status = 0; waitpid(grandchild, &status, 0);
        check("future fork inherits rule", grandchild > 0 && WIFEXITED(status) && !WEXITSTATUS(status));
        char signal = 1;
        write(child_to_parent[1], &signal, 1);
        read(parent_to_child[0], &signal, 1);
        check("rule update changes existing process", getpriority(PRIO_PROCESS, 0) == 9);
        write(child_to_parent[1], &signal, 1);
        read(parent_to_child[0], &signal, 1);
        check("native setpriority after unlock", setpriority(PRIO_PROCESS, 0, 12) == 0 && getpriority(PRIO_PROCESS, 0) == 12);
        _exit(failed);
    }
    char signal = 0; int status = 0;
    if (child > 0) {
        read(child_to_parent[0], &signal, 1);
        check("update existing UID rule", command("lock 19999 9\n") == 0);
        write(parent_to_child[1], &signal, 1);
        read(child_to_parent[0], &signal, 1);
        check("unlock UID", command("unlock 19999\n") == 0);
        write(parent_to_child[1], &signal, 1);
        waitpid(child, &status, 0);
        check("child regression checks", WIFEXITED(status) && !WEXITSTATUS(status));
    } else check("fork created", 0);
    check("install negative nice rule", command("lock 19999 -5\n") == 0);
    child = fork();
    if (!child) {
        int ok = setgid(TEST_UID) == 0 && setuid(TEST_UID) == 0 && getpriority(PRIO_PROCESS, 0) == -5;
        struct sched_param param = {.sched_priority = 0};
        ok &= sched_setscheduler(0, SCHED_OTHER | SCHED_RESET_ON_FORK, &param) == 0;
        pid_t next = fork();
        if (!next) _exit(getpriority(PRIO_PROCESS, 0) == -5 ? 0 : 1);
        waitpid(next, &status, 0);
        _exit(ok && next > 0 && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 1);
    }
    if (child > 0) waitpid(child, &status, 0);
    check("restart UID and RESET_ON_FORK remain locked", child > 0 && WIFEXITED(status) && !WEXITSTATUS(status));
    check("cleanup only reserved test UID", command("unlock 19999\n") == 0);
    check("root parent nice unaffected", before == getpriority(PRIO_PROCESS, 0));
    return failed;
}
