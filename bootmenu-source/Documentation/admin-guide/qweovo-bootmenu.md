# QWEOVO 横屏开机菜单（源码阶段）

本功能在现有 ARM64 内核启动后、Android SELinux 设置之前，通过 SukiSU 的
preinit runner 显示原生横屏菜单。继续使用本次已经运行的内核和原来的 init；
不使用 kexec，不替换 Qualcomm 固件引导器。每次开机重新选择，不保存菜单选项。

## 菜单与时序

菜单提供三个相互独立的开关：SukiSU、自定义免 root 功能和 Status Spoofer。默认均关闭。
BootMenu9 使用深蓝色双卡片界面，提供 Start、Power off 和 Bootloader。
BootMenu8 的 Recovery 请求在 TB320FC 上表现为普通重启，因此本版移除了该入口。

SukiSU 的内部准备可在内核 initcall 阶段执行，但对外功能从上电起处于 PENDING。
first-stage init 完成后，PID 1 执行 `/system/bin/init selinux_setup` 时，现有 hook
切换到静态 preinit runner。runner 先运行菜单，再根据决定运行或跳过 preinit
模块循环，最后以原 argv/envp 执行真正的 init。菜单在任何 preinit 模块执行之前。

选择只能提交一次。内核通过匿名 FD 把决定权限交给 PID 1；菜单绘图子进程只通过
管道返回结果，不能直接提交内核开关。子进程、普通 App、root shell 和管理器均
不能修改已提交状态。进入 Android 后不存在重新激活本次已关闭功能的公开接口。

菜单选择仍仅对本次开机生效。启用 SukiSU 后，其已有模块系统仍按原流程使用
自己的配置和模块启动计数，这不属于菜单选项持久化。

## Status Spoofer

开启后，`/proc/bootconfig` 把已有的 `androidboot.verifiedbootstate` 一行显示为
`green`。菜单在 Android 第二阶段 init 读取 bootconfig 之前提交该选择，
因此 Android 的 `ro.boot.verifiedbootstate` 会从该视图取得值。
此开关无需 SukiSU 授权，也无需开启 Custom features。
真实刷写锁状态和 vbmeta 摘要继续由 ABL 提供，开机警告与硬件证明由固件独立处理。

## 一次性 SukiSU 授权

启用 Custom features 后，内核才创建 `/proc/qweovo_sukisu_next`。App 通过
`qweovo_info.capabilities.sukisu_signed_boot` 判断签名接口是否可用。读取返回
`challenge=MEOW-SU1:<64 字节十六进制数据>` 和 `armed=0|1`。写入只接受
`MEOW-SU1-SIG:<256 字节十六进制签名>`，可带一个结尾换行。

内核在可信的早期 PID 1 根目录定位已挂载的 ext4 `/metadata`，打开并固定
`/metadata/qweovo-sukisu-next-signed-v1`。质询由 RSA 公钥指纹和内核随机生成的
32 字节随机数构成。签名输入是固定的 `MEOWSU1\0`、32 字节指纹和 32 字节随机数。
公钥指纹是 PKCS#1 DER 的 SHA-256。使用 RSA-2048、SHA-256 和 PKCS#1 v1.5 验签。

PID 1 在绘制菜单前通过决策 FD 读取授权。内核再次验签，写入新质询和清零后的应答，
完成 fsync 后才返回本次允许。每次开机都更新质询。存储不可用、验签失败或等待超过
两秒时按不允许处理。即使已授权，仍需在菜单中手动启用 SukiSU。
没有内嵌公钥的构建无法签发授权。旧版一字节文件不参与签名授权。

这个方案依赖启动镜像可信以及存储不会回滚。恢复此前整块 /metadata 的有效快照，
或启动仍认可旧授权协议的镜像，可以撤销这个边界。授权密钥与 AVB 密钥独立。

## SukiSU 关闭范围

- 阻止 reboot 握手安装通信 FD、UID 切换时自动给管理器安装 FD、su-session FD。
- 已有 FD 的统一 ioctl 分派也检查开关，避免只关闭发现入口。
- 禁止 root profile 提权、init/ADB Root 提权转换、su 的 stat/access/exec 兼容入口。
- 跳过 preinit 模块循环、init.rc 注入、模块 rc 注入、ksud 启动脚本和 boot 事件。
- 未启用时，实际第二阶段 init 到来后停止 SukiSU 的输入和 init.rc 监听。
- 菜单等待期间，音量键不进入 SukiSU 安全模式计数。

功能关闭属于“本次不可用”，不是从 Image 中移除代码，也不是修改管理器 UI 回包。
管理器的实际“未安装/不支持”文字取决于其版本，后续需要实机验证。

## 自定义功能关闭范围

