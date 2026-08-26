# UCN V5-65 Transfer 冷启动寻路重入缺陷

> 日期：2026-08-14
> 状态：源码修复、Host 软件矩阵及四板自动 1/2/3 跳复测均已完成。
> 范围：T8K Transfer 在没有预热 Route 时触发的 AODV-Lite Expanding Ring。

## 1. 实机现象

四块 ESP32-S3-N16R8 按 A—B—C—D 使用三段 3 Mbaud UART 连接，ESP-NOW
关闭。A 在清洁启动 10 s 后直接向 D 提交一个 8,192 B、窗口 8 的可靠 Transfer：

- Transfer 返回接受，但 45 s 内 `fragments=0`；
- A 累计 96 次 RREQ、0 RREP；
- D 为 0 RREQ、0 RREP，没有收到 Fragment；
- B/C 到 A 的反向 Route Epoch 持续增长，Ingress Reject 同步增长；
- 三段 UART Heartbeat 正常，无 COBS/长度/溢出、CRC、Panic 或 Watchdog。

同样的冷启动 H1/H2 各完成 10/10。四节点改用公开静态路由后，H1/H2/H3
共 30/30、0 重传/CRC 错；因此物理串口和三跳数据面可用，失败位于冷启动自动
寻路控制面。

外部证据位于：

```text
E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1\test_results\
  v5_hop_benchmark_3m_8k_h3_20260814_run2.log
  v5_hop_benchmark_3m_8k_static_h1_20260814.log
  v5_hop_benchmark_3m_8k_static_h2_20260814_run2.log
  v5_hop_benchmark_3m_8k_static_h3_20260814.log
  v5_hop_benchmark_3m_8k_static_summary_20260814.csv
```

## 2. 源码根因

`send_tx_fragment()` 在业务首片调用 `ucn_node_send()` 返回
`UCN_ERR_NOT_FOUND` 后，无条件调用 `ucn_node_discover_route()`。
`ucn_transfer_step()` 会在活动 TX Slot 上持续重试这条路径。

公开 `ucn_node_discover_route()` 当前把 `restart_active=true` 传入内部发现入口。
当同一目的 Discovery 已活动时：

1. 距上次发送不足 `UCN_ROUTE_REQUEST_MIN_INTERVAL_MS=100 ms` 时暂不发送；
2. 达到 100 ms 后，用相同 Ring Scope 重新发 RREQ；
3. 同时重置 `overall_started_at_ms` 和 `deadline_ms`；
4. Ring Deadline 为 `UCN_ROUTE_RING_TIMEOUT_MS=250 ms`。

因此高频 Transfer Step 每约 100 ms 重启初始 2-Hop Ring，早于 250 ms 到期，
`send_due_route_discovery_ring()` 永远不能把 Scope 扩到 4 Hop。96 次 RREQ 是这个
状态机的实机表现，不是 D 链路速率过低。

V5-63 的九档测试先发送 T32/T64，业务层的路由准备过程使后续 T8K 使用已建立
路线，所以该测试仍证明扩展 Epoch 修复与三跳数据面；它没有覆盖“T8K 作为第一
条业务、Transfer 内部等待路由”的冷启动场景。

## 3. 实际修复

1. 在 Transfer 的 `UCN_ERR_NOT_FOUND` 分支先查询
   `ucn_node_route_pending(node, destination)`；已有发现时只等待 Core 推进，不调用
   Restart API。
2. 新增 Transfer 内部 `ensure_route_discovery()`；首次无 Pending 时才调用公开
   Discovery。公开 API 的管理面“显式重启”语义保持不变，因此没有扩大 API/Wire
   变更范围。
3. 不允许普通业务重置 `overall_started_at_ms`、当前 Ring Deadline 或 Request ID；
   2→4→8→16 只能由 Core 到期状态机推进。
4. Discovery 成功后立即发送首片；Discovery 整体失败后，Transfer 继续受自己的
   绝对 Deadline 限制，不无限 RREQ，也不延长借用 Payload 生命周期。
5. Fragment 发送和返回 ACK 的无路由分支统一调用同一 Ensure Helper，避免两个
   方向以后再次产生不同的重入行为。

