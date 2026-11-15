# Android 12 / Linux 5.10.233：前端 App 接口对接说明

更新时间：2026-10-28。本文对应当前源码中的公开控制接口。

**前提：设备必须编译并启动这份定制内核。安装 App 本身不会创建这些节点。**
当前完成了接口源码适配、相关 ARM64 对象编译和模拟测试；完整内核链接、真实 Android 应用访问及硬件行为尚未验证。

这四个节点供普通应用直接访问，不需要 `su`、UID 授权或 SukiSU 管理器授权。公开控制节点权限为 `0666`，进程信息节点为 `0444`。SELinux 已针对专用节点适配，不需要 App 去关闭 enforcing。

## 1. 接口总览

| 节点 | 读取 | 写入 | 作用范围 |
| --- | --- | --- | --- |
| `/proc/cpu_ctl` | 核心列表与在线/active 状态 | 单核上线、下线、设置在线集合 | 整个系统 |
| `/proc/ttl_ctl` | `0` 或 `1` | 切换正常递减/保留 TTL | 打开文件时的网络命名空间 |
| `/proc/process_info` | TSV 进程/线程表 | 不支持 | 打开文件时的 PID 命名空间可见任务 |
| `/proc/process_ctl` | 当前描述符的操作结果 | 调整单线程或整个进程的 nice | 打开文件时的 PID 命名空间 |

对应配置：

```text
CONFIG_CPU_PUBLIC_CONTROL=y
CONFIG_IP_FORWARD_TTL_PUBLIC_CONTROL=y
CONFIG_PROCESS_PUBLIC_CONTROL=y
```

重要区别：CPU 上下线影响全系统，不是“只让某个游戏使用这些核心”。进程表里的 `cpu` 也不是 CPU 亲和性。本项目目前没有公开的进程 CPU 亲和性、频率、温控、OOM 调整或实时调度接口。

## 2. 所有接口共用的调用规则

- 直接调用文件 API，不需要通过 shell；下文 shell 命令仅用于手工演示。
- 文件 I/O 可能阻塞，必须放在后台线程。Flutter / React Native / WebView 前端应通过异步原生桥调用，不能假设 JavaScript 可以直接访问这些路径。
- 每次 `write()` 提交一条完整命令。用 ASCII 编码，可以带一个结尾换行；不要写 BOM、JSON、多个命令或二进制结构。
- 写入上限按**字节数**计算，包含结尾换行。零字节写入不执行操作。
- 当前实现成功时返回完整写入长度，失败返回负 errno；Android `Os.write()` 会以异常暴露错误。不要只检查文件是否存在就显示“控制成功”。
- 不要按普通磁盘文件处理：`length()` / `stat.st_size` 可能为 0，但仍有内容。循环读到 EOF，不用 `fsync()`、临时文件替换或 `rename()`。
- 修改后应重新打开状态节点读取实际结果。复用读描述符时先 seek 到 0。
- 这些节点不是持久化配置文件。重启、进程重启、系统服务或其他控制器可能改变实际状态。

### 功能探测

分别实际打开四个节点，独立判断功能是否可用。只打开并关闭文件不会执行控制命令；探测写访问时不要发送测试命令。

| 现象 | 前端处理 |
| --- | --- |
| `ENOENT` | 对应功能未编译、未启动新内核或节点不可见；隐藏/禁用该功能 |
| `EACCES` / `EPERM` 发生在打开阶段 | 访问被系统环境拒绝，显示错误；不要伪装成“功能不存在” |
| 可以打开，但写入失败 | 显示操作错误并刷新真实状态，不能据此认定整个接口不可用 |

普通 Android 应用通常共享系统命名空间。若 App 使用容器、独立挂载或 PID/网络命名空间，按下文的命名空间规则处理。

## 3. CPU 核心控制：`/proc/cpu_ctl`

### 3.1 读取格式

```text
possible: 0-7
present: 0-7
online: 0-3,6
active: 0-3,6
hotpluggable: 0-7
```

以上为格式示例，不代表设备固定有八核。

