# QWEOVO 内核身份、能力与 panic 配置接口（API 1）

当前内核：`5.10.233-android12-9-MadeByQWEOVO-Ramdump-UIDLock1`（协议主版本仍为 1）。新增 `priority_uid_lock` 能力和 `/proc/priority_locks`，完整协议见 [UID 优先级锁定](uid-priority-locks.md)。旧客户端可忽略新增字段。

待编译源码新增开机菜单：存在 `boot_policy` 时，`capabilities` 表示本次选择后可用的能力；`custom=false` 时公开控制能力均关闭，只读身份仍可读取。详见 [菜单状态协议](boot-menu-policy.md)。这不表示上面列出的已刷入 UIDLock1 已具备菜单。

所有下列操作均由普通 Android App 直接读写文件，不需要 root、su、管理器授权或额外 Android 权限。保持 SELinux Enforcing；内核只对带私有标记的专用节点放行。文件操作放在后台线程。

## 发现内核和生成 GUI

读取 UTF-8 JSON：`/proc/qweovo_info`（0444，只读）。

识别条件：

```text
schema_version == 1
kernel_id == "qweovo.secret_kernel"
```

`kernel_id` 是稳定的兼容性标识，版本升级保持不变；不依赖可能变化的 uname 名称。它用于接口识别，不是硬件签名或防伪认证。节点不存在、无法访问、JSON 无效或 schema 不支持时，前端显示不支持，不尝试写入。

主要字段：

| JSON 字段 | 含义及 GUI 用法 |
| --- | --- |
| `schema_version` | 当前协议主版本 1；未知字段应忽略 |
| `kernel_id` | 固定为 `qweovo.secret_kernel` |
| `display_name` | `MadeByQWEOVO` |
| `kernel_release` | 编译时的完整内核版本 |
| `requires_root` | 当前为 false |
| `capabilities` | CPU、TTL、进程、panic 等已编译能力；false 时隐藏或禁用对应页面 |
| `interfaces` | 各能力对应的 proc 节点路径 |
| `panic_configuration.oops_continue` | `type`、`default`、`value`，可生成布尔开关 |
| `panic_configuration.panic_action` | `type`、`values`、`default`、`value`，可生成枚举选择器 |
| `panic_configuration.ramdump_ready` | 此次开机的固件路径是否准备就绪；false 时禁用 Ramdump 选项 |
| `panic_configuration.persistence` | `until_reboot`，本次开机有效 |
| `limits.panic_write_bytes` | 单次写入上限 128 字节 |
| `limits.nice_min`、`nice_max` | 进程控制 nice 范围 -20 到 19 |

当前能力包括 `cpu_control`、`ttl_control`、`process_info`、`process_control`、`panic_control`、`oops_try_continue`、`panic_reboot`、`panic_ramdump`；`kpm=false`。能力字段表示编译支持，`ramdump_ready` 表示本次开机的准备状态，不代表新版本已主动触发故障验证。

真实设备返回的完整 JSON 样本见工作区 `dist/MadeByQWEOVO-NoKPM-Ramdump-PanicAPI2/device-validation/qweovo_info.json`。

## 配置 panic 策略

节点：`/proc/panic_ctl`，权限 0666。无需 su，使用一次完整 write 提交一条命令。

```text
oops_continue=0 panic_action=reboot
```

两项配置独立，可一次提交两项，也可只提交一项，未提交项保持当前值。

| 配置 | 默认 | 含义 |
| --- | --- | --- |
| `oops_continue=0` | 是 | 原生 `panic_on_oops=1`，Oops 转入 panic |
| `oops_continue=1` | 否 | 原生 `panic_on_oops=0`，部分异常尝试继续；不使真正的 panic 返回 |
| `panic_action=reboot` | 是 | panic 通知链结束后清除 Qualcomm 下载请求，按常规崩溃重启 |
| `panic_action=ramdump` | 否 | panic 通知链结束后请求 USB 完整转储并 warm reset，供 QMDC 采集 |

