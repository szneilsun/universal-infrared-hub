# ESP32 HomeKit 红外 Hub

基于 ESP32-S3、HomeSpan 和 Arduino-IRremote，将使用 `KKZM-4WB` 遥控器的
美菱空调和 Pioneer 机顶盒接入 Apple 家庭 App。当前版本为 `2.01`，并支持
通过网页学习通用红外设备、动态创建 HomeKit 子配件。

## 项目结构

```text
universal_ir_remote/
├── universal_ir_remote.ino  # 主程序、HomeKit 初始化、红外学习
├── air_conditioner.ino      # 美菱空调协议与 HomeKit 服务
├── television.ino           # Pioneer 电视/机顶盒控制
├── dynamic_devices.ino      # 网页设备管理、学习码与动态 HomeKit 配件
├── dynamic_devices.h        # 动态设备的数据模型与常量
├── home_background.h        # 内嵌的网页居家背景资源
├── assets/                  # 背景图源文件
├── docs/                    # 协议分析文档
├── CHANGELOG.md             # 版本变更记录
└── README.md                # 使用与维护说明
```

## 已实现功能

- 空调开机与关机
- 制冷、制热模式切换
- 16.0～32.0℃温度调节，步进 0.1℃
- 六档风速：自动、1档、2档、3档、4档、最强
- 关屏/开屏切换
- 通过串口学习和重放其他常见红外指令
- 红外学习数据保存到 ESP32 非易失存储，断电后保留
- 网页新建设备、命名、选择类型并逐键学习
- 学习完成后为普通开关、风扇或电视创建独立的 Home App 设备
- 自建设备可在网页删除；支持开关、风扇、电视和通用学习型空调

遥控器没有独立的自动运行模式，因此 HomeKit 仅声明制冷和制热。如果家庭
App 仍显示“自动”，通常是旧配件元数据缓存，需要关闭并重新打开家庭 App，
必要时删除配件后重新添加。

Apple Home 没有空调显示屏这一标准属性，因此关屏命令使用空调页面内的
“摆风”开关。它实际控制显示屏，不控制导风板。实体遥控器的关屏键是无状态
切换命令；如果同时使用实体遥控器，App 中显示的开关状态可能与空调不同步。

“当前温度”显示最后一次发送的设定温度，并非真实室温。若要显示真实温度，
需要另接温度传感器。

## 硬件连接

- GPIO 41：红外接收模块信号
- GPIO 40：红外发射驱动输入
- 串口波特率：115200

红外 LED 不应由 GPIO 直接驱动。建议使用 S8050、2N2222 或逻辑级 MOSFET：

```text
GPIO 40 ── 1kΩ ── NPN 基极
ESP32 GND ──────── NPN 发射极
5V ── 47～100Ω ── 红外 LED 正极
红外 LED 负极 ─── NPN 集电极
```

ESP32 与发射电源必须共地。推荐使用 940 nm 红外 LED，并在 5V 与 GND 之间
增加 `100µF + 0.1µF` 去耦电容。

## 软件依赖与编译

- Arduino ESP32 core 3.3.11
- HomeSpan 2.1.8
- Arduino-IRremote 4.7.1
- 开发板：ESP32S3 Dev Module
- 分区：Huge APP（3 MB No OTA / 1 MB SPIFFS）
- 上传速度：115200

固件入口：[universal_ir_remote.ino](./universal_ir_remote.ino)

代码按功能分为三个 Arduino 标签页：

- `universal_ir_remote.ino`：Hub 初始化、红外学习和串口菜单
- `air_conditioner.ino`：美菱空调协议与 HomeKit 服务
- `television.ino`：Pioneer 电视/机顶盒协议与 HomeKit 服务
- `dynamic_devices.ino`：网页学习设备、NVS 存储和动态 HomeKit 配件

## HomeKit 配置

首次启动时：

1. 连接热点 `IR-AC-Setup`。
2. 热点密码为 `iracsetup`。
3. 完成家庭 Wi-Fi 配置。
4. 在 Apple 家庭 App 中添加 `IR Air Conditioner`。
5. 使用配对码 `111-22-333`。

串口菜单选项 `4` 可清除 HomeKit 配对和 Wi-Fi 配置，但保留已学习的红外码。

## 串口菜单

