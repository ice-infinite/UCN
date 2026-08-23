# UCN V5 Cluster M05 MAJOR-A Release Gate 整改自审报告（2026-08-23）

## 1. 范围与结论

本报告只处理合并审计指出的 **MAJOR-A**：Release 优化等级下，typed `wire_offer` / `selected_wire_offer` 的成功输出曾依赖结构体 padding 的历史字节，导致完整对象比较在 Debug 以外不能作为可靠证据。

结论：**CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL**。

M05 总体仍为 **AUDIT HOLD**。本整改不接入 production v4 RX/TX/FSM、Authority 或默认 v4 encoder，也不覆盖实机、Flash 或掉电。

## 2. 根因

`ucn_cluster_wire_v4_selected_wire_offer_t` 含有不同对齐宽度的字段，存在 ABI padding。旧成功路径只写具名字段；调用者传入的对象若不是全零，padding 可能保留原有值。在优化级编译、跨编译器或完整对象 `memcmp` 检查时，结果就不再由协议字段唯一决定。

这不是 wire word 编码歧义；问题位于 typed API 的输出对象 canonical 化和 Release 级回归缺失。

## 3. 整改

1. `wire_offer_from_word()`、`selected_wire_offer_from_word()` 和 `wire_offer_negotiate()` 仅在完成所有输入校验、确认成功后才处理 output。
2. 成功路径先零化局部 typed 对象，再写入具名字段，最后一次性完整复制到 caller output。
3. 失败路径继续在任何 output 写入前返回，保持既有 fail-closed/no-write 合同。
4. `UCN_ENABLE_WIRE_V4_RELEASE_GATES` 默认 `OFF`；只有显式开启时才建立独立 test-only codec target：GCC `-O1/-O2/-O3`、MSVC `/O1,/O2`。该 target 不把 encoder 宏或 v4 行为泄漏至 production `ucn_cluster`。
5. 定向回归以 `0xA5` 填充 output，成功后对整个对象（包含 padding）作 `memcmp`；因此 parser 或 negotiate 只写字段而遗漏 padding 会失败。

## 4. 自审项目

| 项目 | 结果 |
|---|---|
| 成功输出完整 canonical 化 | PASS |
| 失败 output 无写回 | PASS |
| GCC Release O1/O2/O3 test-only gate | PASS，`3/3` |
| MSVC Release O1/O2 test-only gate | PASS，`2/2` |
| Windows GCC Full / Lite | PASS，各 `37/37` |
| Windows GCC Nano | PASS，`27/27` |
| Windows GCC Service OFF | PASS，`13/13` |
| MSVC Debug Full | PASS，`24/24` |
| WSL ASan/UBSan | PASS，`27/27` |
| WSL GCC `-fanalyzer -Werror` | PASS，`16/16` |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

## 5. 关联审计补证

- M03：增加 Candidate 同簇旧 Term 静默拒绝与远端 higher-Term `HIGHER_AUTHORITY` 重定向回归；增加 `STEPPING_DOWN → TERM_CONFLICT_WAIT` 回归。
- M06：正式固定当前受限产品姿态：v3 只可作为临时 non-voting member，10 s 清扫，不可成为 Backup/failover，keepalive 不得延长 provisional deadline。
- M08：文档与注释统一为双层模型：Authority Fence 立即撤销数据面权力；`TERM_CONFLICT_WAIT` 保留控制面安全等待，二者不互相替代。

## 6. 外部复审重点

1. 成功路径是否在 Release ABI 下对完整 typed output deterministic；
2. 错误路径是否仍然严格不写 caller output；
3. Release gate 是否确实独立、默认关闭，且不改变 production v4 隔离；
4. M03 Candidate stale/higher 与 Stepdown conflict 回归是否覆盖合并审计的遗漏边。

## 7. 未放行边界

- 不以本报告解除 M05 `AUDIT HOLD`；
- 不放行 v4 production RX/TX/FSM、Authority、资格决策或默认 encoder；
- 不放行实机兼容、真实 Flash、掉电原子性或 MCU 资源结论；
- 未经外部复审及用户授权，不提交、不推送，也不进入 M09。