| 字段 | 含义 |
| --- | --- |
| `possible` | 内核可能使用的 CPU 编号集合 |
| `present` | 当前存在的 CPU 集合，前端核心列表以它为基础 |
| `online` | 当前在线集合 |
| `active` | 当前 active 集合；不能假设始终等于 online |
| `hotpluggable` | 查询时被内核判断为可热插拔的集合，不保证之后的操作一定成功 |

集合语法：`0-3,6` 表示 `[0,1,2,3,6]`；空字段表示空集合。核心编号从 0 开始，不要硬编码核心数量。

### 3.2 写入格式

```sh
echo 'online 3' > /proc/cpu_ctl
echo 'offline 3' > /proc/cpu_ctl
echo 'set 0-3,6' > /proc/cpu_ctl
```

| 命令 | 作用 |
| --- | --- |
| `online N` | 上线编号为 N 的核心 |
| `offline N` | 下线编号为 N 的核心 |
| `set CPULIST` | 请求在线集合变为给定列表 |

单次最多 **4095 字节**。列表使用数字、逗号和连字符，如 `0,2,4-6`。

内核至少保留一个活跃核心，拒绝空目标集合；固件禁止下线的核心仍可能返回错误。重复请求已有状态通常成功，但仍受核心和操作支持条件约束。

批量操作先上线目标中缺少的核心，再下线多余核心。**不是事务**：中途失败时，前面的步骤可能已生效。无论成功还是失败，前端都应再次读取 `/proc/cpu_ctl`。

不同应用的请求按热插拔锁串行执行，没有独占控制权。接口不锁定系统/厂商温控与核心策略，也不强制改变开机核心状态。

### 3.3 界面映射

核心卡片显示 `present` 中的编号，开关状态来自 `online`；可另外显示 `active`。点击时保存用户目标，但完成后用重新读取的实际值更新界面。出现 `EBUSY` 时提示“最后活跃核心或热插拔暂不可用”，不要一直自动重试。

## 4. IPv4 转发 TTL：`/proc/ttl_ctl`

### 4.1 读取和切换

```sh
cat /proc/ttl_ctl
echo 1 > /proc/ttl_ctl
echo 0 > /proc/ttl_ctl
```

| 值 | 行为 | 建议界面文案 |
| --- | --- | --- |
| `0` | 转发时正常 TTL−1，并调整 IP 头校验和；默认值 | 正常递减 |
| `1` | 转发时保留原 TTL，不做配套的 TTL 校验和调整 | 保留 TTL |

读取结果为 `0\n` 或 `1\n`。单次最多 **32 字节**；只接受 0/1 及其前后空白。不能用它将 TTL 固定为 64、65 等任意数值。

这是 `net.ipv4.ip_forward_ttl_mode` 的公开别名，两者共用设置；原 sysctl 的权限没有放开。

### 4.2 行为边界

- 只控制已适配的软件 IPv4 转发路径，不影响本机新建报文的初始 TTL。
- 不控制 IPv6 Hop Limit，也不控制绕过这些钩子的硬件/BPF/厂商加速路径。
- 普通 IPv4 转发仍拒绝 `TTL <= 1` 的报文，保留模式也一样。
- 不负责开启 IP 转发、热点、NAT 或 VPN。
- 新网络命名空间默认模式为 0。每个描述符绑定**打开时**的网络命名空间，直到关闭；传递描述符或之后调用 `setns()` 不会重新绑定它。

界面若使用“保留转发 TTL”开关，应将 `checked=true` 映射到 `1`，不要将它反接为“正常模式”。

## 5. 进程与线程列表：`/proc/process_info`

### 5.1 格式与解析

```sh
cat /proc/process_info
```

格式为 **TSV**：第一行字段名，每行一个线程，字段之间是真正的制表符 `\t`。请按字段名解析，不要按空格切分，也不要丢掉末尾空的 `argv0`。

字段顺序：

```text
tid tgid ppid uid state policy nice rt_priority cpu start_ns group_start_ns kernel comm argv0
```

上面为了可读使用空格列出字段名；真实文件使用 TAB。