选择 Ramdump **不会立刻触发 panic、重启或进入 900E**。设置只决定以后真的发生 panic 时的行为。严重错误、init 死亡等仍可能 panic，`oops_continue=1` 不保证系统或 ADB 存活。

示例（普通 shell 或同等文件写入）：

```sh
echo 'oops_continue=1 panic_action=ramdump' > /proc/panic_ctl
echo 'oops_continue=0' > /proc/panic_ctl
echo 'oops_continue=0 panic_action=reboot' > /proc/panic_ctl
cat /proc/panic_ctl
```

读取是键值文本：

```text
oops_continue=0
panic_action=reboot
ramdump_ready=1
last_error=0
last_dump_request=0
```

`last_dump_request` 是本钩子最后成功提交给固件的请求值：0=关闭转储，16=完整转储，-1=尚未提交；它不是重新读取硬件寄存器的结果。`ramdump_ready=1` 表示可选，当前是否选择仍看 `panic_action`。

正常启动会在保留 SDI 调试能力的同时清除 vendor probe 期间临时设置的下载请求；常规 panic 路径还会在 vendor panic 回调后再次清除请求。普通关机/重启保留原有 vendor 处理。

## 输入与错误处理

- 用 UTF-8/ASCII 编码；推荐一次写入两项，最后可带一个换行。
- 单次不超过 128 字节，不拆成多次 write；写入成功返回完整字节数。
- 未知键、重复键、非法值、嵌入 NUL 和嵌入多行命令返回 `EINVAL`。
- 超长返回 `E2BIG`；选择未就绪的 Ramdump 返回 `EOPNOTSUPP`。
- 先完整校验再更新；错误输入不提交其中任何一项。
- 多次合法写入按顺序生效；多个 App 同时配置时以最后提交者为准。
- 写入后重新读取状态；GUI 以回读值更新，不假定旧缓存仍准确。
- 原生 `kernel.panic_on_oops` 与这里共享同一变量，其他工具修改时这里会反映当前值。

## 重启与开机早期配置

运行时设置不写磁盘，普通重启后恢复 `oops_continue=0`、`panic_action=reboot`。App 若需要记忆偏好，可保存到自身配置，开机后再应用；App 启动前的故障使用内核默认值。

编译/打包时可通过启动参数覆盖早期默认：

```text
qweovo_oops_continue=1 qweovo_panic_action=ramdump
```

`qweovo_ramdump=0` 禁用固件转储准备，但身份和配置接口仍存在，此时不能通过运行时接口选择 Ramdump。

两个 panic 动作都需要重启：原生 panic_timeout 为正时保留其延迟，为零时本接口的 panic 处理会改成立即重启。它没有“panic 后继续运行”或“永久停机”的模式，也不覆盖未经过 Linux panic 的固件复位、掉电等情况。

## Kotlin 对接

相邻 `QweovoKernelApi.kt` 提供免 root 的最小调用封装，读 JSON 后可直接使用元数据生成开关和枚举控件。没有 shell 调用和 su。调用方在后台线程执行，捕获 `IOException`、`ErrnoException` 等错误并展示回读结果。

## 验证范围

首版 PanicAPI1 已用无权限声明的 APK 在 UID 10298、`u:r:untrusted_app`、SELinux Enforcing 下通过 25 项，覆盖身份与能力 JSON、四种配置组合、错误输入、部分更新、只读身份节点及原有接口读取。其普通重启和恢复默认设置也已确认。

最终 PanicAPI2 已由无 root 的 UID 2000 shell 通过 22 项，包括新增的尾随字符拒绝和 128 字节边界；解锁后又由 UID 10301、`u:r:untrusted_app` 的普通 App 在 SELinux Enforcing 下通过全部 31 项。测试 App 没有权限声明、没有调用 su，测试后已移除，并恢复 `oops_continue=0 panic_action=reboot`。详细结果见当前构建 `device-validation/untrusted-app-tests.json`。

没有主动制造 Oops/panic。本版本的两条实际崩溃分支尚未通过故障注入复测；此前旧版 Ramdump 路径曾成功导出完整 RAM。
