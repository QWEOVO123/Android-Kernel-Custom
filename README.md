# Meow · Android Kernel DEV

Meow BootMenu 17.9、Meow Kernel Manager 0.9.0 和 Meow Authenticator 1.2 的移植源码。设备基线为 Lenovo TB320FC（Y700）、ARM64、Android 12 GKI 5.10.233。实机使用 ZUI 17.0.475、Android 15。Android 12 指内核分支。

BootMenu 在现有内核启动后、Android 第二阶段 init 之前运行。它直接通过 DRM 绘制，并从 Linux input 设备读取触屏和实体键。默认直接启动 Android，自定义功能默认开启。Manager 可以安排下次进入 BootMenu 或 Bootloader，设置只消费一次。菜单超时为 5–120 秒，默认 60 秒，收到有效输入后停止自动启动倒计时。

SukiSU 与 MeowSU 通过手机验证器授权单次启动，在菜单里互斥。安全页提供 SELinux 锁、内核模块锁和审计。Manager 提供启动设置、审计、CPU、进程优先级及 UID 锁、转发 TTL 和 panic/Oops 配置。内核和 App 之间的控制请求通过核对包名、UID 和 APK 签名的 Manager broker 处理。

## 目录

| 路径 | 内容 |
| --- | --- |
| [kernel-patches/](kernel-patches/) | Android Common 基线提交、跟踪文件的统一补丁及文件清单 |
| [bootmenu-source/](bootmenu-source/) | 新增内核文件、SukiSU 内核源码及 UAPI、BootMenu 和内核接口说明 |
| [config/](config/) | TB320FC 的 17.9 内核配置，ThinLTO / CFI，KPM 关闭 |
| [QweovoManager/](QweovoManager/) | Manager、Authenticator、共享协议和测试模块，含 Gradle Wrapper |
| [tools/](tools/) | 应用内核补丁、构建 Image、构建 Manager broker 及可选载荷的脚本 |
| [tee-patches/](tee-patches/) | 针对上游 TEESimulator-RS 的可选源码差异 |
| [tests/](tests/) | 主机端渲染、硬件初始化、输入回放及诊断源码 |
| [licenses/](licenses/) | Linux 和 TEESimulator-RS 许可；SukiSU 与字体许可随源码保留 |

## 开发与移植

1. 用 Linux 或 WSL 准备干净的 Android Common 内核，检出 [kernel-patches/BASE](kernel-patches/BASE) 中的提交。
2. 从本仓库根目录执行以下命令。两个目录应处于同一父目录。脚本先确认基线、工作树和新增文件的目标路径，再应用补丁及重建符号链接。

       python3 tools/apply_kernel_overlay.py ../common --check
       python3 tools/apply_kernel_overlay.py ../common

3. 打开 [QweovoManager/](QweovoManager/) 构建 App，或按 [开发与排障](docs/development.md) 构建 Manager broker、内核和启动镜像。
4. 如需通过手机授权 SukiSU 或 MeowSU，按 [一次性 Root 授权](docs/root-authorization.md)生成密钥、编入公钥并配对验证器。
5. 如果设备使用锁定的自定义 AVB 启动链，参照 [AVB 签名与 Bootloader 导入](docs/avb.md)。

此仓库是源码增量。构建所需的设备镜像、固件、二进制载荷、签名及授权材料均由构建者在本地准备。启动链、厂商模块和显示、触控驱动依赖目标设备及固件版本。

## 来源

Linux 内核增量基于 [Android Common Kernel](https://android.googlesource.com/kernel/common/+/5c3dd95c7d4d75e175133fca33a307e00da335da)。SukiSU 内核部分保留 [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra) 的许可及源码声明。BootMenu 字形来自 Noto Sans SC，许可见 [FONT-LICENSE.txt](bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/FONT-LICENSE.txt)。可选 TEE 适配基于 [TEESimulator-RS](https://github.com/Enginex0/TEESimulator-RS/tree/6d241e56d6e8146cd67ed4ac3dadc9c632969549)。
## 许可证

内核改动沿用 GPL-2.0-only，原创 Manager 和 Authenticator 采用 Apache-2.0。第三方组件及协议适用范围见 [许可证](LICENSE.md)。
