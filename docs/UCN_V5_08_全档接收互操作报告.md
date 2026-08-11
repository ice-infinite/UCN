# UCN V5-08 全档接收互操作报告

> 日期：2026-08-11
> 范围：W0～W3 统一解码、低发送档/宽接收档、普通静态 Endpoint 指令跨档接收；不增加 Nano/Lite/Full 未编译的高级功能。

## 结论

V5-08 将下列关系冻结为官方互操作规则：

```text
Build Profile（Nano/Lite/Full）决定代码和状态表能力
Wire Profile（W0/W1/W2/W3）决定这一帧的线上编码宽度

本节点 TX：选择本地域能够表达的最低档
本节点 RX：默认允许到 W3
```

Nano、Lite、Full 都使用 `ucn_frame_decode()`，因此都具备 W0～W3 基础帧解码能力。Node 初始化保持 W3 TX/W3 RX；产品若将 TX 收窄到 W0/W1，推荐仍把 `max_receive_profile` 保持为 W3。这样低资源节点可以接收高档节点发出的普通 Endpoint 指令，而不必把自己的每个上行小包扩大成 W3。

```c
ucn_node_init(&node, &config);
ucn_node_set_wire_profiles(&node,
    UCN_WIRE_PROFILE_W0_LOCAL,
    UCN_WIRE_PROFILE_W3_BACKBONE);
```

## 实现与测试

- `ucn_node.h` 明确了统一 Decoder、低 TX/宽 RX 推荐策略和 Build Feature 边界。
- `test_node_wire_profile.c` 新增低档 Node 专项：Node 固定 `W0 TX/W3 RX`，同一个静态命令 Endpoint 依次接收 W0、W1、W2、W3 帧，并核对实际 Profile、Payload 和分发次数。
- 原有 `W0 TX/W0 RX` 节点拒绝 W1 帧的负向测试保留，证明产品主动收窄接收上限仍然失败关闭。

Windows Debug 与 Release 的 Core/Scale 回归均通过：Nano `1/1`，Lite/Full `3/3`；V5-09 独立配置契约为 `3/3`，WSL Full 在显式启用配置契约后 ASan+UBSan `6/6`。33/50/64 B 最小 MTU 继续按既有门禁只证明 Core 编译通过；完整测试套件包含超过最小 MTU 的 W3 大载荷向量，不能在最小 MTU 配置下全跑。

## “能解码”不等于“具备全部功能”

低档 Node 接收高档帧还必须满足：

1. `max_receive_profile` 不低于入站帧 Profile；
2. Network ID 一致，Source/Destination/Session/Hop 合法；
3. Link MTU 和 `UCN_MAX_FRAME_BYTES` 能容纳该帧；
4. 静态 Endpoint 已注册，Payload ABI 一致；
5. 安全策略与 Provider 允许该明文或受保护帧。

Nano 收到 W3 编码的普通 Endpoint 指令可以分发，但仍不会因此获得 AODV、Path、Policy 或诊断能力；这些消息按 Build Profile 的 Feature 门禁返回 `UCN_ERR_CONFIG`。产品也可以因极小 MTU、功耗、攻击面或安全域要求把最大接收档收窄到 W0/W1/W2。

## 资源边界

本项不增加 Node 字段、队列、动态内存或线上字节；统一 W0～W3 Decoder 在 v5 已存在。新增内容只有 API 契约、测试与文档。真实 MCU 解码耗时、栈峰值、无线吞吐和受攻击输入下的 CPU 预算仍归 S06/S07 实机门禁。
