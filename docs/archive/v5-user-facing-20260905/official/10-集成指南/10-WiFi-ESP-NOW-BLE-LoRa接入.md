# Wi-Fi、ESP-NOW、BLE、LoRa 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

仓库不内置这些厂商驱动。产品实现一个 Link/Adapter：

1. TX 回调接收完整 UCN frame；
2. 驱动 completion/接收回调只投递固定队列；
3. Owner 上下文提交 RX；
4. 提供 MTU、可用性、RSSI/丢包/队列等指标；
5. 将对端 MAC/连接句柄映射到 UCN neighbor/link 上下文。

UCN 统一上层寻址和消息语义，但不会替代 Wi-Fi 关联、ESP-NOW peer、BLE connection 或 LoRa PHY/MAC 的初始化。

## 共同 Adapter 合同

无线 SDK callback 交付的 packet 指针通常只在 callback 内有效，因此必须复制到固定 queue，随后 Owner 调用 Node RX。Link send 只提交到 SDK/产品 TX queue；“SDK 接受”不是空口完成或对端收到。

每个 peer/connection 维护物理地址到 UCN Node ID 的 binding，只有 Join/Security 通过后才视为 admitted neighbor。广播发现包和已认证业务包应有不同 rate/security 策略。

## Wi-Fi UDP/Raw

Wi-Fi 关联/IP/DHCP/Socket 由产品实现。UDP packet 天然有边界，可一包一条 carrier；TCP 是流，必须使用 Stream framing。若用 multicast/broadcast 做 HELLO，只在一跳局域网内发送并限频，不能让 Heartbeat全网转发。

Wi-Fi 的 MCS/PHY rate 波动很大，preset 只是初值；metrics 应用实际 queue、retry、RTT、RSSI/quality 与 channel busy。RSSI 与 quality来自同一来源时避免重复罚分。

## ESP-NOW

产品维护 peer table、channel、加密 key/LMK 和 send callback。ESP-NOW payload MTU 应由当前 SDK/模式实际确认并报告给 Link；不要假设历史 250 B 对所有芯片/版本成立。callback 中复制 source MAC+payload，并映射 ingress Link。

ESP-NOW 自动 peer发现并不等于 UCN 安全入网；UCN Join/authorization仍决定 Node 是否进入网络。

## BLE

需要选择 GATT characteristic/L2CAP CoC 或厂商 data channel。GATT notification/write 的 ATT MTU 可能远小于 Core frame，必须使用 Stream/Carrier分片或选择更窄 Wire Class。Connection handle 是短期物理身份，断开重连后重新绑定 Node/session。

## LoRa/低速无线

LoRa P2P airtime长、占空比/法规限制严格，base cost与 RTT很高。需要显式限制 Heartbeat、Route discovery、诊断和 Transfer；8 KiB 大消息在某些区域法规下没有实用意义。产品必须写明 SF/BW/CR、频段、duty cycle、最大 payload和半双工仲裁。

## 安全

链路层加密不能替代 UCN E2E/Endpoint ACL：中间网关或已加入 Wi-Fi/BLE 网络的设备仍可能看到/注入业务。外部无线产品应默认 `security_required=true`，同时审计 key provisioning和重放。

## 验收矩阵

- peer 加入/移除、MAC/handle 重用、channel/connection 变化；
- packet queue 满、SDK callback 并发、发送 completion丢失；
- 弱信号、干扰、断开/重连、多个 peer并发；
- MTU 变化、窄 Class、Transfer；
- metrics→Cost→Route 切换与滞回；
- 未认证发现、伪造 source、replay和广播风暴。
