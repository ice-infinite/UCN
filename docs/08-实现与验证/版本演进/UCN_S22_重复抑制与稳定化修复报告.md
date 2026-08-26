# UCN S22～S26 稳定化修复报告

> 日期：2026-08-11
>
> 范围：Host C99 Core、单元/虚拟拓扑、规模模拟和构建门禁。本文不把 Host 结果写成 MCU 实机、真实介质或生产密码证据。

## 1. 结论

S21 在 256 Node、80‰ 重复、每 Node 每 Tick 4 条业务时复现的 1070 次重复业务投递已经修复。同一固定 Seed/参数重放 M1、M2、M4 后，三档均为 100% 原始业务交付、`duplicate_business=0`、无 Route Loop、无 Harness 背压。

本轮同时完成：

- 普通帧去重与 RREQ Best Cost 状态拆分；
- Nano/Lite/Full 按实际 Feature 运行行为测试；
- Service Router Node 0 校验和 Binding 生命周期契约；
- `UCN_MAX_HOPS` 可覆盖及 1～254 编译门禁；
- 生产安全配置未完成时的失败关闭激活门禁。

## 2. 普通帧去重

旧的 8 项逐帧环会被其他帧快速覆盖。新状态按 `(Source, Session)` 建立固定滑动窗口：

```text
Source + Session
├─ highest_sequence
├─ received_bitmap
└─ last_observed_ms
```

默认容量：

| Profile | Source/Session 窗口 | 每窗口 Sequence 位图 | 非活动回收 |
| --- | ---: | ---: | ---: |
| Nano | 4 | 32 bit | 60 s |
| Lite | 16 | 32 bit | 60 s |
| Full | 32 | 64 bit | 60 s |

窗口内未见过的乱序 Sequence 可接收；已经出现或落在窗口外的 Sequence 返回 `UCN_ERR_REPLAY`。只有新高位或窗口内首次出现的乱序 Sequence 会刷新 `last_observed_ms`，重复/过旧重放不能无限占住固定槽。全部窗口都被活跃来源占用时返回 `UCN_ERR_NO_SPACE`，不会静默淘汰仍活跃的来源。

`Session=0` 为兼容开发网络仍可接收，但同一来源重启并复用旧 Sequence 时，接收端无法判断它是重启帧还是延迟副本，因此在窗口过期前按 Replay 拒绝。无生产 Provider 的产品应在每次启动后调用 `ucn_node_set_plain_session_id()` 设置非零 Boot Session；安全网络由 Provider 提供认证 Session。

该窗口只是易失网络去重，不认证来源，也不替代生产 Security Replay Window 或高风险 Command Replay。

## 3. RREQ 状态拆分

RREQ 使用独立固定缓存，键为 `(Origin, Session, Request ID)`，值保存当前 Best Cost。Lite 默认 8 槽，Full 默认 16 槽，非活动项 5 s 后可回收。

接收顺序固定为：

```text
完整格式/安全校验
        ↓
Peek：New / Better / Replay / Full
        ↓
New 或 Better 才申请该 Peer 的 RREQ Token
        ↓
Token 成功后 Commit Best Cost
        ↓
处理/转发 RREQ
```

所以同 Cost/更差的副本不会耗尽合法控制 Token；更低 Cost 请求若因 Token 不足失败，也不会提前污染 Best Cost，Token 恢复后相同更优请求仍可处理。

## 4. Feature-aware 测试

CMake 不再让 Nano/Lite 只跑 Profile Smoke：

| Profile | 直接运行的主要行为测试 |
| --- | --- |
| Nano | Frame、Node、QoS、静态 Route、Endpoint、Adapter RX、Service、Source/Session 去重 |
| Lite | Nano 公共语义 + AODV/RERR、Neighbor/HELLO/Heartbeat、多 Bearer、安全 Provider、Control Budget、Stress |
| Full | Lite + Candidate、Path、Policy/Balance、Trace/Snapshot/Policy Diagnostic、Dynamic Stress |

这次 Nano 直接测试发现并修复了三个真实差异：不支持的 Traffic Class 错误码不一致、Q1 被错误限制为只能 Latest Value、Endpoint 已专用分发后仍进入通用 RX 回调。

## 5. Service、Hop 与生产安全门禁

