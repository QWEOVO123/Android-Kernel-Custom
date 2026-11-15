# UID 优先级锁定接口（API 1 增量）

识别 `/proc/qweovo_info` 的稳定 ID 和 schema 后，检查 `capabilities.priority_uid_lock=true`、`interfaces.priority_locks="/proc/priority_locks"`、`priority_lock_configuration.api_version=1`、`scope="uid"`。旧内核没有该能力时隐藏锁定控件，保留原有单次调整功能。

普通 App 直接读写，无需 root。所有 I/O 放在工作线程。

```text
lock 10302 -5
unlock 10302
```

一次 write 一条完整 ASCII 命令，最多 128 字节（含换行）。写成功后重新打开读取列表，确认规则仍符合预期；其他前端可以并发修改规则。无需依赖写文件描述符的读取位置。

读取 `/proc/priority_locks` 为 TSV：

```text
uid	nice	updated_ns
10302	-5	123456789012345
```

`updated_ns` 是开机时间纳秒字符串，不能转成浮点数。列表来自内核规则表，包含当前没有运行进程的应用；前端通过 PackageManager 按 UID 查询全部关联包名/标签，共享 UID 应显示全部关联应用。UID 对应的应用卸载后规则仍保留至解除或重启；同次开机若 UID 被复用，新应用也会命中，前端可提示并允许解除。

锁定覆盖完整 UID（包含 Android 多用户偏移），其 appId=`uid % 100000` 必须在 10000..19999。系统 UID、隔离 UID 不支持；最多 256 条。nice 范围 -20..19。

同 UID 当前及未来的普通/批处理线程（SCHED_NORMAL=0、SCHED_BATCH=3）统一使用设定 nice，覆盖 `setpriority`/`nice` 和普通调度策略设置入口，也覆盖 fork、线程创建、Zygote 切换到应用 UID。应用退出重开后仍生效，设备重启后清空。更新规则会重新应用到当前线程。

实时/Deadline/Idle 策略、PI 临时提升、CPU 亲和性、cgroup、uclamp 保持原生行为。该功能锁定的是普通调度的静态 nice，并不冻结整个 Android 调度系统。

解除只移除约束，不猜测或恢复各线程之前不同的 nice；系统之后可以重新调整。命令重复解除允许成功，更新现有 UID 不占新名额。错误输入不修改规则；EINVAL=格式/范围错误，E2BIG=指令过长，ENOSPC=规则表已满，EOPNOTSUPP=启动参数禁用。

前端建议默认勾选应用 UID 锁定，将单次进程/线程调整作为取消勾选后的功能；已经锁定时取消勾选并应用仅执行解除。锁定列表独立于运行进程列表，支持修改 nice、解除以及刷新，关闭页面后恢复运行列表自动刷新。