| 字段 | 类型建议 | 含义 |
| --- | --- | --- |
| `tid` | Int | 线程 ID；Linux 的每个线程都有独立 TID |
| `tgid` | Int | 所属进程 ID；`tid == tgid` 是进程主线程 |
| `ppid` | Int | 父进程 ID；命名空间外的父进程可能显示 0 |
| `uid` | Long | 初始用户命名空间的 Linux UID；不要假设等于唯一包名 |
| `state` | String | Linux 状态字符，如 R/S/D/T/t/Z/X/I 等 |
| `policy` | Int | Linux 调度策略编号 |
| `nice` | Int | 当前 nice，通常为 -20～19 |
| `rt_priority` | Int | 实时优先级字段，不是 nice；本接口不提供它的写入 |
| `cpu` | Int | 采样时当前/最后所在 CPU，不是允许使用的 CPU 集合 |
| `start_ns` | **十进制字符串** | 当前线程的原始启动时间标记，纳秒 |
| `group_start_ns` | **十进制字符串** | 线程组主线程的启动标记 |
| `kernel` | Boolean/0或1 | 1 表示内核线程，不能通过本控制接口改 nice |
| `comm` | String | 线程名，最多 TASK_COMM_LEN−1 字节；当前通常是 15 字节 |
| `argv0` | String | 首个命令名，最多 256 字节；不可用时为空 |

只输出 `argv0`，不输出其他命令行参数或环境变量。字符串中的 TAB、CR、LF、反斜杠分别按 `\011`、`\015`、`\012`、`\134` 等三位八进制形式转义，需**解码一次**。UTF-8 名称可能在字节边界截断，显示时使用容错解码。

`comm` 可能被截断，且进程/线程可自行修改名称。不要仅凭它识别包。可结合 UID、`argv0` 和 Android 包信息；同一个包可能有多个进程，同一个 UID 也可能对应多个包。前台 Activity、图标等不在这个接口中。

### 5.2 进程列表与线程详情

- 进程主页按 `tgid` 分组；有 `tid == tgid` 行时用它作为主行。
- 线程详情展示该 TGID 下的所有行，各线程 nice 可以不同。
- 进程表还会列出当前 PID 命名空间可见的内核线程；控制按钮需按 `kernel` 与 `policy` 决定是否可用。
- 进程扫描是实时遍历，不是整张表的原子快照。读取期间会有任务创建、退出、exec 或字段变化。
- 文件描述符绑定打开时的 PID 命名空间，不会因为之后 `setns()` 而改变。

## 6. 修改优先级：`/proc/process_ctl`

### 6.1 命令格式

```text
process TGID GROUP_START_NS NICE
thread TID START_NS NICE
```

| 参数 | 来源 |
| --- | --- |
| `TGID` | 列表的 tgid，必须是进程主线程 ID |
| `GROUP_START_NS` | 同一进程的 group_start_ns；主线程行中也等于 start_ns |
| `TID` | 要修改的具体线程 ID |
| `START_NS` | 该具体线程的 start_ns |
| `NICE` | -20～19；越小越优先，0 不是最高优先级 |

替换为列表中真实的值后，例如：

```sh
echo 'process 12345 9876543210 -5' > /proc/process_ctl
echo 'thread 12346 9876543999 5' > /proc/process_ctl
```

每次写入最多 **128 字节**，只允许一条指令。启动标记为必填，用于防止 PID 复用。**不能省略标记，也不能在 ESTALE 后自动换上新标记重放旧点击。** 应重新扫描并让上层重新确认目标。

仅支持用户态 `SCHED_NORMAL`（0）与 `SCHED_BATCH`（3）。内核线程、FIFO（1）、RR（2）、IDLE（5）、DEADLINE（6）等不支持修改。接口调用内核调度 API，保留策略与 reset-on-fork；用户自定义 utilization clamp 保留，非用户定义项遵循该 API 的默认处理。

nice 调整不是实时优先级调整，也不会固定 CPU 配额、频率、前台状态、OOM 或 cgroup。Android 服务、应用自身或其他控制器之后仍可能重设 nice。

### 6.2 整个进程的处理规则

`process` 收集当时存活的线程并逐一修改，最多 4096 个线程。后创建的线程不保证包含在内；线程快速增长还可能超过本次分配的快照容量而返回 E2BIG。

这不是事务。某个线程退出、改变调度策略或被拒绝时，之前的线程可能已经修改。收到错误也必须刷新列表，不能假设全部回滚。

