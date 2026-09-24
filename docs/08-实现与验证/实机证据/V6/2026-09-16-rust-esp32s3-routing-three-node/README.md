# UCN v6 简化版 Rust ESP32-S3 三节点动态路由实测记录

## 1. 结论

2026-09-16，Rust 简化版在现有 `B—C—D` UART 物理链上完成双向动态发现、两跳转发、
错误 RERR 防误删、路由失效和自动重发现：

```ini
RUST_ESP32S3_ROUTING_THREE_NODE = PASS
TOPOLOGY                        = B(COM53) -- C(COM58) -- D(COM56)
BAUD                            = 691200
DYNAMIC_DIRECTIONS              = B->D + D->B
RELAY_DYNAMIC_ROUTES            = 4
UNIQUE_RTT_SAMPLES              = 44
COMBINED_RTT_P50_US             = 1020
COMBINED_RTT_P95_US             = 1023
TIMEOUT/CARRIER/WIRE/ROUTE_ERR  = 0
```

这是 RUST-07 自动路由首次进入真实 ESP32-S3 多跳物理链的证据，不是整个 RUST-10、
Security、Flow 或生产 Driver 的放行。

## 2. 板卡、链路与同镜像角色选择

三块板烧录同一个 ELF，固件按 eFuse MAC 选择角色：

| 角色 | 控制台 | MAC | 数据 UART |
| --- | --- | --- | --- |
| B / Origin | COM53 | `7c:4f:ad:2a:92:44` | UART2，GPIO19/20，Link 1 |
| C / Relay | COM58 | `7c:4f:ad:29:9c:d8` | 西侧 UART2 GPIO19/20，东侧 UART1 GPIO15/16 |
| D / Target | COM56 | `7c:4f:ad:2a:8f:98` | UART1，GPIO15/16，Link 2 |

实际物理链仍是前序六板唯一周期探针确认的：

```text
B -- C -- D -- A -- E -- F
```

本次只重刷 B/C/D；A/E/F 保持前序 Rust smoke 固件，没有改变接线。

## 3. 本次实际运行的协议路径

### 3.1 双向发现

B 发起 `B→D` Discovery，D 独立发起 `D→B` Discovery。每一方向均真实调用
`ucn-routing` 的固定容量接口：

```text
ensure_discovery
  -> RREQ encode
  -> C on_rreq / reverse obligation / forward
  -> target make_rrep
  -> C on_rrep / forward / complete_rrep_forward
  -> origin on_rrep / SoftRoute active
```

RREQ、RREP 的路由 Payload 使用生产 Rust codec。UART 是字节流，测试固件在外层增加
`magic + length + CRC16` 以恢复物理帧边界；该 Carrier 不属于 UCN Wire ABI。

两个方向完成后，C 持有 4 条动态 SoftRoute，并记录：

```text
UCN_RUST_ROUTE RELAY_PATHS_READY role=C routes=4
```

### 3.2 两跳业务转发

B/D 的业务帧使用真实 `C1/A0/O0/H0/Q1` codec。C 对收到的每个数据帧执行：

```text
C1 decode
  -> 构造精确 RouteDomain
  -> RouteOwner::resolve(current facts)
  -> 核对 next Link
  -> Hop Limit 递减
  -> C1 re-encode
  -> 从另一 UART 发出
```

C 不依赖测试脚本指定下一跳，也没有绕过 `RouteOwner::resolve()` 直接透传。B 与 D 均通过
C 收到对端 Ping 并返回 ACK。

### 3.3 错误 RERR 与失效重建

B 在初始路由建立后，以生产 RERR codec 做一次板内负向门禁：只篡改
`route_causal_id`，解码后交给 `on_rerr()`。结果为 `preserved=1`，原路由仍可解析。

B 获得 8 次成功 Ping 后，对当前下一跳 Link Generation 执行软件失效：

1. `invalidate_link()` 精确删除 1 条本地路由；
2. 紧接着的 `resolve()` 拒绝旧路由；
3. B 重新创建 Discovery；
4. RREQ/RREP 再次经过 C；
5. B 以新 Route Generation 恢复。

