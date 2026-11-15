# AVB 签名与 Bootloader 导入

以下步骤适用于已经确认支持自定义 AVB 信任根的固件。示例使用本地相对位置。TB320FC 的导入曾在目标固件实机验证，其他固件需先确认支持情况。

## 生成一对签名镜像

1. 从目标设备和目标槽位取得匹配的 `boot`、顶层 `vbmeta` 和关联分区作为本地参考。用 `avbtool info_image` 记录分区描述符、flags、回滚索引及其位置。先准备同槽位的可启动回退镜像。
2. 重新打包 boot，只替换经过核对的内核组件。使用与原分区匹配的大小和无槽位后缀的描述符名称 `boot` 生成 AVB hash footer。该版本的顶层 `vbmeta` 直接持有 boot hash 描述符，boot footer 可用 `NONE` 算法生成，最终由顶层签名覆盖。
3. 只替换顶层 `vbmeta` 中的 boot hash 描述符，保留其他 hash、hashtree 和 chain 描述符、回滚字段及原厂验证 flags。用现有信任配置对应的本地凭据签署顶层 `vbmeta`，本设备使用 `SHA256_RSA4096`。用 `avbtool` 和对应链式分区离线核对签名、boot 哈希和 AVB 摘要。单独记录两个镜像的 SHA-256，写入同一槽位后分别读回核对。

## 首次导入

1. 首次安装前备份资料。Bootloader 解锁或重新上锁可能触发恢复出厂设置。已导入相同信任根的设备直接走下面的日常更新。
2. 按固件支持的方式在 Bootloader Fastboot 下进入允许导入的状态。在本地用 AVB 工具从签名身份生成 Bootloader 导入数据，以设备实际支持的分区名称导入。目标固件使用 `fastboot flash avb_custom_key ./local/avb-enrollment.bin`。
3. 安装同一槽位、同次签名的 `boot` 与 `vbmeta`，并检查其余链式分区与签名参考一致。先验证可启动性，再执行 `fastboot flashing lock` 并在设备上确认。
4. 锁定后检查设备启动时的自定义系统黄色提示，以及 Android 可用后报告的锁定状态和预期 AVB 摘要。检查颜色时关闭 BootMenu 的 Status Spoofer，现场状态以 Bootloader 启动提示为准。

## 已锁定设备的后续更新

沿用已导入的签名身份，始终成对更新目标槽位的 `boot` 和 `vbmeta`。TB320FC 上可从已获授权的 Android root 会话写入，或用设备匹配的 Qualcomm EDL 工具按分区写入。写入前核对槽位、分区长度和文件 SHA-256，写入后读回两项逐一验证。避免为日常更新解锁 Fastboot，以免清除数据。故障回退同样写回一对之前验证可启动的镜像。

[AOSP Android Verified Boot 文档](https://android.googlesource.com/platform/external/avb/+/master/README.md)