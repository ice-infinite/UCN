# UCN S05 动态综合压力测试

> 状态：代码与软件验证完成；真实介质/多板性能不在本测试中推断。
> 日期：2026-08-10
> 入口：`tests/test_stress.c`、`tests/test_dynamic_stress.c`

## 1. 目标与边界

S05 用 Host 上的固定内存虚拟网络验证 UCN 在动态事件、固定表满和长期运行下不会出现无界增长、无限重试、同 Epoch 路由环、重复业务投递或故障后无法恢复。它不改变 MCU Profile，也不测量真实 ESP-NOW/UART/CAN 的空口、总线、功耗、栈或 Heap。

旧 `test_stress.c` 继续提供 32 Node、2 Link 静态环网的 5000 轮快速回归；新增 `test_dynamic_stress.c` 负责动态综合场景。

## 2. 动态事件网

拓扑固定为 32 Node，每 Node 最多 4 个 Link：相邻 `±1` 与跨距 `±4`，总资源在编译期确定。启动时所有 Link 的 `peer_node_id=0`，双方经 HELLO 和 Open Join 自动绑定，随后才允许普通业务和 AODV 控制帧。

测试事件队列固定为 1024 槽，只存在于 Host 测试程序。Link `send()` 可确定性地注入：

- 25‰ 丢失；
- 80‰ 重复；
- 0～7 ms 延迟，由不同 Due Time 形成乱序；
- 双向 Link Down/恢复；
- 1～200 的基础 Cost 波动；
- 持续 Q1 Endpoint 与每 7 轮一次的 Q0；
- 周期 HELLO Burst 与正常 Heartbeat/AODV/RERR 维护。

每次失败输出 Seed、轮次和最近 16 个事件。默认 Seed 为 `0x51A05EED`，默认动态轮数为 2000。

## 3. 路由不变量

RREQ 从源端 `Cost=0/Hop=0` 出发并逐跳累加，用于选择源到目标的候选。RREP 从目标 `Cost=0/Hop=0` 返回，每个节点按 ingress 对应的目标方向 Link 累加，因此缓存值是“本节点到目标”的剩余 Cost/Hop。

无环检查必须模拟 v4 的真实转发：同一业务帧只使用同一个 `route_epoch`，每一跳只查 Current 或未过期 Previous；直连目标立即终止。不同 Epoch 的缓存表项不能拼接后误判为同一条可执行路径。

测试会先建立未知目标的首包 Q1 路由，再断开源端实际出口。恢复采用固定最多 5 次、每次 1.5 s 的产品侧 Q1 Latest 重试，覆盖第一次调用只完成 Link Down/RERR、后续受 RREQ 最小间隔和发现超时约束的正常情况；没有无限重试。

## 4. 固定资源耗尽

S05 聚合验证以下边界，既有专项测试继续提供更细分支：

| 资源 | 表满行为 | 恢复/回收 |
| --- | --- | --- |
| Neighbor Candidate | 第 `UCN_MAX_NEIGHBORS+1` 项返回 `UCN_ERR_NO_SPACE` | Candidate Timeout 后槽可复用 |
| Route | 第 `UCN_MAX_ROUTES+1` 条静态 Route 返回 `UCN_ERR_NO_SPACE` | 动态网时间跳转清 Route/Candidate 后重新 AODV |
| Policy Path | 第 `UCN_MAX_POLICY_PATHS+1` 项失败 | 显式 Clear 后可复用 |
| Q1 Flow | 第 `UCN_MAX_POLICY_FLOWS+1` 项失败 | Lease 到期后可复用 |
| Path Forward | 第 `UCN_MAX_PATH_FORWARD_ENTRIES+1` 项失败 | 到期后可复用 |
| Service Q0/Q1 | 本机/远端固定队列满时失败 | Take/覆盖后恢复；Q1 保持 Latest |
| Seen/Candidate/Bridge | 由重复注入及既有 `test_candidate_route.c`、`test_service_bridge.c` 覆盖 | 固定缓存/固定 Pending，无动态增长 |

## 5. 运行方式

普通 CI 短跑：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

固定 Seed 长跑；`UCN_STRESS_EVENTS` 接受 `1..1000000`：

```powershell
$env:UCN_STRESS_SEED = '0x51A05EED'
$env:UCN_STRESS_EVENTS = '1000000'
& .\build\Debug\ucn_tests.exe
Remove-Item Env:UCN_STRESS_SEED, Env:UCN_STRESS_EVENTS
```

Linux/WSL sanitizer 与仓库 CI 参数一致：

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize --output-on-failure
```

## 6. 2026-08-10 实际结果

- 5 个不同 Seed，各 1000 轮：全部通过。
- 固定 Seed `0x51A05EED`，100,000 轮：事件入队 614,713、投递 614,573、故障丢弃 14,766、重复注入 45,578、事件高水位 147/1024、业务投递 6844；无重复业务投递、无同 Epoch 环、无事件队列背压。
- Debug、Release、`UCN_MAX_FRAME_BYTES=64`、`UCN_MAX_BEARERS_PER_NEIGHBOR=1` 四个独立 Profile：CTest 均 `1/1`。
- WSL Ubuntu 24.04 / GCC 13.3 的 ASan+UBSan：CTest `1/1`。
- 百万轮入口已实现，本轮未执行，不能写成百万轮已经通过。

上述数字是确定性虚拟事件模型的测试统计，不是网络吞吐、时延或 MCU RAM 指标。真实三板多跳、物理拔线、无线干扰、Service Task 压力和资源峰值继续分别由 S06、S07 验收。
