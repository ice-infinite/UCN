# UCN V5-51 物理静默检测窗与关键流保护方案

> 状态：设计、源码、软件门禁和三板 10 轮故障功能门禁均已完成。10/10 轮实现 UART→Wi-Fi→UART 自动切换，最大连续业务缺口为 1；CPU% 与功耗绝对标定仍待后续。本文不把本地 UART TX 入队成功解释为对端收到，也不承诺零丢帧。

## 1. 问题与现有能力

普通 TTL UART 没有天然的 Carrier Detect。物理线拔掉后，本地驱动仍可能把字节写入 TX FIFO 并返回成功；因此 V5-50 的同调用 `LINK_DOWN` 单次 Backup 重试无法在静默故障发生的第一帧触发。

Core 已经具备每个已准入 Bearer 独立的 `HEARTBEAT request/ACK`、`last_seen_ms`、`ADMITTED → SUSPECT → DOWN` 和同 Neighbor Backup 接管。任何通过完整 UCN 帧校验和安全门禁的直连帧都会刷新该 Bearer；所以 V5-51 不再增加 UART 私有 Probe Wire，也不把 HELLO 当成业务 ACK。

## 2. 三条路线结论

| 路线 | 结论 | 原因 |
| --- | --- | --- |
| 再造 Adapter Probe/ACK | 不采用为默认方案 | 与现有一跳 Heartbeat 重复，还会新增 Carrier 格式、完整性与兼容边界。 |
| DCD/CTS/独立 GPIO | 保留为 Adapter 可选快速来源 | 有真实硬件信号时可让 `get_status()` 立即报告 Down；当前三板 TTL UART 接线没有该信号，不能伪造。 |
| 关键流双发+去重 | 本轮不默认实现 | 只在 SUSPECT 后双发无法覆盖“断线到判疑”窗口；全时双发才可能覆盖该窗口，但会改变交付语义、占用两条 Bearer，并需要明确的准入、统计和安全策略。现有 `(Source, Session, Sequence)` 去重可作为未来显式冗余 API 的接收基础。 |

因此 V5-51 的默认方案是：**复用 UCN Heartbeat，增加每 Link 的固定存活档位，让 UART 选择快速档，Wi-Fi 继续使用默认档；物理状态仍可比 Heartbeat 更早失败关闭。**

## 3. Link 存活档位

`ucn_link_t` 增加一个紧凑 `liveness_profile` 字段；零初始化保持默认行为，不改变 Wire。字段必须使用固定枚举，注册 Link 时拒绝未知值。

| 档位 | Heartbeat 间隔 | 进入 SUSPECT | 进入 DOWN/移除该 Bearer | 推荐介质 |
| --- | ---: | ---: | ---: | --- |
| `DEFAULT` | 1000 ms | 3000 ms | 4000 ms | Wi-Fi、ESP-NOW、一般动态介质 |
| `FAST` | 250 ms | 1250 ms | 2000 ms | 板间 UART、RS-485、短时延有线控制链路 |

`FAST` 不设为 `250/750/1500 ms`，因为默认最坏维护服务上界为 800 ms。冻结值满足 `250 + 800 < 1250`，避免正常 Q0/Q1 压力仅因维护排队就误判 UART。

周期 Heartbeat 已被“每 Bearer 固定间隔 + 固定 Link 上限”约束，本轮将其从通用路由/探测 Token Bucket 中分离；RREQ、Candidate Probe、Activate 仍受原 Token Bucket 限制。对端 Heartbeat Request 继续受独立的每 Peer RX Token 限制。这样快速 UART 不会因其他控制事务耗尽通用 Token 而产生假 Down，也不会出现无界发送。

## 4. 状态与选路动作

```text
完整直连 UCN 帧 / Heartbeat ACK
  → 刷新该 Bearer last_seen
  → SUSPECT 可恢复为 ADMITTED

now - last_seen >= suspect_timeout
  → 仅该 Bearer 进入 SUSPECT
  → 同 Neighbor 有 ADMITTED Backup 时立即换 Primary
  → V5-50 同步 Route/Previous/Candidate/Static 的物理代表与本地基础 Cost

now - last_seen >= remove_timeout 或 get_status() 明确 Down
  → 仅该 Bearer DOWN
  → 全部 Bearer Down 才移除 Neighbor、动态 Route/Path 并触发既有 RERR/重发现
```

恢复仍由该 Adapter 的 HELLO 调度完成。FAST 只缩短已准入 Bearer 的存活判断，不代替发现、身份准入或安全 Provider。

## 5. 软件测试任务

1. 注册契约：默认/FAST 可注册，未知档位失败关闭；Nano/Lite/Full 公共头和链接不缺符号。
2. 静默故障：同 Neighbor 的 FAST 主 Bearer 静默、DEFAULT Backup 正常；在 1250 ms 前不误切，到点切换，2000 ms 后只回收故障 Bearer。
3. 延迟/误判：在窗口内注入合法帧或 Heartbeat ACK，FAST Bearer 保持/恢复 `ADMITTED`，不得抖动删除 Neighbor。
4. 混合调度：FAST 与 DEFAULT 同时存在时分别按自己的节拍发送；业务持续排队时 Heartbeat 的最大服务延迟仍不越界。
5. 时间回绕：跨 `UINT32_MAX` 的 FAST 判疑/Down 使用无符号差值，结果不变。
6. 固定资源：确认 `sizeof(ucn_link_t)` 不高于当前 40 B、Node 不新增动态内存，Full/Lite/Nano、Service OFF、128 B/3-Link、Sanitizer/Analyzer 全回归。

## 6. ESP32 三板实测门禁