CPU、TTL、进程信息、nice 调整、UID nice 锁定、公开 panic 配置节点拒绝操作。
专用节点的 SELinux 免 root 放行也受开关约束。TTL 转发辅助函数和 UID nice
执行路径检查同一个开关，避免只关 UI 或 proc 写入口。

BootMenu14 仅公开 `/proc/qweovo_manager_api` 版本标记。CPU、TTL、进程、优先级锁、
panic、启动状态和 SukiSU 授权节点归属 UID 5998。内核还会检查访问者处于
`meow_manager` SELinux 域，确保继承描述符与路径别名遵循同一权限。

开启 Custom features 后，init 启动 UID 5998 的服务。服务通过本地 Unix 套接字取得
系统提供的调用 UID，再向 Android PackageManager 核对同一 UID 的包名和当前 APK
签名证书。Manager 0.5.0 使用固定的读写操作，套接字不会传递内核文件描述符。
如果服务或身份核对失败，访问会被拒绝。旧版固件仍可使用原 Manager API。

## TB320FC 显示、触摸和按键

基于本任务内只读 ADB 核查：DRM `/dev/dri/card0`，内置 DSI 面板原生尺寸
1600×2560；没有 `/dev/fb0`。采用 DRM atomic、双 dumb buffer 和 RGBX 字节布局
（DRM XBGR8888），不依赖 SurfaceFlinger。按 connector 的高通 topology 选择
1/2/4 个平面，先 TEST_ONLY 检查；扫描源高度使用完整面板高度。

参考设备树的横屏方案：画面旋转 90°，触摸交换 XY 后翻转逻辑 Y。
实际 Novatek 触摸为 Protocol B，X=0..16000、Y=0..25600、10 个 slot；实现从
evdev 查询轴范围，未固定 event5 编号。过滤 hbtp_vm、笔和非目标按键设备。
触摸抬起时确认点击，滑动、多指和 SYN_DROPPED 取消不完整点击。

音量键切换、电源键短按确认。只有音量键时，短按切换、长按音量键后松开确认。
只有电源键可用时，短按切换、双击确认。进入菜单时已按住的电源键会被忽略到释放。
菜单内定时接入迟到的设备，保留现有描述符和手势状态。Novatek Protocol B 的 slot
保留上次有效坐标，以处理新接触仅上报 tracking ID 或单轴坐标的情况。
按键和触摸退出时释放 EVIOCGRAB，DRM 状态及背光尽力恢复。

参考：https://github.com/polygraphene/android_device_lenovo_TB320FC/tree/android-12.1
当前调试画面采用纯 C 内置 5×7 ASCII 字模、实色矩形和 RGB 色条，没有字体
文件或 Android UI 依赖。旧版 Noto 字模仅作为历史源码保留，当前不编入程序。
以上描述对应已经验证并单独备份的 BootMenu6。BootMenu7 仅恢复现代化绘图层：
深色卡片、中文位图字、圆角开关；字模仍编译进静态 C 程序，没有外部字体文件
或 Android 图形服务依赖。144 Hz 首选模式、驱动加载、固件路径与输入等待均沿用
BootMenu6。对应 source overlay 的 render.h 和实机验证记录区分这两个版本。
首帧沿用 connector 的首选显示模式，不强制切换 60 Hz：该设备连续启动画面
接管阶段不支持首帧 DMS，强制切换会出现提交成功但面板准备失败的灰屏。

## 超时和失败处理

TB320FC 的 DRM/Novatek 驱动原本在 Android early-init 才加载。菜单先从
只读 vendor 的 modules.dep 递归加载原厂 pinctrl-spmi-gpio、pwm-qti-lpg、
i2c-msm-geni、spi-msm-geni、qcom-pon、pm8941-pwrkey、msm_drm 及依赖，跳过已加载模块；不访问 /data
或 KSU 模块目录。I²C 提供背光所在的总线，SPI 提供 Novatek 触摸总线。加载预算
30 秒，另有 PID 1 的监督截止时间。触摸固件临时从 /vendor/firmware 搜索，
进入 Android 前恢复原 firmware_class.path。

TB320FC 的音量上由 gpio-keys 提供。正常系统中音量下和电源键分别由
pmic_resin、pmic_pwrkey 提供，两者的 platform device 由 qcom-pon 创建，
因此菜单先加载 qcom-pon，再加载 pm8941-pwrkey。
字符串参数写入不附加换行，清空时写一个 NUL；kernel param_set_copystring
会保留换行，不能把普通数值 sysfs 的写法用于固件目录。输入发现等待触摸设备
注册；准备预算用尽才退回按键。
加载器传递 /proc/cmdline 中对应的 module.* 参数，保留双引号值并统一模块名的
连字符/下划线；尤其必须传递 msm_drm.dsi_display0 的 bootloader 面板选择。

