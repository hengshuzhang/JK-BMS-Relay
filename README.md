# ESP32-S3-SuperMini Arduino BLE 透明 BMS 中继

本工程是 BLE 原始数据透明中继，PlatformIO 构建入口为 `src/`。中继结构以 GitHub 项目 `wenwumumu/JK-BMS-4G` 的 Arduino/NimBLE relay 为基础重建。

## 透明中继行为

- 一个 NimBLE Central 按用户配置的 MAC 连接真实 BMS；
- 真实 BMS 侧优先发现 `FFE0/FFE1`，并保留 `E07F/FFE1` 服务回退；
- ESP32 不识别 BMS 品牌，不解析、不修改、不重组任何 BMS 数据；
- ESP32 不主动生成 BMS 查询帧；客户端通过虚拟 `FFE1` 的 Write/Write Without Response 自己发送查询命令；
- ESP32 不在真实 BMS 连接成功时主动订阅 Notify/Indicate；只有虚拟客户端开启 CCCD 后，才由客户端订阅事件驱动真实 BMS 的上游订阅；最后一个虚拟客户端取消订阅或断开后，ESP32 会移除上游订阅；
- 真实 BMS 的 Notify/Indicate payload 按接收分片原样转发给所有已订阅的虚拟客户端；
- 未开启虚拟 CCCD 的客户端不会收到自动 Notify，但可以通过 GATT Read 获取最近一次缓存的原始数据；
- 虚拟客户端的 Write 原样转发到真实 BMS，不区分或改写 BMS 协议；
- 真实 BMS 断线后只执行连接恢复和服务发现，不自动发送查询，也不会因为定时器自动订阅；如果虚拟客户端仍保持订阅，重连后会按照该客户端仍然存在的订阅请求恢复上游订阅；
- 默认使用两个 NimBLE Legacy 广播实例，名称为 `JXL50111C1` 和 `JXL50111C2`；
- 可通过 Web 页面编辑两个固定虚拟信号的名称；
- 每个虚拟信号名称最多12字节，以适配 Legacy BLE 广播包；
- 两个虚拟信号共享同一个 `FFE0/FFE1` GATT 服务；
- SoftAP-only：`CAR` / `147258369` / `192.168.4.1`；
- Preferences 保存真实 BMS MAC 和虚拟广播配置；
- 保留 NimBLE bonding/security 配置，不混用 Bluedroid；
- Wi-Fi SoftAP 发射功率固定为7 dBm，BLE所有角色发射功率设置为9 dBm；
- 虚拟广播仅在真实 BMS 已连接且完成 `FFE0/E07F` 的 `FFE1` 服务发现后开启；BMS 未配置、扫描中、连接中或断开时，两个虚拟广播均保持关闭。连接成功后会先等待 500ms，再以至少 250ms 间隔逐个启动两个广告实例，避免与 BMS Central 建连的 GAP/GATT 操作重叠；
- 虚拟客户端连接某一虚拟信号后，中继会在主循环确认停止**该信号**的广告实例，因此它不再出现在 BLE 扫描或 Scan Response 中；停止广告不会断开已建立的 GATT 连接，另一个未连接虚拟信号继续广播；
- 真实 BMS 断开时，ESP32 会立即停止虚拟广播、取消下游订阅请求并强制断开全部已连接手机客户端；BMS 重连并完成服务发现后才重新开启虚拟广播；

当前实现一个真实 BMS 连接、最多两个手机连接，并固定提供两个用途相同的 BLE 虚拟中继广播。两个广播使用不同名称，但共享同一个 BLE 控制器地址和 GATT Server。手机连接任意信号后都使用 `FFE0/FFE1`，发送和接收相同的原始 BMS 数据。

## HTTP

- `GET /`：配置页；
- `GET /status`：Central、上游订阅状态、Relay、真实 BMS 广播许可状态、广告恢复等待状态和两个固定虚拟信号的广播/下游订阅状态；两个信号逐项位于 `signals.items` 数组中；
- `POST /connect`：`{"mac":"xx:xx:xx:xx:xx:xx"}`，只配置真实 BMS MAC；
- `POST /disconnect`：断开真实 BMS；
- `POST /signals`：保存两个固定虚拟信号的名称，例如 `{"names":["JXL50111C1","JXL50111C2"]}`；每个名称最多12个字节；
- `GET /relay/status`：返回与 `/status` 相同的状态信息。

## GATT 数据流

### 下行：虚拟客户端到 ESP32

1. 客户端连接虚拟广播；
2. 客户端根据需要对虚拟 `FFE1` 写入 CCCD `01 00`，开启 Notify；
3. 第一个开启 CCCD 的客户端事件会驱动 ESP32 订阅真实 BMS 的 FFE1 Notify/Indicate；
4. 客户端将 BMS 查询命令原样 Write 到虚拟 `FFE1`；
5. ESP32 将 Write payload 原样写入真实 BMS；
6. 真实 BMS 返回的原始 Notify/Indicate 被转发给所有已开启 CCCD 的虚拟客户端。

