# 五分钟构建与最小 Host 示例

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

```powershell
cmake -S . -B build -DUCN_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

建议使用独立构建目录并先检查 CMake 输出中的 Profile/Feature。Windows 多配置生成器运行 CTest 时需要 `-C Debug`；Ninja/Unix Makefiles 通常不需要。

## 预期结果

configure、build 和 CTest 都应返回 0。若某测试失败，先查看第一条失败输出，不要继续写硬件 glue。默认测试包含 Core/Frame/Route/Adapter 等软件门禁，但不代表 UART/CAN/Wi-Fi 驱动已经在目标板通过。

最小 Host 程序创建两个 Node、各自静态 storage 和 Fake Link，把发送回调接到对端 RX 队列；注册 Endpoint 后循环调用两个节点的 `step(now_ms)`。应先验证一帧 Q0/Q1 收发，再加入路由、Transfer 或 Cluster。

## 最小拓扑

```text
Node A (ID=1) ── Fake Link A→B ──> RX Queue B
Node B (ID=2) ── Fake Link B→A ──> RX Queue A
```

Fake Link 的 `send()` 必须复制 frame 到对端队列，不能保存调用者的临时输出 buffer。两个 Node 各有独立 Link、queue、session、sequence 和 Owner 时钟。

## 建立步骤

1. 分别构造 `ucn_config_t`，使用相同 `network_id` 和不同 `node_id`；
2. `ucn_node_init()`，设置非零 plain session；
3. 创建 A/B 两个 Link，并把 `peer_node_id` 指向对端；
4. 注册 Link，添加固定直达 route，排除动态发现干扰；
5. B 注册业务 Endpoint handler；
6. A 调用 `ucn_node_send_endpoint()`；
7. 循环 pump 两端 RX queue 和 `ucn_node_step(now_ms)`；
8. 断言 B 收到正确 source/endpoint/payload，A/B stats 无格式错误。

### 伪代码

```c
init_node(&a, 1U, 2U, &link_ab);
init_node(&b, 2U, 1U, &link_ba);
ucn_node_add_route(&a, 2U, &link_ab);
ucn_node_set_endpoint_handler(&b, 0x40U, on_message, &received);

ucn_node_send_endpoint(&a, 2U, 0x40U,
                       UCN_TRAFFIC_Q1_REALTIME,
                       payload, sizeof(payload));

for (now = 1U; now < 100U && !received; ++now) {
    pump_fake_links();
    ucn_node_step(&a, now);
    ucn_node_step(&b, now);
}
```

函数签名以公共头为准；伪代码强调流程，不应绕过项目已有 Host fake Port/测试辅助。

## 下一步递增验证

- 删除固定 route，验证 RREQ/RREP；
- 增加第三节点，验证一跳中继和 TTL；
- 让一条 Link down，验证 RERR；
- 使用 T128，验证 Transfer Fragment/ACK；
- 同一 Endpoint 分别本机/远端调用 Service；
- 最后才接入安全和 Cluster。

每增加一层都保留上一层回归，这样硬件异常时能定位是 Driver、Carrier、Core、Route 还是应用。

具体函数签名以 `include/ucn/` 和现有 tests 为准。Host 成功只证明软件合同，不证明真实 UART/CAN/Wi-Fi 时序。

## 常见构建问题

- CMake cache 仍使用旧 Profile：换新 build 目录；
- MSVC CTest 找不到可执行文件：补 `-C Debug/Release`；
- Nano/Lite 链接高级 API 失败：检查是否应返回 stub、target 是否链接正确 archive；
- Release 才失败：检查未初始化/padding/UB，并运行 sanitizer 构建。