同一个应用有主进程、服务进程等多个 TGID 时，分别获取标记、分别发起 `process` 指令。该指令不会自动处理某个包或 UID 的全部进程。

### 6.3 读取结果：必须使用同一个文件描述符

以 `O_RDWR` 打开后写入指令，seek 到 0，再读取：

```text
updated=3 error=0
```

| 字段 | 含义 |
| --- | --- |
| `updated` | 最近一次已解析操作成功处理的线程数，包含 nice 原本已满足的情况 |
| `error` | 0 表示操作成功，负数表示内核 errno；例如 -116 对应 ESTALE |

每个打开描述符的结果独立，新打开的文件是 `updated=0 error=0`。**先用 echo 写入、再用 cat 重新打开，读不到 echo 对应的结果。**

解析、拷贝、长度错误不会更新“最近一次已解析操作”的结果。因此 write 的 errno 优先于状态文本：不能因为状态里 `error=0` 就覆盖一个失败的 write。推荐每个请求新开一个 O_RDWR 描述符，仅写一次、读取结果后关闭。

## 7. Android Kotlin 原生桥示例

以下使用 Android `android.system.Os`（API 21+），能保留 errno。它是**阻塞式代码**，应由 App 的后台执行器调用。示例展示文件协议，尚未在完整 Android 工程中编译/真机验证。

```kotlin
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import java.io.ByteArrayOutputStream
import java.io.FileDescriptor
import java.io.IOException

object KernelPublicApi {
    const val CPU = "/proc/cpu_ctl"
    const val TTL = "/proc/ttl_ctl"
    const val INFO = "/proc/process_info"
    const val CTL = "/proc/process_ctl"

    data class NiceReply(
        val updated: Int?,
        val statusError: Int?,       // 内核文本：0 或负 errno
        val writeErrno: Int?,        // Android 异常：正 errno
        val statusReadErrno: Int?,
        val rawStatus: String
    ) {
        val success: Boolean get() =
            writeErrno == null && statusReadErrno == null && statusError == 0
    }

    private inline fun <T> withNode(
        path: String, flags: Int, block: (FileDescriptor) -> T
    ): T {
        val fd = Os.open(path, flags or OsConstants.O_CLOEXEC, 0)
        try {
            return block(fd)
        } finally {
            Os.close(fd)
        }
    }

    private fun readAll(fd: FileDescriptor): ByteArray {
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (true) {
            val n = Os.read(fd, buffer, 0, buffer.size)
            if (n == 0) break
            out.write(buffer, 0, n)
        }
        return out.toByteArray()
    }

    private fun writeOnce(fd: FileDescriptor, command: String, maxBytes: Int) {
        require(command.isNotEmpty())
        require(command.all { it in ' '..'~' || it == '\t' })
        val data = (command + "\n").toByteArray(Charsets.US_ASCII)
        require(data.size <= maxBytes)
        val n = Os.write(fd, data, 0, data.size)
        if (n != data.size) {
            // 不要把剩余半条命令再写一次，应刷新真实状态。
            throw IOException("Unexpected short control write: $n/${data.size}")
        }
    }

    fun readText(path: String): String = withNode(path, OsConstants.O_RDONLY) {
        readAll(it).toString(Charsets.UTF_8)
    }

    fun writeCommand(path: String, command: String, maxBytes: Int) {
        withNode(path, OsConstants.O_WRONLY) { writeOnce(it, command, maxBytes) }
    }

    fun setCpuOnline(cpu: Int, online: Boolean) {
        require(cpu >= 0)
        val verb = if (online) "online" else "offline"
        writeCommand(CPU, "$verb $cpu", 4095)
    }

    fun setCpuMask(list: String) {
        require(Regex("[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*").matches(list))
        writeCommand(CPU, "set $list", 4095)
    }

    fun readTtlMode(): Int {
        val mode = readText(TTL).trim().toInt()
        require(mode == 0 || mode == 1)
        return mode
    }

    fun setPreserveTtl(enabled: Boolean) {
        writeCommand(TTL, if (enabled) "1" else "0", 32)
    }

    private fun decodeProcEscapes(value: String): String =
        Regex("\\\\([0-7]{3})").replace(value) {
            it.groupValues[1].toInt(8).toChar().toString()
        }

    // 保留原始数值字符串，尤其不要将启动标记转换成浮点数。
    fun readTasks(): List<Map<String, String>> {
        val lines = readText(INFO).lineSequence().filter { it.isNotEmpty() }.iterator()
        if (!lines.hasNext()) return emptyList()
        val header = lines.next().split('\t')
        require(listOf("tid", "tgid", "start_ns", "group_start_ns").all { it in header })
        val result = ArrayList<Map<String, String>>()
        while (lines.hasNext()) {
            val fields = lines.next().split('\t', limit = header.size)
            if (fields.size != header.size) continue
            val row = header.zip(fields).toMap().toMutableMap()
            row["comm"] = decodeProcEscapes(row["comm"].orEmpty())
            row["argv0"] = decodeProcEscapes(row["argv0"].orEmpty())
            result.add(row)
        }
        return result
    }

    fun setNice(scope: String, id: Int, startNs: String, nice: Int): NiceReply {
        require(scope == "process" || scope == "thread")
        require(id > 0 && nice in -20..19)
        require(Regex("[1-9][0-9]{0,19}").matches(startNs))
        // startNs 必须来自刚读取的对应列表字段，最终范围由内核校验。
        return withNode(CTL, OsConstants.O_RDWR) { fd ->
            var writeErrno: Int? = null
            try {
                writeOnce(fd, "$scope $id $startNs $nice", 128)
            } catch (error: ErrnoException) {
                writeErrno = error.errno
            }
            var readErrno: Int? = null
            val raw = try {
                Os.lseek(fd, 0L, OsConstants.SEEK_SET)
                readAll(fd).toString(Charsets.US_ASCII).trim()
            } catch (error: ErrnoException) {
                readErrno = error.errno
                ""
            }
            val match = Regex("updated=([0-9]+) error=(-?[0-9]+)").matchEntire(raw)
            NiceReply(
                updated = match?.groupValues?.get(1)?.toIntOrNull(),
                statusError = match?.groupValues?.get(2)?.toIntOrNull(),
                writeErrno = writeErrno,
                statusReadErrno = readErrno,
                rawStatus = raw
            )
        }
    }
}
```

