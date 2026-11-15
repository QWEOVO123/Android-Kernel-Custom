# 一次性 Root 授权

平板运行 Meow Kernel Manager，手机单独安装 Meow Authenticator。平板内核嵌入 RSA 公钥，手机使用自己导入的配对私钥签名。授权在进入 BootMenu 的那次启动消费一次，由你在 **SU** 页手动选择 SukiSU 或 MeowSU。两种模式共用一条授权路径。

这一套密钥只用于菜单 Root 授权。AVB 签名和 Android APK 签名使用各自独立的凭据。

## 1. 在电脑上生成密钥对

在 Linux 或 WSL 安装 Bash 和 OpenSSL 3，从仓库根目录运行。示例把授权材料放在仓库外，目标子目录必须还不存在。建议把它放在 Linux 文件系统上，以便文件权限生效。

```sh
mkdir -p ../meow-private
bash tools/provision_sukisu_signer.sh ../meow-private/root-authorization
```

| 文件 | 用途 |
| --- | --- |
| `sukisu-auth-public.der` | RSA-2048、公钥指数 65537、SPKI DER。构建内核时只传这份公钥。 |
| `sukisu-auth-import.p12` | 供手机导入的加密 PKCS#12，含一条私钥和配套证书。 |
| `import-passphrase.txt` | 随机生成的导入口令。在电脑上本地读取，仅在手机导入时输入。 |

脚本显示用于核对的指纹。指纹算法为 **PKCS#1 DER 公钥的 SHA-256**，和手机显示以及内核报告一致。直接计算 SPKI 文件的 SHA-256 会得到另一个值。请离线备份加密导入包和口令并分开存放。GitHub 仓库只提交源码和说明。

## 2. 把公钥嵌入内核

先按[开发说明](development.md)应用内核补丁并构建 Manager broker。然后从仓库根目录运行：

```sh
python3 tools/build_kernel.py \
  --kernel ../common \
  --clang-bin ../toolchain/clang-r416183b/bin \
  --manager-payload out/manager-broker/service.jar \
  --sukisu-public-key ../meow-private/root-authorization/sukisu-auth-public.der
```

构建脚本把 SPKI 转成内核验签用的 PKCS#1 DER，在内核输出目录生成 `qweovo_sukisu_public_key.h` 并随 Image 编译。构建报告的 `sukisu_authorization_key_sha256` 应和生成脚本输出一致。若省略 `--sukisu-public-key`，BootMenu 的 Root 模式保持锁定。然后重打包 boot，并按设备的 [AVB 流程](avb.md)签名和更新。

## 3. 在手机上导入私钥

1. 从 `QweovoManager/` 执行 `./gradlew :authenticator:assembleDebug`，Windows 用 `gradlew.bat`。将 `authenticator/build/outputs/apk/debug/` 下的 APK 安装到用于授权的独立手机。App 最低为 Android 12（API 31），手机需设置屏幕锁。
2. 临时把 `sukisu-auth-import.p12` 传到手机。打开 **Meow Authenticator → 导入授权密钥**，选中该文件并输入本地生成的口令。
3. 核对手机显示的指纹与构建报告。核对成功后，从手机共享存储删除临时导入文件。验证器只接受受硬件保护、且逐次身份验证由硬件执行的导入结果，支持 TEE 或 StrongBox。后续签名使用 Android Keystore。 [Android Keystore 文档](https://developer.android.com/privacy-and-security/keystore)

如果换手机，导入原来的加密备份即可继续匹配现有内核。如果要换一套密钥，生成新的目录，用新公钥重编、签名并更新内核。已有授权条目的手机 App 会拒绝覆盖导入，先确认备份可恢复，再清除验证器的 App 数据或重装验证器后导入。新内核启用后，旧私钥签出的应答会被拒绝。

## 4. 日常扫码授权

1. 平板启动 Android 后，在 Manager 打开 **授权下次启动选择 Root 模式 → 显示质询二维码**。
2. 手机的 Authenticator 扫描质询，核对用途和指纹，按系统提示用强生物识别或手机锁屏凭据完成签名。
3. 平板 Manager 点 **扫描签名应答** 扫回手机显示的二维码。Manager 通过经过身份验证的 broker 把应答提交给内核，然后尝试把下次启动目标设为 Menu。重启前在 **启动设置** 确认目标为进入 Menu。
4. 重启到 BootMenu，在 **SU** 页选 **SukiSU** 或 **MeowSU**，再按 **Start Android**。进入菜单时内核就会消费应答，即使最后选择 Root off，也需要再签一次才能开启另一次启动。

内核在提交时验签，并在菜单启动时重新校验保存的应答，轮换随机质询并清空旧应答。验证使用 RSA-2048、SHA-256 和 PKCS#1 v1.5。Root 模式本身不会持久化。移植时要确保早期 init 已能访问独立挂载的 ext4 `/metadata`，否则没有可用授权。完整恢复旧的 metadata 快照可能恢复有效的旧质询和应答，因此此机制依赖受保护的启动与存储。

源码入口见 [内核验签](../bootmenu-source/kernel/qweovo_sukisu_grant.h)、[启动和存储逻辑](../bootmenu-source/kernel/qweovo_bootmenu.c)、[共享协议](../QweovoManager/shared/)和[手机 Keystore 导入](../QweovoManager/authenticator/src/main/java/io/qweovo/authenticator/AuthorizationKey.java)。