# GhostLock / DirtyFrag 修复记录

日期：2026-10-28。基线：`android12-5.10.233_r00`，提交
`5c3dd95c7d4d75e175133fca33a307e00da335da`。

## 修复范围与来源

| 漏洞 | 修复 | 上游 / stable 提交 |
| --- | --- | --- |
| GhostLock，CVE-2026-43499 | `remove_waiter()` 使用 waiter 自己的 task、pi_lock 和 pi_blocked_on，而非 current | 上游 `3bfdc63936dd4773109b7b8c280c0f3b5ae7d349`；5.10 回移植 `f3fa3424bceb128d2be4b3745506b22844b87db7` |
| 配套修正，CVE-2026-53163 | 跳过从未入队的 waiter；仅在 proxy lock 返回负错误码时移除 waiter | 上游 `40a25d59e85b3c8709ac2424d44f65610467871e`；5.10 回移植 `bfbc047ceb42c0e1fac8f3a155d4548a8bbe76b1` |
| DirtyFrag ESP，CVE-2026-43284 | IPv4 sendpage 标记共享页；IPv4/IPv6 ESP 输入对共享页执行 COW | 上游 `f4c50a4034e62ab75f1d5cdd191dd5f9c77fdff4`；5.10 回移植 `a6cb440f274a22456ef3e86b457344f1678f38f9` |
| DirtyFrag RxRPC，CVE-2026-43500 | 受保护的 DATA 包如果被 clone、包含 frag_list 或共享页，则复制为私有线性 skb 后解密 | 根据上游 `aa54b1d27fe0c2b78e664a34fd0fdf7cd1960d71` 的条件适配到本树 `net/rxrpc/input.c` |
| 相关后续修正 Fragnesia，CVE-2026-46300 | skb 合并并转移页片段时保留 `SKBTX_SHARED_FRAG` | 上游 `f84eca5817390257cef78013d0112481c503b4a3`；5.10 回移植 `3599e6b3cc1ada96883d496a50a210d3afbb6987` |

四份 5.10 stable 补丁直接应用；相关五个文件已与在原始基线上依次应用这些补丁的结果逐字节核对。

本树使用旧版 `SKBTX_SHARED_FRAG` / `tx_flags`，不能直接照搬新版内核的
`SKBFL_SHARED_FRAG` / `flags`。5.10 的 UDP 页发送入口也与新版不同，因此使用了
stable 已适配的 `ip_append_page()` 修复。

RxRPC 的 DATA 解密入口位于 `input.c`，并非新版的 `call_event.c`。
适配使用 `skb_copy()`，保留内存不足时丢包、释放原 skb、刷新 skb 私有数据指针和
RxRPC 引用计数跟踪。此版本的 RXKAD RESPONSE 校验先将 response 和 ticket
复制到独立分配的内存，再解密这些副本，因此无需照搬新版 RESPONSE 入口补丁。

部分旧通告列出 CVE-2026-53166；截至核查时，该编号在 Linux CNA 的 CVE 记录中
已标记为 REJECTED。本次采用上述有效的 stable 配套修正，不单独声称修复该撤销编号。

## 验证结果与边界

- `git diff --check` 通过。
- ARM64 GKI 配置保留 `CONFIG_KSU=y`、原有 LTO/CFI 配置及 TTL 修改。
- Clang/LLVM 14.0.6 编译通过：`kernel/locking/rtmutex.o`、`net/core/skbuff.o`、
  `net/ipv4/esp4.o`、`net/ipv4/ip_output.o`、`net/ipv6/esp6.o`。
- GKI 默认关闭 `CONFIG_AF_RXRPC`。在单独的验证输出目录启用
  `CONFIG_AF_RXRPC=y`、`CONFIG_AF_RXRPC_IPV6=y`、`CONFIG_RXKAD=y` 后，
  整个 `net/rxrpc/` 编译通过并生成 `built-in.a`；源 defconfig 未因此改变。
- 已生成独立补丁 `patches/security/ghostlock-dirtyfrag.patch`，仅含本次安全修复，
  不含先前 TTL/SukiSU 改动。
- 尚未完成完整内核链接、设备启动、KASAN/并发回归或运行时漏洞回归测试。
  编译和源码核对不能替代这些检查，也不表示 5.10.233 的其他漏洞均已修复。

## 原始记录

- Linux CNA CVE 记录：
  <https://github.com/CVEProject/cvelistV5/tree/main/cves/2026>
- GhostLock stable 修复：
  <https://git.kernel.org/stable/c/f3fa3424bceb128d2be4b3745506b22844b87db7>
- GhostLock 配套修正：
  <https://git.kernel.org/stable/c/bfbc047ceb42c0e1fac8f3a155d4548a8bbe76b1>
- DirtyFrag ESP stable 修复：
  <https://git.kernel.org/stable/c/a6cb440f274a22456ef3e86b457344f1678f38f9>
- DirtyFrag RxRPC 上游修复：
  <https://git.kernel.org/stable/c/aa54b1d27fe0c2b78e664a34fd0fdf7cd1960d71>
- Fragnesia stable 修复：
  <https://git.kernel.org/stable/c/3599e6b3cc1ada96883d496a50a210d3afbb6987>