使用示例，均应在后台线程执行：

```kotlin
val cpuState = KernelPublicApi.readText(KernelPublicApi.CPU)
KernelPublicApi.setCpuOnline(cpu = 3, online = false)
val actualCpuState = KernelPublicApi.readText(KernelPublicApi.CPU)

KernelPublicApi.setPreserveTtl(true)
val actualTtlMode = KernelPublicApi.readTtlMode()

val tasks = KernelPublicApi.readTasks()
val target = tasks.first { it["tid"] == it["tgid"] && it["argv0"] == "com.example.game" }
val reply = KernelPublicApi.setNice(
    scope = "process",
    id = target.getValue("tgid").toInt(),
    startNs = target.getValue("group_start_ns"),
    nice = -5
)
// reply.success 只是本次操作结果，随后仍需重新读取任务实际 nice。
```

上面的 `first` 只是定位示例，正式 App 需处理目标不存在、同名进程、多个进程和刚退出等情况。读取到的 comm/argv0 当普通文本显示，不拼接 shell 或未转义 HTML。

CPU/TTL 的 `writeCommand` 失败会直接抛异常。nice 的打开失败仍抛异常；打开成功后的 write errno 会记录在 `NiceReply.writeErrno`。还应处理 `IOException` / `InterruptedIOException`、解析异常以及读取状态失败，不要吞掉异常后显示成功。

## 8. 前端桥接数据约定

Flutter、React Native 或 WebView 可以让原生层返回如下结构。此为 App 层示例结构，**不是向内核写入的 JSON 协议**：

```json
{
  "tid": 12345,
  "tgid": 12345,
  "uid": 10123,
  "nice": 0,
  "policy": 0,
  "startNs": "9876543210123456",
  "groupStartNs": "9876543210123456",
  "kernel": false,
  "comm": "com.example.gam",
  "argv0": "com.example.game"
}
```

