# 构建与移植说明

## Manager 和 Authenticator

在 Android Studio 打开 `QweovoManager/`，准备 JDK 17 或以上、Android SDK Platform 34。从该目录运行：

```sh
./gradlew :app:testDebugUnitTest :app:assembleDebug :authenticator:assembleDebug
```

Windows 使用 `gradlew.bat`。APK 位于各模块的 `build/outputs/apk/`。Manager 的 debug 包名含 `.debug`，用于构建和界面测试。设备端正式 broker 默认识别 `io.qweovo.manager`，需要使用该包名的正式构建。

正式 App 使用构建者自己的 APK 签名设置，保存在本地的 `QweovoManager/signing/`。该目录不进入版本控制。更换 APK 签名后，设备端 broker 需要同步用新 APK 的签名证书 SHA-256 重建。可用 Android SDK 的 `apksigner verify --print-certs` 读取该摘要。

在仓库根目录用 PowerShell 7 构建 broker。指定本机环境变量 `JAVA_HOME` 和 `ANDROID_HOME`，再运行：

```powershell
./tools/build_meow_manager_broker.ps1 -ManagerCertificateSHA256 $managerCertificateDigest
```

产物在 `out/manager-broker/service.jar`。它包含 Manager 共用的 wire 定义，随内核的 preinit runner 嵌入。维护控制接口时同时检查 [manager-protocol](../QweovoManager/manager-protocol/) 和 broker。内核目录里的早期 API 说明含历史版本信息，当前行为以源码及本说明为准。

## 内核

建议使用对应的 AOSP Clang `r416183b`（12.0.5）及该基线所需的 Linux 内核构建工具。内核工作树、工具链和构建目录宜放在 Linux 文件系统中。使用曾用于目标固件的完整配置，并以 `Module.symvers` 和厂商模块为依据核对 ABI。

应用补丁以后，从仓库根目录执行以下示例。各位置可用脚本参数替换。

```sh
python3 tools/build_kernel.py \
  --kernel ../common \
  --clang-bin ../toolchain/clang-r416183b/bin \
  --manager-payload out/manager-broker/service.jar
```

脚本先导出目标 UAPI 头文件再构建 Image 和 modules。产物在 `out/artifacts/`，工作目录下同时保留 `vmlinux` 供故障分析。可选参数见 `--help`。如需在 BootMenu 选择 Root 模式，请按 [一次性 Root 授权](root-authorization.md) 准备公钥与手机导入文件。

SukiSU 的 Android 用户空间可按目标基线从其上游项目取得。可选 TEE 部分使用 [tee-patches/BASE](../tee-patches/BASE) 中的上游提交，应用本仓库中的差异后，用 `tools/build_meow_tee_payload.py` 构建载荷，再通过 `--tee-payload` 传入。其工具依赖上游对应 ARM64 发布包、Android SDK Platform 37.0、Build Tools 37.0.0、JDK 21 和已缓存的 Gradle 依赖。

## 重新打包与启动排障

- 在独立目录用 Magiskboot 解包同一设备、同一固件和槽位的可启动 boot 模板。使用原始 ramdisk 和头部，将 `kernel` 替换成新构建的 `Image`，再重新打包。若模板来自装有 LKM 启动包装的旧内核，先核对其 `init`、`init.real`、`kernelsu.ko` 和 `ksu_config`，避免在内置版本里继续运行旧包装。
- 对重打包的 boot 再次解包，核对 Image、头部、压缩格式和 ramdisk。对 TB320FC 应重点核对 Header v4、厂商模块 CRC、ThinLTO/CFI、原有 vendor_boot，以及被签名覆盖的分区内容。
- 卡在第一屏且自动重启时，先确认槽位及 AVB 配对、厂商模块 ABI、boot ramdisk。后续再按需保留 `vmlinux`、`System.map` 和内核配置供 Ramdump 符号化。显示菜单只说明早期内核和 preinit 已运行。
- 菜单显示但触屏失效时，用实体键启动 Android，然后立刻执行 `adb shell cat /proc/qweovo_boot_diag`。该节点保留本次启动最近 64 条记录，可免 root 读取。核对首帧、触控固件完成、IRQ 计数、首个触点和最后的输入计数。
- 17.8 实机曾在触控固件报告就绪后仍收到 0 个触点。17.9 恢复由显示模块加载 Novatek 依赖的次序，修正等待状态下的重绘，收到有效输入后才取消计时。一次 17.9 实机启动在 8.37 秒固件就绪，8.99 秒收到首个触点，菜单退出前共记录 28 个触点和 0 个读取错误。固件就绪前可以先用实体键。
## 启动状态与源码入口

- [qweovo_bootmenu.c](../bootmenu-source/kernel/qweovo_bootmenu.c) 固定使用 `metadata` 上的设置位置。当前快照保存 Status Spoofer、TEE、Security 开关及菜单超时。持久配置无效时，17.9 源码启用 Status Spoofer 和三项 Security 开关，并将超时恢复为 60 秒。
- 同一文件的一次性启动请求在读取后用全零记录清空，包括格式无效的请求。SukiSU、MeowSU 和 Custom 的菜单选择只对本次启动生效。定制固件上保留记录格式校验、失败回退与接口访问控制。
- [BootMenu runner](../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/) 包含渲染、DRM、输入、模块依赖及触控初始化。移植到其他设备时优先核对 `hardware.h` 和 `input.h` 的面板、触控及显示路径。
- [统一内核补丁](../kernel-patches/0001-android12-5.10.233-meow.patch) 包含当前工作树的安全回移植与策略接入。历史内核说明中提到的旧补丁编号对应先前工作区拆分方式。

## 本次导出核对

- 对固定基线检出应用内核补丁及源码增量，和原工作树逐文件比较。
- BootMenu 的输入回放、硬件加载和渲染测试通过。Linux 本机的可选 su 客户端协议测试通过。
- 两个 App 的 Debug 构建、Manager 的 13 个单元测试以及 broker DEX 打包通过。设备的现有 17.9 实机诊断记录见上文。本次源码导出未重新编译或刷入内核。
