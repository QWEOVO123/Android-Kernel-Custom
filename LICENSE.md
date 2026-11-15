# 许可证

此仓库包含多项采用不同许可证的组件。以下许可声明适用于 QWEOVO 有权授权的原创内容。第三方代码、资源和回移补丁遵循其原有许可及文件声明。

| 路径或组件 | 许可 |
| --- | --- |
| 内核补丁、内核侧原创代码及 BootMenu | [GPL-2.0-only](licenses/Linux-GPL-2.0.txt)。内核 UAPI 头文件按文件声明适用 [Linux-syscall-note](licenses/Linux-syscall-note.txt)。 |
| 原创 Meow Kernel Manager、Meow Authenticator、测试 App、共享 App 与通信协议代码、Manager broker | [Apache-2.0](licenses/Apache-2.0.txt)。Gradle Wrapper 和 Android 依赖各自保留原许可。 |
| Root 授权密钥生成脚本 | [Apache-2.0](licenses/Apache-2.0.txt)。 |
| SukiSU 树 | 见 [SukiSU 根目录 LICENSE](bootmenu-source/KernelSU/LICENSE)、[SukiSU 内核目录 LICENSE](bootmenu-source/KernelSU/kernel/LICENSE) 及各文件 SPDX 声明。nolibc 保留原文件的许可选择。 |
| TEESimulator-RS 改动 | 保留 [TEESimulator-RS 的 GPLv3 许可及上游声明](licenses/TEESimulator-RS.txt)。 |
| Noto Sans SC 字形 | 见 [SIL Open Font License 1.1](bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/FONT-LICENSE.txt)。 |

分发内核镜像时，应按适用的 GPL 条款提供对应源码。版本发布时可关联至用于构建该镜像的源码标签、配置与构建脚本。