`startNs` / `groupStartNs` 在 JSON 与桥接层都保持字符串。示例 Kotlin 的 `readTasks()` 返回原始字段字符串，转换到上面的 App DTO 时再转换 PID/nice/policy 等字段。

推荐操作流程：读取能力与状态 → 用户选择目标 → 发送单条命令 → 处理 errno/部分结果 → 重新读取状态 → 更新界面。进程详情的缓存键用 `(tid, start_ns)`，不能只用 PID/TID。

## 9. 错误码与界面处理

Android `ErrnoException.errno` 为正数；`process_ctl` 文本中的 `error` 为 0 或负数。按符号常量处理，不必硬编码数字。

| errno | 常见原因 | 前端处理 |
| --- | --- | --- |
| `ENOENT` | 节点不存在或不可见 | 标记对应功能不可用 |
| `EACCES` / `EPERM` | 打开被拒绝，或 CPU 受保护/固件拒绝操作 | 区分打开错误与操作错误，显示原因并刷新 |
| `EINVAL` | 命令、参数、TTL 值、空 CPU 集合、nice 范围或进程 ID 类型不合法 | 修正请求，不自动重放 |
| `ERANGE` | 部分 CPU 数值/列表解析超出范围 | 检查 CPU 列表及参数 |
| `ENODEV` | 指定 CPU 不存在/不可用 | 刷新 present 列表 |
| `EBUSY` | 最后活跃核心、热插拔被临时禁止或底层忙 | 保留实际状态，不循环强制关闭 |
| `EOPNOTSUPP` | CPU 操作不支持，或目标是内核线程/不支持的调度策略 | 禁用对应操作或显示“不支持” |
| `ESRCH` | 目标进程/线程已退出 | 刷新列表、移除过期目标 |
| `ESTALE` | PID 启动标记已过期/目标不匹配 | 不自动套用新标记，重新选择目标 |
| `EAGAIN` | 并发调度/厂商处理导致 nice 未达到请求值 | 刷新后重新评估 |
| `E2BIG` | 写入过长，或线程组超出快照容量/4096 上限 | 缩短指令；刷新线程数，不无限重试 |
| `ENOMEM` / `EFAULT` / `EIO` 等 | 内存、用户缓冲区、硬件/固件等底层错误 | 显示具体错误；批量操作后刷新实际状态 |

CPU 批量设置和进程线程组调整均可能**部分完成后报错**。发生异常时也要刷新，不要仅恢复旧的 UI 本地副本。

## 10. 持久化与尚未提供的能力

内核不替 App 保存调优配置。TTL 新命名空间默认 0；CPU 开机状态由系统启动流程决定；进程 nice 可以继承或被 Android 后续调整。不要将 PID、TID、启动标记作为跨重启的持久配置。

本说明覆盖四个新增公开节点。SukiSU/KPM、zram 不是这些节点的公开 API：KPM 仍需要其运行时与镜像注入步骤；当前没有新增 zram 调优接口。已有 root sysctl 和逐核 sysfs 接口可以继续存在，但普通 App 应使用本说明中的公开节点。

## 11. 验证与参考

现有验证包含：相关 ARM64 对象构建、功能关闭时的 SELinux 构建、控制函数模拟测试及源码差异检查。真机上的 Android app SELinux 域、热插拔、转发报文和调度效果仍待验证。Kotlin 示例按 Android 官方 API 签名编写，未在 Android SDK 工程中编译。

启动新内核后，可使用以下自测；网络拓扑和跨 UID 测试的准备步骤可能需要测试环境 root，这不代表业务接口需要 root：

```sh
python3 tools/testing/selftests/cpu-hotplug/cpu-public-control.py
python3 tools/testing/selftests/net/ip_forward_ttl.py --public
python3 tools/testing/selftests/proc/process-public-control.py
```

自测仅覆盖其声明的路径，不等同于完整系统并发验证。

- [CPU ABI](../Documentation/ABI/testing/procfs-cpu-public-control)
- [TTL ABI](../Documentation/ABI/testing/procfs-ttl-public-control)
- [进程信息/优先级 ABI](../Documentation/ABI/testing/procfs-process-public-control)
- [Android Os 官方 API](https://developer.android.com/reference/android/system/Os)
