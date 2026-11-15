# SukiSU 内建 KPM 支持

本树的 ARM64 `gki_defconfig` 已启用：

```text
CONFIG_KSU=y
CONFIG_KSU_MANUAL_SU=y
CONFIG_KPM=y
CONFIG_KALLSYMS=y
CONFIG_KALLSYMS_ALL=y
```

`KALLSYMS` 依赖通过 Kconfig 选中；`KALLSYMS_ALL` 在原 defconfig 中也已启用。
KSU/KPM 接口编入内核，不需要加载 `kernelsu.ko`。

## 运行时与镜像步骤

当前 SukiSU 通过 ABI v2 桥接调用 KernelPatch 的 KPM 加载器。仅编译
`CONFIG_KPM=y` 不会自动将该加载器加入 Linux Image；还必须在编译完成后对
Image 应用 KernelPatch。这与 SukiSU 管理器中的 KPM 镜像补丁步骤对应。

本树加入了官方 `SukiSU-Ultra/SukiSU_KernelPatch_patch` 源码，位于 `KPM/`。
配套版本：

- SukiSU：`29f8b18d08dae3be66c0c3cd364536827df0a378`。
- KernelPatch：`1c5b608916357ac86a94cc736e235db1fac4a29d`，版本 `0.14.0`。
- 三个协议标识、ABI 版本、PROBE 操作及输出容量均已核对一致。

在内核源码根目录运行：

```bash
# 需要宿主 C 编译器、静态 zlib 开发库、Python 3、ARM64 GCC/binutils。
# GCC 可通过 KPM_CC 指定；工具前缀可通过 KPM_CROSS_COMPILE 指定。
scripts/sukisu-kpm.sh build

# 完成正常内核编译后，将原始 ARM64 Image 注入为一个新文件。
scripts/sukisu-kpm.sh patch out/arch/arm64/boot/Image out/arch/arm64/boot/Image.kpm
```

后续打包应使用 **Image.kpm**。如果设备要求 `Image.gz` 或 `Image.lz4`，
应在注入后再压缩该文件。脚本只接受未压缩的 ARM64 Image，不接受 boot.img，
不执行刷机，也不覆盖原始 Image 或已有输出。

脚本在处理前检查双方协议，并在注入后用 kptools 检查 `patched=true`。
常规 `make Image` 不会隐式运行此脚本；镜像打包流程必须包含上述注入步骤。

## 已完成的验证

- 生成后的 ARM64 GKI `.config` 确认 KSU、KPM、KALLSYMS、KALLSYMS_ALL 均为 `y`。
- Clang/LLVM 14.0.6 下整个 `drivers/kernelsu/` 内建编译通过，归档包含三个
  KPM 对象文件；原有 Linux LTO/CFI 设置保留。
- 使用 ARM64 GCC 13.3.0 从源码编译并链接 `kpimg` 成功。
- 从源码构建宿主 `kptools` 成功，并能读取该 kpimg 的 ARM64 / 0.14.0 元数据。
- 脚本语法、错误输入和已有输出保护检查通过。

当前尚未构建完整 Linux Image，因此未完成真实内核镜像的注入、设备启动及
`.kpm` 加载/卸载测试。上述编译结果不等同于运行时兼容性确认。

官方源码说明：
<https://github.com/SukiSU-Ultra/SukiSU_KernelPatch_patch/tree/1c5b608916357ac86a94cc736e235db1fac4a29d/kernel/patch/sukisu>