```text
1                   # 进入学习模式
1                   # 选择存储槽 1，然后按遥控器按键
0                   # 返回主菜单
2                   # 进入发送模式
1                   # 发送存储槽 1
3                   # 显示固件和硬件版本
4                   # 清除 HomeKit 与 Wi-Fi 配置
5                   # 开关完整红外时序输出
6                   # 切换机顶盒载波频率（40/56/36/38 kHz）
```

通用 Pulse-Distance/Pulse-Width 信号默认只输出紧凑重放数据。如需逐脉冲完整
时序，在主菜单输入 `5` 即可开启或关闭，无需重新编译。
`IR_CAPTURE_VERBOSE` 用于设置每次启动后的默认状态。

## 网页学习与动态设备（2.01）

Hub 接入家庭 Wi-Fi 后，在同一局域网浏览器打开：

```text
http://ir-hub.local:8080
```

这是固定的 Bonjour/mDNS 地址，适用于 iPhone、iPad 和 Mac；即使路由器重新分配
DHCP IP 也无需修改。串口连接成功日志和网页顶部还会同时显示实际 IPv4 地址，可用
`http://<Hub 的 IP>:8080` 访问。页面顶部的“内置设备”区统一管理既有的美菱空调
和 Pioneer 机顶盒：可修改 HomeKit 名称、启用/隐藏设备，空调可网页测试开关和
显示屏命令，机顶盒可测试电源/音量并设置原始时序载波频率。保存内置设备配置后
Hub 会自动重启并刷新 HomeKit 配件列表。

网页可创建最多 8 个自定义设备，支持：

- **普通开关**：学习一个“电源 / 开关”按键；
- **风扇**：学习电源、风速增加和风速降低；
- **电视**：学习电源、音量、频道和方向/确认按键。
- **空调**：学习电源、温度增加/降低、模式切换和风速增加/降低；在 Home App
  中显示为标准空调控制器。

创建后逐键点击“学习”，将实体遥控器在接收头约 20 cm 内按一次对应键。点击
“完成并发布此设备”时，配置和红外码会写入 NVS，Hub 自动重启并更新 HomeKit
桥接器配置。重启后，新设备会出现在 Apple 家庭 App；如果 Home App 保留旧配件
列表，关闭并重新打开 App，仍未出现时删除并重新添加“红外 Hub”。

红外遥控器通常没有状态回传。对于使用“电源切换”码的设备，App 显示状态可能
和实体设备不一致；这是红外单向控制本身的限制。

每个自建设备卡片底部均提供“删除此设备”。确认后会一并清除该设备的所有学习码，
重启 Hub 并从 HomeKit 桥接器中移除对应配件；内置空调和机顶盒不受此操作影响。

网页底部的“系统维护”提供三项独立操作：重置网络（仅清除 Wi-Fi）、重置
HomeKit（仅清除配对）和删除所有学习设备（清除所有自建设备及其红外码）。
每项操作均需确认并会重启 Hub。

网页顶部会同时显示固定管理地址和当前 DHCP IPv4 地址。固定地址推荐收藏到
Safari：`http://ir-hub.local:8080`。

## 已识别协议

- 载波：38 kHz
- 数据长度：120 bit
- 帧头：约 8450 µs / 4200 µs
- 逻辑 0：约 550 µs / 550 µs
- 逻辑 1：约 550 µs / 1600 µs
- 位序：LSB first
- 校验：前 14 字节所有十六进制半字节之和

详细的字段、温度、模式和风速采集结果见
[MEILING_KKZM_4WB_PROTOCOL.md](./docs/MEILING_KKZM_4WB_PROTOCOL.md)。

Pioneer 命令及机顶盒波形记录见
[PIONEER_TV_IR_PROTOCOL.md](./docs/PIONEER_TV_IR_PROTOCOL.md)。

## 已知限制

- 空调没有状态回传，实体遥控器操作不会自动同步到家庭 App。
- 家庭 App 的控件布局和大小由 Apple 决定，固件无法自定义。
- “摆风”目前被用作关屏/开屏控制，尚未接入真实上下扫风。
- 原始未知协议可以输出，但尚不能保存到学习槽或网页设备。网页学习目前仅支持
  Arduino-IRremote 能解析并重放的协议。
