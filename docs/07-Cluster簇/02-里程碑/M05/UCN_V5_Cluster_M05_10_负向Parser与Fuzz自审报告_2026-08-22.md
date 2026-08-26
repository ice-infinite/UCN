# CLV2-05-10 负向 Parser 与 Fixed-seed Fuzz 自审报告（2026-08-22）

## 结论

初版被外审指出 P1：每个 Type 只破坏一项字段和 `DISABLED` Role，不足以锁住 RFC4 全字段约束。现已整改为独立 RFC4 fixture table，外部复审已签署 **GO（受限范围）**；`CLV2-05-10` 为 **DONE**，但 M05 总体仍 `AUDIT HOLD`。

本项只强化隔离的 Cluster Wire v4 codec。没有改 RFC4、v3 production codec、Core Wire、pending admission、Cluster production RX/TX/FSM、资格、Authority 或 default-disabled v4 encoder。

## 实现核对

| 项目 | 实现与证据 |
|---|---|
| 独立原始输入 | `tests/test_cluster_wire_v4_codec.c` 手写 RFC4 40 B network-order fixture；负向样本不调用被测 v4 encoder。 |
| Type 覆盖 | Type `1..33` 的每个合法基线都先经 raw decode、strict dispatch、semantic parser 验证。 |
| RFC4 fixture | 独立表固定每 Type 的允许 Role 集、flag contract 和 `P0..P5` 的字段类别；规则不从被测 validator 推导。 |
| 负向矩阵 | 所有 Role 枚举、全部 `0..255` flags 和每个受 codec 约束的 P 字段都独立变异；覆盖 zero/nonzero、serial、Node/Cluster ID、duration、score/offer、reason、range/count、descriptor 与所有 zero-tail。Type 12 另逐项覆盖 BEGIN marker P3..P5。 |
| 输出无副作用 | raw decode、strict dispatch、semantic parser 的失败 output 全用 sentinel + `memcmp` 验证未写回。 |
| Fuzz | 固定 seed `0xC15A4E5D`，4096 次有界 mutation；涵盖随机 40 B、1..3 字节篡改与 39/41 B 长度。成功结果必须有效且可转 semantic，失败不写 output。 |
| 隔离 | `src/extended/ucn_cluster.c` v4 refs=none；`src/adapters/` 和 `include/ucn/adapters/` Cluster refs=none；encoder 默认 0。 |

## 自审结果

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `28/28` CTest 通过。 |
| 定向 v4 codec | 通过。 |
| WSL ASan/UBSan | `18/18` CTest 通过。 |
| WSL `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest 通过。 |
| `git diff --check` | 通过；仅既有 CRLF 转换提示。 |

## 待外审确认

1. 独立 raw fixture 与 33-Type 矩阵能否覆盖 RFC4 的 role、flag、zero-tail、serial/ID 域；
2. fuzz 对成功/失败 output 合同的判断是否足以防止 parser 写回；
3. 无 production Cluster/Adapter v4 接线，encoder 仍为 default-disabled；
4. 本项没有把 codec 容错误表述为 quorum、Authority、Handover、Config、Rekey 或实机传输完成。

M05 总体保持 **AUDIT HOLD**。仅在外部审计确认本项后，才可进入下一个受限 M05 子任务。