菜单只使用 /dev/.qweovo-bootmenu-* 临时设备节点，PID 1 在退出/失败后清理。
不得在 SELinux 加载前创建 /dev/dri 或 /dev/input，否则继承的 tmpfs 标签可能
阻止 ueventd 正常冷启动。日志写入 /metadata/qweovo-bootmenu.log（0600，每次
启动覆盖），不保存开关选择。
另将进入 Android 前的内核环形日志保存到
/metadata/qweovo-bootmenu-kernel.log（0600，上限约 2 MiB）。读取 /dev/kmsg 不
清空内核环形缓冲区。异常若发生在日志保存前，仍需 ramdump/其他崩溃捕获。

驱动加载后，显示/输入准备预算 8 秒，菜单 60 秒无操作则以两项关闭继续启动。
交互可重置无操作计时，总菜单预算最多 180 秒；PID 1 有独立监督截止时间。
菜单 fork、绘图、输入、协议或提交失败时，两项保持关闭。
充电启动跳过菜单，以两项关闭继续原流程；没有正常 init 交接的 recovery
启动也不开放功能。关机/重启调用若失败，则记录日志并以两项关闭继续启动。

## 构建状态与待验证事项

用户已授权编译及刷入。BootMenu1 编译通过但没有菜单，Android 显示服务反复
崩溃；BootMenu2 修正预先创建 /dev/dri 的问题，实机 sys.boot_completed=1，
两项功能关闭，但日志证明菜单阶段尚无 DRM 驱动。两次测试后均恢复了 UIDLock1。
BootMenu3 加入原厂硬件驱动加载，但遗漏模块启动参数，实机黑屏且未完成启动，
随后恢复 UIDLock1。BootMenu4 补齐参数传递及内核日志，Android 正常开机；菜单
因缺少背光 I²C 总线驱动，显示组件绑定返回 -517，仍没有显示。随后恢复 UIDLock1。
独立诊断程序在正常系统中临时接管 DRM，实机成功绘制 2560×1600 横屏菜单，
识别 Novatek 触摸并收到 action=0/features=3。此项是绘图/交互测试，不是开机
时机测试。BootMenu5 补齐 I²C/SPI 总线驱动，DRM 和面板绑定完成，但首帧强制
60 Hz 报 DMS not supported on first frame，实机灰屏。BootMenu6 修复首选模式、
固件路径换行与过早输入扫描，并改用基础画面；结果须以 dist 验证记录为准。
BootMenu6 已经实机确认：开机阶段菜单可见，选择两项开启后提交 features=3，
Android sys.boot_completed=1，SELinux Enforcing，root shell 和公开接口读取正常。
本次首帧模式为 144 Hz，Novatek 固件更新成功。未执行实际关机、bootloader、
recovery 按钮，也尚未完成四种开关组合和独立按键回退的全部验证。
BootMenu7 已编译刷入，用户确认开机阶段现代化中文界面和触摸正常。两项开启后
Android 启动完成、root 和公开接口读取正常，SELinux Enforcing。相对 BootMenu6
仅更换绘图 C 代码，14,579 个导出符号 CRC 一致；基础版镜像和源码已单独备份。
BootMenu8 使用 Meow BootMenu 名称、扳手图标和全英文文案，用户已确认开机菜单
与触摸正常。没有加入新的启动配置协议或 BL 状态覆盖；Oops、panic 和 TTL 仍由
Meow Kernel manager 的公开接口控制。App 0.3.0 跟随系统首选语言，中文使用中文，
其他语言使用英文；debug 包实机通过 4 组语言匹配和 71 项免 root 接口检查。

`tests/test_bootmenu_sources.py` 使用 pycparser 执行真实 model.h 的 AST，检查
四种组合、按钮命中、触摸方向；另有门控覆盖和语法检查。它不是 C 类型检查、
链接验证或设备测试。tests/test_bootmenu_hardware.c 在主机执行真实加载器，
模拟 finit_module 和 sysfs，检查依赖顺序、去重、已加载模块、失败、循环依赖、
路径限制和超时；仍不能替代设备上的驱动/固件测试。

已导出目标 UAPI headers，构建 ThinLTO/CFI 内核与静态 ARM64 runner，
BootMenu1/2 的 14,579 个导出符号 CRC 与 UIDLock1 一致。仍需验证菜单
时机是否已有 DRM、Novatek 固件及输入设备，检查
触摸方向和显示裁切、四种组合、SukiSU 管理器识别、模块执行顺序，以及四个按钮。
UIDLock1 仅作恢复镜像，不包含菜单及随后新增的 CVE 修复。