- Service Router 现在同时拒绝 Node ID `0` 和广播 ID。
- Binding 数组继续采用借用式零拷贝；必须在 Router 全生命周期保持有效且不可修改，推荐 `static const`。这避免每个小 MCU Router 强制复制最多 6 项 Binding。
- `UCN_MAX_HOPS` 默认仍为 16，可通过全工程统一宏覆盖；`0` 和 `255` 编译失败。覆盖 32 的 Full CTest 已通过，但这不证明 32/100 跳产品网络已可用。
- 开发构建默认保持兼容。产品可在编译期定义 `UCN_SECURITY_REQUIRED_BY_DEFAULT=1`，或初始化后调用 `ucn_node_set_security_required(node, true)`。
- Required 状态下，完整 Provider、非零 Session、持久 Sequence、TX/RX 授权、`seal/open`、Node 默认策略和每个 Endpoint 覆盖必须全部禁止明文，`ucn_node_security_ready()` 才返回真；否则 `step/send/receive` 返回 `UCN_ERR_SECURITY`。
- Nano 不含 Security Feature，若强制生产安全构建会在公共头编译期失败。

失败关闭门禁只能防止“忘记配置就明文运行”，不能证明测试 Provider 是生产 AEAD。设备身份、审计密码库、密钥供应/轮换/吊销和逐跳控制面认证继续归 S02。

## 6. 软件验证结果

### 6.1 CTest

| 构建 | 结果 |
| --- | ---: |
| Windows GCC Debug Full / Service ON | 2/2 |
| Windows GCC Debug Lite / Service ON | 2/2 |
| Windows GCC Debug Nano / Service ON | 1/1 |
| Windows GCC Release Full / Service OFF | 2/2 |
| Windows GCC Release Lite / Service OFF | 2/2 |
| Windows GCC Release Nano / Service OFF | 1/1 |
| WSL GCC 13.3 Full / Service OFF / ASan+UBSan | 2/2 |

额外验证：Full、`UCN_MAX_HOPS=32` 为 2/2；0/255 按预期编译失败；Full、`UCN_SECURITY_REQUIRED_BY_DEFAULT=1` Core 构建通过，Nano 同配置按预期编译失败。

### 6.2 S21 重复场景重放

| 场景 | Generated / Accepted / Delivered | 重复业务 | Event HWM | 结果 |
| --- | ---: | ---: | ---: | --- |
| 256 Node，80‰，M1 | 25,600 / 25,600 / 25,600 | 0 | 655/16,384 | PASS |
| 256 Node，80‰，M2 | 51,200 / 51,200 / 51,200 | 0 | 935/16,384 | PASS |
| 256 Node，80‰，M4 | 102,400 / 102,400 / 102,400 | 0 | 1,490/16,384 | PASS |

逐节点与汇总 CSV 位于 [`results/S22`](../../results/S22/)。WSL ASan+UBSan 下另运行 M4，也为 PASS、重复业务 0。

### 6.3 Host 静态资源变化

Windows x64、GCC 14.2、Release、Service OFF：

| Profile | S22 前 Node | 当前 Node | 增量 | S22 前 `.text` | 当前 `.text` | 变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Nano | 2,640 B | 2,648 B | +8 B | 13,904 B | 13,132 B | -772 B |
| Lite | 5,456 B | 5,888 B | +432 B | 50,308 B | 52,120 B | +1,812 B |
| Full | 8,136 B | 9,400 B | +1,264 B | 106,704 B | 109,900 B | +3,196 B |

上述值是 Host ABI 和静态库对象合计，不是 ESP32/STM32 的 RAM/Flash。Full 增量最大，是因为同时保留 32 个 64 bit Source/Session 窗口和 16 项 RREQ 表；Lite/Nano 按 Profile 缩小。

## 7. 仍未完成

- 目标 MCU 的绝对 RAM、Flash、Task 栈、Heap、CPU 和功耗；
- ESP-NOW/UART/CAN 等真实乱序/重复链路；
- 三板以上真实多跳和长路径 Cost/超时/控制预算；
- S02 的真实身份、生产 AEAD、密钥生命周期与逐跳认证；
- 远端 GitHub Actions，必须在推送后读取实际结果，不能由本地测试代替。
