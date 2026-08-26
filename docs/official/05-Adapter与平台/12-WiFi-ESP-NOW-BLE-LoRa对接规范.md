# Wi-Fi、ESP-NOW、BLE、LoRa 对接规范

> 文档级别：`NORMATIVE INTEGRATION CONTRACT`
> 实现状态：公共 Adapter 合同 `CURRENT`；对应 SDK Driver 不在仓库内
> 最近核对：`a093862`，2026-08-25

## 统一接入步骤

1. 产品 Driver 初始化 PHY/MAC、频道、peer、连接或广播参数；
2. 为该物理实例建立独立 Link/Adapter/Source context；
3. 冻结物理地址到 UCN Peer 的映射；
4. 报告真实 logical MTU、base cost 和 liveness profile；
5. RX callback/ISR 把完整 UCN Frame 或 Carrier 数据写入固定 Ring；
6. Event Runtime 通知唯一 Owner；
7. send callback 只负责把编码帧交给 Driver；
8. 将 RSSI/SNR/retry/failure/queue/airtime 归一成通用 metrics。

## 不要双重 Mesh

若使用 ESP-WIFI-MESH/BLE Mesh/Thread 等已有 Mesh，必须明确谁负责多跳。常见选择：

- 底层只提供一跳/peer transport，由 UCN 路由；
- 底层 Mesh 整体作为一个 Tunnel Link，UCN 不再逐跳控制其内部节点。

不要同时让底层 Mesh 和 UCN 对同一节点集合都做独立泛洪/下一跳，否则会重复路由、难以解释 Cost 和故障。

## Metrics

Wi-Fi/ESP-NOW 可报告 RSSI、retry、peer loss、airtime/queue；BLE/LoRa 可报告 RSSI/SNR、duty cycle、PHY rate。指标缺失保持 Invalid，不用固定假值欺骗选路。

## 当前事实

Standard preset 已有这些介质名称和初始 Cost，但源码没有 ESP-IDF/NimBLE/LoRa Radio Driver。实机项目需要独立维护并测试 BSP glue。

## 先决定物理交付单元

不同无线 SDK 的 callback 可能提供 datagram、connection stream、GATT notification 或 radio packet。Adapter 必须明确：

- 一次 callback 是否恰好一个完整 UCN Frame；
- 若会分段/合并，使用何种 Carrier/长度/边界；
- 最大实际 Payload 与安全/SDK Header 后的 logical MTU；
- peer address、广播和 Node ID 如何绑定；
- send 返回 queued、MAC ACK 还是应用 ACK；
- callback buffer 生命周期和复制策略。

不能因为都叫“无线”就共用一份假设。

## Wi-Fi/ESP-NOW

ESP-NOW 常提供 peer MAC/datagram callback，适合“一 datagram 一完整 UCN Frame”或定义明确 Carrier。产品要处理 peer 表容量、频道、加密模式、send callback、Wi-Fi 共存和 RSSI/重试来源。

普通 Wi-Fi UDP 可把 UDP peer 当一跳 Link；TCP 是可靠字节流，应使用长度/COBS Stream 语义且避免与 UCN ACK 重复误解。使用 ESP-WIFI-MESH 时见双重 Mesh 决策。

## BLE

BLE connection/GATT notification 的 MTU 会协商变化，logical MTU 必须取 ATT/L2CAP 与产品协议后的真实可用值。多个 connection 各自是 Bearer；connection handle 不是稳定 Node ID，重连后要重新绑定/准入。

BLE Mesh 若负责多跳，可作为 Tunnel Link；若 UCN 负责逐跳，则底层应暴露一跳 peer transport。不要让两层同时对同一消息泛洪。

## LoRa/FSK/私有 Radio

低速、长 RTT、duty-cycle 受限介质要使用较高 Base Cost、较长 ACK/Heartbeat 和较小控制频率。Radio packet 通常小，Carrier/Fragment Header 占比高；大 Transfer 可能不适合。SNR/RSSI 是指标，不能把瞬时值直接当全网 Route Cost。

区域法规的 duty cycle、发射功率和频率计划属于产品硬约束，UCN 不替代合规设计。

## 地址与准入

MAC/connection handle/radio short address 只是物理 peer key。首次 HELLO/Provider admission 后才绑定 UCN Node ID。两个物理地址声称相同 Node ID 时，应按产品身份/安全策略拒绝冲突，而不是最后到者无条件覆盖。

## 无线队列与回调

SDK RX/TX callback 可能来自高优先级系统任务而非硬 ISR，但仍不应直接执行 Node。复制到固定 Ring/slot并通知 Owner。SDK 队列满、peer missing、channel change 要映射准确错误，不要统一返回成功。

## 实机验收

- 一对一完整 Frame 和随机 payload；
- 多 peer 同时入网、地址冲突和重连；
- logical MTU 边界与 SDK fragmentation；
- 信号变化、丢包、queue pressure 和 Metrics freshness；
- Link Down 检测到 Route/Flow 切换时间；
- 与其他无线协议/频道共存；
- 加密开关、密文透明中继和攻击输入；
- 1/2/3 跳吞吐、控制开销、CPU/堆/功耗。

完成这些之前只能说“公共对接规范和 preset 存在”，不能说对应无线 Adapter 已实现。
