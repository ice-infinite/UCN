# UCN RT-02 Endpoint 时间策略与双门禁自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 范围：默认不链接的实验软件归档，不接入生产 Node/Service RX/TX

## 1. 本阶段实现

- 新增固定容量 Endpoint Policy Registry，同一 Node 可同时配置 `NONE`、`LOCAL_STAMP`、`SYNCED_STAMP` 与 `DEADLINE`；
- 新增调用者存储上的 Payload Builder，固定组合顺序为 `Envelope | Command Guard | Business`；
- REQUIRED 同步/Deadline 策略禁止隐式降级并强制要求 E2E Protected；
- 发送端按量化后的 sender uncertainty 上界检查，不把原始值小于门限误当作可编码；
- 接收端唯一计算 `U = decoded sender uncertainty + current receiver uncertainty`，并使用 `U <= max_uncertainty_us`；
- Deadline 使用半开区间 `age_upper_us < effective_max_age_us`，Timed Command 同时校验 Guard 与 `capture_time_us` 的毫秒量化绑定；
- REQUIRED 固定拒绝远端 `SOURCE_HOLDOVER`；PREFERRED 仅在策略、E2E 与 Source ACL 三项同时成立时接受；
- 提供诊断式 Evaluate 与无写回的 Execution Admit，业务执行前可以重新读取 Domain view 并再次评估。

## 2. 阶段自审发现与整改

首轮测试和全体交叉审计共发现并修正四项边界：

1. sender 原始 uncertainty 合格但向上量化后可能超过 Policy 门限；现按解码后的 class 上界复核；
2. REQUIRED 同步流若不显式绑定 E2E，可被错误用于安全执行；现由 canonical Policy 强制；
3. 极小 `UCN_MAX_PAYLOAD_BYTES` 配置下，若先写 16/28 B 前缀再检查容量会越界；现先计算完整前缀与业务长度，容量不足时零写回返回 `UCN_ERR_TOO_LARGE`。
4. 最初直接调用 Service Guard Codec，使 `UCN_FEATURE_SERVICE=OFF` 产品无法独立链接 RT-02；现 RT-02 内部使用冻结的 12 B 私有 Guard Codec。Service 开启时测试逐字节比较两份实现，Service 关闭时不保留任何 Service 符号依赖。

## 3. 定向验证

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，RT-01/RT-02 | 2/2 PASS |
| Windows GCC Full Release，RT-01/RT-02 | 2/2 PASS |
| Windows GCC Service OFF | 30/30 PASS，RT-02 独立链接 |
| Service ON Guard Wire 一致性 | 完整 12 B `memcmp` PASS |
| 四种模式同节点策略表 | PASS |
| 完整 32 B `Envelope + Guard + 4 B Business` | PASS |
| REQUIRED 无时钟、无 E2E、无硬件采样 | 零写回拒绝 |
| `64 + 36 = 100` / `64 + 37 = 101` | 等值接受 / 加一拒绝 |
| future-skew 与 `age_upper == deadline` | 边界按合同拒绝 |
| Receive 通过、Execution 时过期 | 不暴露业务指针，完整输出不写回 |
| Release Policy 对象 | `text=5132 B, data=0 B, bss=0 B` |

## 4. 隔离与未完成边界

- `ucn_realtime_policy` 为 `STATIC EXCLUDE_FROM_ALL`，只依赖 RT-01 与现有 Core；
- 没有扩大 `ucn_node_t`、`ucn_service_router_t` 或普通队列项；
- 没有注册生产 Endpoint、没有接入生产 handler、没有启用 Time Sync；
- 本报告只是 RT-02 阶段自审，不代替 RT-03～RT-07、MSVC、实机或外部审计。

## 5. 外审 RT-A01/A02 整改

- RT-A01：REQUIRED 接收现在无条件要求 `source_acl_authorized=true`，它与
  `e2e_protected` 是两个都必须通过的门，不再只在远端 HOLDOVER 特例中读取 ACL。
  ACL false 的 SYNCED/DEADLINE 输入均在暴露业务 Payload 前拒绝，view 不写回。
- RT-A02：`ucn_realtime_send_request_t` 新增显式
  `sample_capture_bound_us/sample_capture_bound_known`。发送端只在当前 Domain
  uncertainty 已知、采样锁存误差已知且非零、checked-add 和量化门限均通过时
  生成共享 Domain Envelope；接收端同样要求本地 clock uncertainty 已知。
- 回归覆盖 unknown/zero sample bound、unknown local uncertainty、采样误差造成的
  class 跨级、REQUIRED ACL false，以及所有失败路径完整 output 不写回。

本节修复已通过受限实验软件范围外部复审；生产 Node/Service 仍未接线。
