# 任意应用控制 CPU 核心

此定制内核启用 `CONFIG_CPU_PUBLIC_CONTROL=y`，通过 `/proc/cpu_ctl` 提供接口。
节点由内核创建，权限为 `0666`；不需要 root、UID 授权、管理器或独占控制权。

```sh
cat /proc/cpu_ctl
echo 'offline 3' > /proc/cpu_ctl
echo 'online 3' > /proc/cpu_ctl
echo 'set 0-3,6' > /proc/cpu_ctl
```

应用可以直接使用普通文件 API，例如 Android Java：

```java
try (java.io.FileOutputStream output =
        new java.io.FileOutputStream("/proc/cpu_ctl")) {
    output.write("offline 3\n".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
}
```

每次 `write()` 提交一条完整指令。多个应用之间没有优先权，按热插拔锁串行执行。
批量设置先上线新核心，再下线其他核心；仍由内核保护至少一个活跃核心。
固件禁止关闭的核心不能被强行关闭。批量操作失败可能已有部分步骤完成，应重新读取
接口查看实际状态。该接口不锁定厂商调度/温控策略，也不改变原本的开机核心状态。

CPU 控制的访问处理仅匹配该驱动的私有 proc inode 标记；没有关闭全局 SELinux，
也没有通用地放开普通 `/proc`、`sysfs`、设备节点或 UID 的权限。另行启用的
`/proc/ttl_ctl` 使用独立标记。普通 proc 挂载上的文件
读写已在相关 SELinux hook 中适配，包括 FileOutputStream 的 `O_TRUNC` 打开方式。

接口协议见 `Documentation/ABI/testing/procfs-cpu-public-control`。
非破坏性 smoke test 为：

```sh
python3 tools/testing/selftests/cpu-hotplug/cpu-public-control.py
```

测试以非 root UID 执行查询、无状态变化的上线/掩码指令和错误输入检查。
它不主动关闭核心；最后活跃核心保护及真实 Android app SELinux 域仍需在启动后的
设备上实测。当前云端运行的不是这份定制内核，不能在这里验证实际硬件热插拔。

已完成的静态/构建验证：

- ARM64 GKI 的驱动对象与 SELinux hooks 编译通过。
- 关闭 `CONFIG_CPU_PUBLIC_CONTROL` 后，SELinux hooks 编译通过，且不再引用驱动识别函数。
- 提取实际驱动函数并模拟 CPU 热插拔，检查了八核模型中全部 65,025 种非空列表之间
  的切换、先上线后下线顺序、最后核心拒绝、保护核心、错误输入与部分失败处理。
  AddressSanitizer/UndefinedBehaviorSanitizer 检查通过；容器不支持该测试的
  LeakSanitizer 进程检查，因此未启用泄漏检测。
- 原有 `kernel/cpu.c` 与基线一致，最后活跃核心保护仍使用原实现。
- 设备 smoke test 在当前云端按预期跳过，因为宿主未运行此定制内核。

`checkpatch` 对 `0666` 报出 `EXPORTED_WORLD_WRITABLE`；该公开权限是本接口的
明确设计，保留原值。新增文件还会提示补充维护者信息。完整内核链接和真机运行未验证。