如果客户端不使用 Notify，也可以发送 Write 后主动 Read 虚拟 `FFE1`，读取 ESP32 缓存的最近一段 BMS 原始数据。缓存属于所有虚拟客户端共享，不按请求者区分。

### 上行：真实 BMS到 ESP32

ESP32 只负责连接、发现 FFE1、按虚拟客户端的 CCCD 状态建立或移除 Notify/Indicate 订阅，以及转发原始 payload。它不主动发送任何品牌相关查询帧，不解析 BMS 返回值。

## 服务与特征

- 真实 BMS：优先查找 `FFE0/FFE1`，失败后回退 `E07F/FFE1`；
- 虚拟中继：提供 `FFE0/FFE1`；
- 虚拟 `FFE1` 属性：`READ`、`WRITE`、`WRITE_NR`、`NOTIFY`；
- NimBLE 自动管理 Notify 所需 CCCD，不手工创建无功能的 `0x2902` descriptor。

## 使用乐鑫 Flash Download Tool 刷写

PlatformIO 构建完成后，会在 `.pio/build/esp32-s3-fh4r2/` 自动生成 `firmware_merged.bin`。这是从地址 `0x00000000` 开始的完整4 MB原始镜像，可在乐鑫官方 `flash_download_tool` 中只添加这一项并将地址设置为 `0x00000000`。

合并镜像包含以下区域：

| Flash 地址 | 内容 |
| --- | --- |
| `0x00000000` | `bootloader.bin` |
| `0x00008000` | `partitions.bin` |
| `0x0000E000` | Arduino-ESP32 的 `boot_app0.bin` |
| `0x00020000` | `firmware.bin`（`ota_0`） |

量产或首次刷写时可以先执行擦除；升级时不要勾选整片擦除，否则会清除 NVS 中保存的 BMS MAC 和虚拟广播配置。

## 构建验证

本次透明中继与长时健康恢复更新按用户要求未执行 PlatformIO 编译、实际硬件烧录或 Flash 擦除。执行构建后，预期在 `.pio/build/esp32-s3-fh4r2/` 生成 `firmware_merged.bin`。

## 来源与许可证

中继类的结构、回调流程和 CCCD 缓存行为参考：
<https://github.com/wenwumumu/JK-BMS-4G/tree/main/esp32s3-arduino>

该仓库声明为 GPL-2.0。这里保留来源说明；若分发包含其派生代码的固件或源码，应同时遵守 GPL-2.0 的源代码和许可证义务。

## 长时运行健康恢复与诊断

为避免真实 BMS 仍连接、但一次临时 ATT/CCCD 操作失败后上游 Notify 永久停止，本版本在虚拟客户端仍开启 CCCD 且真实 BMS 已连接、但上游订阅未成功时，每 `5` 秒自动重试真实 BMS 的 Notify/Indicate 订阅。重试只操作上游 CCCD，**不会**生成或写入任何 BMS 查询命令；当最后一个虚拟客户端取消订阅或断开时，仍会移除上游订阅。

若同一个虚拟客户端的 `notify(handle)` 连续失败 `3` 次，中继将主动断开该失效连接并清理其订阅状态，避免陈旧连接持续占用转发路径。默认使用 `GET /status` 获取以下排障指标；只有临时启用串口日志构建时，才会每 5 秒额外输出 `[App] health`：

- 上游订阅请求、pending、实际订阅状态及尝试/成功/失败次数；
- 最近一次上游 Notify 的帧数、长度与距今时间；
- 下游 Write/Notify 成功失败计数、缓存帧数与因连续 Notify 失败断开的次数；
- `free_heap` 和 `min_free_heap`，用于验证是否存在持续内存下降。

出现“客户端显示连接但没有数据”时，请保存故障发生前后的 `/status` JSON。若 `upstream_subscription_pending=true`、`upstream_subscribed=false` 且失败计数增加，说明正在自动恢复真实 BMS 的 CCCD；若上游帧数继续增长而下游 Notify 失败数增长，则问题位于虚拟客户端连接；若两侧计数停止且堆内存稳定，则应继续检查真实 BMS 是否还有响应或射频链路是否断开。需要串口诊断时，可临时将 `APP_ENABLE_SERIAL_LOGS` 设为 `1` 后重新构建。

## 串口日志

默认构建通过 `APP_ENABLE_SERIAL_LOGS=0` 编译移除应用的串口日志，并将 `CORE_DEBUG_LEVEL` 设为 `0`。这会消除日志格式化、UART 输出及相关 Core 调试开销；`GET /status`、Web 配置、OTA 和 BLE 中继功能不受影响。若需要临时排障，可将 `platformio.ini` 中的 `-DAPP_ENABLE_SERIAL_LOGS=0` 改为 `-DAPP_ENABLE_SERIAL_LOGS=1` 后重新构建。