## 4. 回归门禁

- A-B-C-D 虚拟线形拓扑，T8K 是第一条业务；2-Hop Ring miss、4-Hop Ring hit，
  41 个 Fragment 完成并收到最终 ACK。
- 在 250 ms 内高频调用 Transfer Step，不增加同 Scope Request ID，不刷新 Ring 或
  Overall Deadline。
- H1/H2 仍能发现；H3 不依赖先发 Direct 小包预热。
- 最大 Ring 无目标时在有界时间结束；随后允许新的业务重新开始一次发现。
- 覆盖时间回绕、Q0/Queue 背压、Full/Lite/Nano、低资源产品配置、Sanitizer 和
  Analyzer；修复后再复跑四板自动 H1/H2/H3 各 10 轮。

## 5. 与 V5-64 的边界

V5-65 是**单 Origin、单目的、Transfer 冷启动重入**问题；V5-64 是多个 Origin
同时发现共享目的时的 Route Epoch 所有权问题。两者可能都表现为 Epoch 变化，
但触发条件和修复位置不同，不能合并为一次未经隔离的修改。

## 6. 回归结果

### 6.1 Host 四节点冷启动

新增 A—B—C—D 线形 Transfer 回归，不预装 Route，第一条业务直接使用本地允许的
最高 Class（默认 Full 为 T8K）：

- 10 ms 首次 Step 发出 1 次 2-Hop RREQ；
- 110 ms、210 ms 再次 Step，RREQ 仍为 1，证明 Transfer 没有重启活动 Ring；
- 261 ms 由 Core 到期扩为 4 Hop，RREQ 变为 2、Ring Expansion 为 1；
- 建立路线后完成全部 Fragment、最终 ACK、CRC 校验和接收 Slot Release。

软件矩阵：Windows MinGW Full/Lite/Service OFF 均 `11/11`，Nano `1/1`，128 B/3-Link
产品配置 `12/12`；WSL GCC 13.3 ASan+UBSan 和 `-fanalyzer` 均 `11/11`。全部构建
保持 `-Wall -Wextra -Wpedantic -Werror`，`git diff --check` 通过。

### 6.2 四板自动路由复测

四块 ESP32-S3-N16R8 保持 A—B—C—D 三段 3 Mbaud UART、250 B Wire、窗口 8，
关闭静态路由后分别从清洁 A 会话执行 T8K 10 轮：

| 跳数 | 自动寻路 | 成功 | 重试 | 中位耗时 | 中位吞吐 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | 直连，不需要 RREQ | 10/10 | 0 | 173.5 ms | 46.109 KiB/s |
| 2 | 1 次 2-Hop RREQ | 10/10 | 0 | 215.0 ms | 37.209 KiB/s |
| 3 | 2-Hop miss 后扩 4-Hop，共 2 次 RREQ | 10/10 | 0 | 245.5 ms | 32.587 KiB/s |

H3 首轮包含寻路与扩圈为 498 ms；路线建立后多数轮次为 230～247 ms。最终路线为
3 Hop/Cost 102/Epoch 2；三组共 1,230 Fragment、30/30 Delivered、0 重试、0
Transfer 失败，UART Decode/Length/Overflow、Adapter Queue Drop、CRC、Panic 和
Watchdog 均未出现。动态路由中位吞吐相邻损失为 `19.303%/12.422%`，3 Hop 相对
1 Hop 下降 `29.327%`。

新增实机证据：

```text
E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1\test_results\
  v5_hop_benchmark_3m_8k_dynamic_fixed_h1_20260814.log
  v5_hop_benchmark_3m_8k_dynamic_fixed_h2_20260814.log
  v5_hop_benchmark_3m_8k_dynamic_fixed_h3_20260814_run2.log
  v5_hop_benchmark_3m_8k_dynamic_fixed_samples_20260814.csv
  v5_hop_benchmark_3m_8k_dynamic_fixed_summary_20260814.csv
```

这组结果关闭 V5-65 的单源 UART 冷启动缺陷；它不关闭 V5-64 多 Origin Epoch、
ESP-NOW 四节点、逐段断链/恢复、长稳、CPU/功耗或其他 Bearer/RTOS 实机门禁。
