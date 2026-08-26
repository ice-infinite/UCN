# UCN V5 Cluster M07-11 Config Body 双槽恢复自审报告

日期：2026-08-23  
范围：`CLV2-07-11`；实验 Config owner 的完整 body 双槽格式和 Host 掉电模型。

## 实现

新增 caller-owned `ucn_cluster_config_store_t`：两个固定长度、CRC32 保护的
Config body slot。每条记录绑定 canonical Stable Config body、M04 Config ref
和单调 slot generation；写入同一 body/ref 是 no-write idempotent，新的 body
仅替换较旧或无效 slot。

实验 persistence owner 现在必须持有该 store：

1. Prepare 前先写入并验证 Stable(C_new) body；随后才可提交 M04 PREPARED；
2. Commit/Abort、同步 completion 和异步 poll 的 reload proof 都重新匹配
   M04 durable ref 与双槽 body；
3. restart 时 `committed_config` 唯一决定 Active body；PREPARED 时 C_old
   仍是唯一 Active，C_new 只作为 exact staged body 返回；缺少任一所需 body
   即 fail-closed。

这样 durable Commit 在任何生产 v4 broadcast 之前已经能恢复唯一 Stable
C_new；本阶段仍禁止真正的 v4 Config broadcast，因此不伪造该发送测试。

## 掉电/恢复矩阵（软件模型）

| 边界 | M04 Record | 双槽恢复结果 |
|---|---|---|
| proposing / 未 Prepare | C_old | 仅 C_old Active |
| Joint persisted | C_old + PREPARED C_new | C_old 唯一 Active；C_new 仅 staged |
| Commit persisted、尚未发送广播 | C_new | 仅 C_new Active |
| Commit 后 local runtime apply | C_new | 重启仍只选 C_new，不依赖 RAM apply |
| staged C_new 撕裂 | PREPARED C_new | 拒绝恢复，不把不完整 C_new 当作 Active |
| owner/Backup staging reader 更换 | 同一 Record + slots | 独立 owner/recovery 得到相同 C_old/C_new binding |

最后一项是 M07 的 Config body reader replacement；真正 Backup committed/staging
mirror 和接管输入仍明确属于 M09，未被提前实现。

## 自审与验证

- Config canonical deserialize 拒绝保留位/脏尾，不写回 output。
- 双槽验证了 Prepare、Commit、torn C_new、fallback C_old、ref mismatch 和
  output no-write。
- owner restart 在 PREPARED 且两个 body 完整时成功；C_new body 被破坏时
  初始化 `UCN_ERR_STATE`，owner output 不变。
- Windows GCC Full/Lite/Nano 与 WSL Full ASan/UBSan 均为 `12/12` CTest 通过。

## 限制

该对象是 BSP 应放入独立 erase/program unit 的固定字节格式；本轮只做 Host
内存及 CRC/torn-record 模拟，未验证真实 Flash 原子性、掉电波形或 MCU 存储
寿命。没有 production v4 RX/TX/FSM、Authority、Takeover、Adapter 或默认
encoder 接线；M05 顶层 `AUDIT HOLD` 不变。

`CLV2-07-11`：**CODE COMPLETE / SELF-AUDIT PASS**。