观测到 B 的初始 Route Generation 为 1，恢复后为 3；D 的独立方向为 2。该数值只用于
说明本次运行中确实创建了新路由，不定义跨重启持久化语义。

## 4. 正式采集结果

35 秒正式门禁结果如下：

| 节点 | 不重复 RTT 样本 | 最小 | 均值 | P50 | P95 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| B | 20 | 1017 µs | 1019.45 µs | 1019 µs | 1020 µs | 1025 µs |
| D | 24 | 1018 µs | 1189.92 µs | 1020 µs | 1023 µs | 5101 µs |
| 合计 | 44 | 1017 µs | 1112.43 µs | 1020 µs | 1023 µs | 5101 µs |

D 的首个 5101 µs 样本与 5 秒周期摘要日志重合；其后稳态样本集中在约 1.02 ms。本报告
保留该样本，不从统计中删除。日志输出会影响当前轮询式测试固件的最坏延时，不能把本次数字
直接当作 DMA/ISR 生产 Driver 的极限。

三个节点均满足：

- 正确角色 READY；
- C 四条动态路由与数据转发均被观测；
- B/D 各至少 12 个去重 RTT 样本；
- B 错误 RERR 保护、失效拒绝和恢复全部出现；
- `timeout/carrier_err/wire_err/route_err/tx_err/rx_err` 均为 0；
- 无 Panic、Watchdog、Guru Meditation 或串口读取失败。

## 5. 自审中发现并关闭的测试编排问题

第一次采集时，B 的 `B→D` 路由先完成，测试业务流在 C 的 `D→B` 方向完成前启动，导致
首个 ACK 无反向路由，出现 1 次 Timeout 和 1 次 Relay Route Error。该次运行正确判定失败，
原始证据保存在 `attempt-01-coordination-failed.log/json`。

整改仅延后首次业务流，等待两个独立 Discovery 收敛；稳态 Ping 周期、路由算法和 Wire 均未
改变。第二次协议运行已经零错误，但采集器仍把易被 FTDI 分片的冗余 BOOT 文本当成硬条件，
因此保持 HOLD。最终采集以固件特有的 READY 作为“该镜像已启动并完成 MAC 角色选择”的证明，
没有放宽任何协议错误条件。

## 6. 工件哈希

| 工件 | SHA-256 |
| --- | --- |
| 三节点同镜像 ELF | `539BD3DBA06BC48A78C7B974D59DF39790FCD5FF82F4EEBB836E7FF14A23264B` |
| 正式原始日志 | `5B4709DD510A1B1451AC99C171D7015007F65495C13BA0669B424BBEFCA46561` |
| 正式汇总 JSON | `70755E1C05A67776B79754546879B7E66D4985D70285729E382C20B14EF7A328` |
| 首次失败日志 | `E6083630FA4DC29CBBE49A66CAEBF70745E1AA20FA49E2A7687942388D847F81` |
| 首次失败汇总 | `EECD29F58ECCB2A7A003B872DF3DE52D1E46BBD4CF0E21ADD5D78217CEB73A66` |

## 7. 已证明与未证明边界

本次已经证明：

- 同一 Rust 镜像在三个真实 ESP32-S3 上按 MAC 形成 Endpoint/Relay 角色；
- `ucn-routing` RREQ/RREP、Reverse obligation、SoftRoute 和 Route Generation 在两跳物理链运行；
- C 对每个数据帧按当前 Route Facts 查路并转发；
- 两个方向均能传输真实 C1 帧；
- 错误 causal RERR 不误删，软件 Link 失效后旧 Route 不可用且能重发现；
- 35 秒窗口内三节点协议错误计数为零。

本次没有证明：

- 拔线、Driver Down、迟到 RERR 的跨节点物理收敛；
- `ucn-adapter` 在本三节点镜像中的 Token 生命周期；Adapter 已由前一项 B/C 一跳独立证明；
- 生产 DMA/ISR Driver、无线/CAN/USB Bearer；
- Security、Admission、Capability、Flow、Reliable、Transfer、Service；
- 真实 Flash、掉电恢复、资源上限、功耗和长时间稳定性。

因此本项可记为“Rust 动态路由三节点实机 PASS / RUST-10 部分证据”，不能扩大为整个 Rust
v6 简化协议或产品发布 GO。
