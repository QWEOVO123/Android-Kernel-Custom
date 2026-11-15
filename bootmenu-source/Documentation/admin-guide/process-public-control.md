# 免 root 进程信息与优先级接口

`CONFIG_PROCESS_PUBLIC_CONTROL=y` 提供两个普通应用可直接访问的节点：

| 节点 | 权限 | 用途 |
| --- | --- | --- |
| `/proc/process_info` | 0444 | 列出当前 PID 命名空间全部可见进程及线程，不按 UID 过滤 |
| `/proc/process_ctl` | 0666 | 修改任意用户进程/线程的 nice，无需 root 或授权 |

普通 Android 应用共享系统 PID 命名空间时，可以看到系统进程。独立 PID 命名空间
仍按其可见范围工作；文件打开时绑定命名空间并持有引用，直到关闭。

列表是 TSV，首行为字段名，每线程一行；`tid == tgid` 的行是进程主线程。
包含 UID、状态、调度策略、nice、RT 优先级、采样 CPU、启动时间、内核线程标记、
线程名及完整首个命令名（argv0 最多 256 字节）。其他命令行参数和环境不导出。
线程名最多 15 字节，不能仅凭线程名判断包名；可结合 UID、argv0 和 PackageManager。
字段中的制表符/换行/回车/反斜杠按八进制转义，详细约定见 ABI 文档。

```sh
cat /proc/process_info
# 从列表读取真实 ID 和启动时间后，例如：
echo 'process 12345 9876543210 -5' > /proc/process_ctl
echo 'thread 12346 9876543999 5' > /proc/process_ctl
```

调用格式为 `process TGID group_start_ns nice` 或 `thread TID start_ns nice`。
启动时间是防止 PID 复用误操作的必填标记，应使用 64 位整数或原始十进制字符串
传递；JavaScript 不要将它转成普通 Number。

nice 范围为 **-20 到 19**，数值越小越优先。这一版仅支持普通/BATCH 用户线程；
不修改内核线程、实时/Deadline/IDLE 策略。通过内核调度 API 更新，不直接改任务字段。
同一进程各线程可以有不同 nice，`process` 会处理当时收集到的全部活跃线程。

每个文件描述符有独立结果。用 `RandomAccessFile("/proc/process_ctl", "rw")`
或 `open(O_RDWR)` 写入后 seek 到 0，可读到 `updated=N error=ERRNO`；重新打开不会
读取先前描述符的结果。批量操作并非事务，发生错误时部分线程可能已经修改，需刷新
列表核对。新建线程、并发控制器、Android 调度服务可能随后改变实际结果。

接口不改变全局 SELinux enforcing，也不放开其他 `/proc` 或普通调度系统调用。
它仅对这两个私有标记节点放行所需文件访问，并在驱动内部提供公开 nice 控制。

启动新内核后可运行：

```sh
python3 tools/testing/selftests/proc/process-public-control.py
```

测试仅修改其创建的临时目标进程：控制器使用非 root UID，检查整组/单线程 nice、
策略保留、过期启动标记和非法输入。若由 root 启动，目标和控制器还使用不同 UID。
UID 测试不等同于真实 Android 应用 SELinux 域验证。当前云端未运行此定制内核，
完整内核链接、真实应用访问、退出/exec 并发及设备调度效果需启动后验证。

构建检查已通过：ARM64 的接口实现和 SELinux hooks，以及关闭所有公开控制接口时
的 SELinux hooks。未修改原调度系统调用或调度器核心源码，旧 CPU/TTL/KPM 配置保留。
提取实际控制、遍历和 argv0 读取函数，结合模拟任务/mm/调度原语检查了 nice 全范围、
线程组/单线程操作、PID 复用与命名空间查找、引用回收、部分失败、4097 线程拒绝、
内存错误和模拟 exec 时的 mm 绑定；ASan/UBSan 通过。容器限制导致未启用泄漏检测，
这些模型测试不替代实际运行内核的并发验证。

`0666` 是公开控制接口的明确设计，因此保留 checkpatch 的
`EXPORTED_WORLD_WRITABLE` 提示；新增文件还会提示维护者信息。
