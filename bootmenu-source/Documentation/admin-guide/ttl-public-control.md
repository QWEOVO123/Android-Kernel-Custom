# 免 root 切换 IPv4 转发 TTL

本树启用 `CONFIG_IP_FORWARD_TTL_PUBLIC_CONTROL=y`，提供权限为 `0666` 的
`/proc/ttl_ctl`。任何应用均可直接读写，不需要 root、管理器或 UID 授权。

```sh
cat /proc/ttl_ctl       # 0 为正常递减；1 为保持原值
echo 1 > /proc/ttl_ctl  # 保留转发报文的 TTL
echo 0 > /proc/ttl_ctl  # 恢复正常递减，默认模式
```

应用可直接调用普通文件 API，例如 Android Java：

```java
try (java.io.FileOutputStream output =
        new java.io.FileOutputStream("/proc/ttl_ctl")) {
    output.write("1\n".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
}
```

该文件和 `net.ipv4.ip_forward_ttl_mode` 共用同一设置。只接受 0/1，每次写入最多
32 字节；非法输入不会改变模式。并发修改以实际写入顺序为准，不设独占控制权。

设置按网络命名空间隔离。文件打开时持有当前网络命名空间引用，直到关闭；同一个
文件描述符在 `setns()` 或传递给其他进程后仍作用于最初打开时的命名空间。
普通 Android 应用通常共享系统网络命名空间，因此会共同影响该命名空间的转发模式。

默认仍为正常递减，不改变开机转发状态。普通 IPv4 转发仍丢弃 TTL <= 1 的报文；
IPv6、本机生成报文和绕过这些软件转发钩子的硬件/BPF/厂商加速路径保持原行为。

SELinux 只按该 proc 节点的私有标记放行相关文件操作，不放行原 sysctl 或其他
文件；全局 enforcing 模式保持。若重新使用同一个读描述符，应先 seek 到 0，
或关闭再打开以读取更新后的值。

启动新内核后可执行：

```sh
python3 tools/testing/selftests/net/ip_forward_ttl.py --public
```

测试用 root 创建 veth/网络命名空间，然后以 UID 65534 写公开接口，检查模式切换、
sysctl 同步、命名空间隔离、非法值和转发报文 TTL/校验和。这个 UID 测试不等同于
Android 应用 SELinux 域测试；实际应用访问和真机转发仍需启动后验证。

已完成 ARM64 构建验证：两个公开接口同时开启、同时关闭、仅 TTL 接口开启且
关闭 SYSCTL，三种配置均通过。提取实际控制函数并模拟内核原语，检查了 4,681 组
输入、跨命名空间的已打开描述符、引用回收、分配失败和私有 inode 标记；
ASan/UBSan 检查通过（因容器限制，未启用 LeakSanitizer 进程检查）。设备自测在
当前云端按预期跳过，完整内核链接和真实 Android app/网络报文测试仍未完成。

`0666` 是所要求的公开接口设计，checkpatch 的 `EXPORTED_WORLD_WRITABLE`
提示因此保留；新增文件的维护者提示不影响接口编译。
