# UCN V5 Cluster M07-06 Config Persistence 自审报告

日期：2026-08-23  
范围：`CLV2-07-06`；实现 explicit Config persistence owner，且仅允许测试/实验调用。

## 合同

1. owner 初始化必须从 Provider 读取一个 READY、schema-v2、已有 committed Config 的快照；Factory Empty bootstrap 不在本项范围。
2. Prepare 只接受当前 durable C_old identity 与 transaction base 完全一致的请求；其 staging ref 是 C_new 的 deterministic canonical digest。
3. ACK 权限仅在 submit 返回 COMMITTED 后，或 PENDING 经 poll 后 reload 到完整相同 journal 时获得。PENDING/FAILED 从不构成 ACK 权限。
4. Commit 除 staging transaction/identity 匹配外，必须先满足 C_old 和 C_new 的双 quorum；持久化失败、加载不一致或 quorum 未达到时不会产生 Commit 权限。

## 定向回归

| 场景 | 结果 |
|---|---|
| sync Prepare → old/new quorum → Commit | 两次均 submit/reload proof；最终 durable Config 指向 C_new |
| 未满足 Joint quorum 的 Commit | 返回错误，Provider submit count 不变 |
| submit FAILED | 返回 Provider error，durable state、pending 均不变 |
| async Prepare，连续 PENDING | pending 期间 Commit 拒绝、durable=false |
| terminal COMMITTED | 仅在 load exact journal 后返回 durable=true 和 PREPARE action |

## 隔离审查

模块没有调用 Cluster RX、TX、step、Adapter 或 Authority；亦不引用 `VOLATILE_TEST`。它不会自行发送 CONFIG_ACK/CONFIG_COMMIT，只向明确的 test/experiment owner 返回 durable proof。M04 的公共 `ucn_cluster_persist_config_prepare/commit()` 在默认产品仍保持 fail-closed。

## 验证

- Windows GCC Full/Lite/Nano：各 `9/9` CTest 通过。
- WSL Full ASan/UBSan：`9/9` CTest 通过。
- `git diff --check`：无空白错误；仅已有 CRLF 提示。

## 结论

`CLV2-07-06`：**CODE COMPLETE / SELF-AUDIT PASS**。这建立了 persist-before-ACK/Commit 的实验值层，但不是生产 Config FSM、wire 或掉电实机签字。M05 顶层 AUDIT HOLD 不变。
