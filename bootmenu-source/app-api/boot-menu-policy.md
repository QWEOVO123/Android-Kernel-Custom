# 开机菜单状态（API 1 增量）

保持原来的 `kernel_id=qweovo.secret_kernel`、`schema_version=1`。
若 `/proc/qweovo_info` 包含 `boot_policy`，可读取：

```json
{"menu":true,"committed":true,"sukisu":false,"custom":true,"status_spoofer":false,"persistence":"none","mutable":false}
```

这表示菜单已选择：本次 SukiSU 和 Status Spoofer 关闭，自定义免 root 功能开启。
没有新字段的旧内核按旧协议处理，不猜测菜单状态。

`capabilities` 表示本次可用能力。custom=false 时，CPU/TTL/进程/nice/UID 锁定/
公开 panic 控制均为 false，前端隐藏或禁用相应操作。内核信息仍可读取。
这些是只读状态，不提供 App 修改本次选择或保存下次选择的命令。

也可读取 `/proc/qweovo_boot_status` 的 key=value 文本。菜单选项每次重新选择，
不保存在磁盘。SukiSU 与自定义接口相互独立，SukiSU 关闭不妨碍开启的自定义接口。
