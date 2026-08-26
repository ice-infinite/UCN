# UCN V5 Cluster M07-05 Joint Quorum 自审报告

日期：2026-08-23  
范围：`CLV2-07-05`；仅实现 Config transaction 的本地 bitmap quorum 值函数。

## 自审发现与整改

在为双 quorum 准备反例时发现：初版 `config_tx` 虽绑定 C_old，却允许任意 C_new。该接口若被未来 owner 误用，可能把多个成员变化伪装成一次 add/remove。已将 `config_tx_is_valid()` 和 `begin()` 同时收紧：

- ADD：C_new 必须等于 C_old 加唯一 proposal Node ID。
- REMOVE：C_new 必须等于 C_old 去掉唯一 proposal Node ID，且不能形成空集。
- 非法 delta 在 active validator 与 begin 双重拒绝，输出/原 transaction 不写回。

## Quorum 合同

`ucn_cluster_config_joint_quorum_reached(tx)` 只有在 active canonical transaction 中、且以下两项分别为真时返回 true：

```text
popcount(old_ack_bitmap) >= quorum(C_old)
AND
popcount(new_ack_bitmap) >= quorum(C_new)
```

每个 bitmap 都先检查没有超出对应 canonical voter set 的位；没有单一“总票数”捷径。

## 反例和验证

- Head 自投后不足以达到任一 quorum。
- C_old `2/3` 而 C_new `2/4` 时仍拒绝。
- C_new 达标而 C_old 只一票时仍拒绝。
- 越界 bit 直接 fail-closed。
- multi-node replacement 与错误 proposal kind 在 transaction begin 前被拒绝且无写回。

Windows GCC Full/Lite/Nano 与 WSL Full ASan/UBSan CTest 均为 `8/8` 通过；`git diff --check` 无空白错误，仅已有 CRLF 提示。

## 结论

`CLV2-07-05`：**CODE COMPLETE / SELF-AUDIT PASS**。这是值层 quorum 计算，不是 Head Authority、wire certificate 或 CONFIG_COMMIT 许可；上述均仍后置到 M08/M10/后续 M07 persistence owner，M05 顶层 `AUDIT HOLD` 不变。