三板 UART Link 显式选择 FAST，ESP-NOW 保持 DEFAULT。Bench 增加毫秒事件、最近有效 UART RX、Heartbeat 计数、Primary/Route 变化、逐方向序号缺口、最大连续缺口、重复/乱序和资源快照。

至少重复 10 轮 A—B UART 热断/恢复，并报告：

- 最后有效 UART RX → UART SUSPECT/Primary 切换；
- 恢复首个有效 UART RX/HELLO → UART Primary/Route 恢复；
- 每轮缺口集合与最大连续缺口；
- 重复、乱序、发送拒绝、假阳性切换；
- Heartbeat/HELLO 控制帧增量、内部 Heap、任务栈余量和固件尺寸。

验收目标是把 V5-50 的约 2.5～3 s 静默窗口压缩到有证明的 1.25 s 上界，并验证没有假阳性和资源失控。若仍要求断线瞬间近零丢失，后续必须单独冻结“显式关键流全时冗余”或端到端 ACK/重传语义，不能将本轮快速判疑描述成可靠交付。

## 7. 不变边界

- 不升级 v5 Wire，不增加普通帧字节。
- 不把 UART 本地 TX 成功当作对端 ACK。
- 不全网洪泛、不为所有业务无条件复制。
- 不让 Linux 成为存活检测或路由前提。
- 不宣称当前测试 Provider、Heartbeat 或 TTL UART 具备生产密码认证、功能安全或硬实时保证。

## 8. 已实现内容与软件证据

### 8.1 Core

- `ucn_link_t::liveness_profile` 使用既有对齐空隙；当前 Host `sizeof(ucn_link_t)=40 B`，未增加 Node 动态内存。
- Link 注册在 Full/Lite/Nano 中均拒绝未知存活档位；零初始化仍为 `DEFAULT`。
- Heartbeat、SUSPECT、DOWN 分别按当前 Link 档位取时间参数；跨 `UINT32_MAX` 仍使用无符号时间差。
- 当前 Primary 进入 SUSPECT 且同 Neighbor 存在 ADMITTED Backup 时，立即把 Backup 选为 Primary；没有健康 Backup 时才保留 SUSPECT 兜底。
- 周期 Heartbeat 从通用 RREQ/Probe/Activate Token Bucket 分离；单测将通用 Token 清零后，FAST Heartbeat 仍按期发出且不产生通用预算丢弃。
- 没有修改 v5 Wire、Heartbeat 载荷、路由 Epoch、Endpoint API 或安全帧格式。

### 8.2 ESP32 Bench

- UART Adapter 显式使用 `FAST`；ESP-NOW 的零初始化 Link 保持 `DEFAULT`。
- 新增 `V5EVENT` 毫秒事件，记录 Bearer 状态/Primary 与 Route 出口、Cost、Hop、Epoch 的变化。
- `V5LINK` 输出存活档位；`V5UART` 输出最后有效 RX 及年龄；`V5STAT` 输出 Heartbeat 请求/ACK/接收、最大服务延迟与去重计数。
- A/B/C 三目标构建通过，RAM/Flash 分别为 `46828/595727 B`、`48756/596095 B`、`50960/629979 B`。相对 V5-50 最终 Bench 三者 RAM 均增加 176 B，主要来自固定事件快照；Core Link 大小仍为 40 B。
- A/COM5、B/COM34、C/COM35 均已完成写入与 Hash 校验。启动日志确认 UART `live=1`、ESP-NOW `live=0`，A→C 两跳 Route 保持 UART/cost 68。
- 三板持续监测约 467 s 并完成 10 轮 A—B UART 断开/恢复：A/B 各 10 次 `SUSPECT`、10 次 `DOWN`，每轮均切 Wi-Fi Backup 并恢复 UART Primary；9/10 轮 Route 事件在 0～16 ms 内切换，第 10 轮约 1 s。最终 A↔C 为 2 Hop/cost 68。
- 全 10 轮 A/C 端分别记录 11/13 个离散缺序，两个方向最大连续缺口均为 1；重复、倒序、Ping 提交拒绝、UART Carrier 错误、RX Queue 满丢弃、重启和异常均为 0。Heartbeat 最大服务延迟 A/B/C=`58/142/1 ms`，Internal Heap 首末稳定。
- `V5EVENT age` 是 UART Adapter 最近完整 Carrier 帧的代理年龄，不等于 Core 安全/准入门禁后的 `bearer.last_seen_ms`。17/20 个端点代理值为 1248～1249 ms；其余 3 个差值不能用于否定或证明 Core 精确阈值。Core 1250/2000 ms 由专项软件测试直接覆盖；若要每轮板端直接证明，后续增加双时间源诊断即可，不改变协议行为。

### 8.3 回归矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows Full + 配置契约 + Scale | `14/14` |
| Windows Lite | `11/11` |
| Windows Nano | `1/1` |
| Windows Full / Service OFF | `11/11` |
| Windows 128 B / 3-Link 产品配置 | `12/12` |
| WSL GCC ASan + UBSan + Leak Detection | `14/14` |
| WSL GCC `-fanalyzer` | `14/14` |
| `git diff --check` | 通过，仅有既有 LF/CRLF 提示 |

软件证据证明状态机、调度上界、固定容量和跨 Profile 编译成立；第 6 节要求的三板十轮故障功能门禁也已完成。当前 Bench 未启用 CPU Run-Time Stats，因此 CPU 利用率与功耗的绝对标定仍待目标资源任务；FAST 也不提供零丢帧、端到端 ACK/重传或硬实时保证。逐轮结果见 [UCN V5-51 三节点十轮实测报告](UCN_V5_51_三节点十轮实测报告.md)。
