# UCN V5 Cluster M05-02 隔离 Codec 自审报告

- 日期：2026-08-21
- 范围：`CLV2-05-02`、`CLV2-05-R06` 与 `CLV2-05-R06-B`
- 结论：**外部复审 GO（仅 CLV2-05-02）；M05 整体仍 AUDIT HOLD**

## 1. 范围与禁止项

本轮只落实 RFC4 的字节级编解码边界：精确的 `32 B/v3`、`40 B/v4` 分派，v4 的 Type `1..33` 结构检查，以及无动态分配的 Certificate-pending codec helper。

复扫确认没有改动以下生产语义：

- Core `W0..W3` 与 `UCN_PROTOCOL_VERSION`；
- 既有 `UCN_CLUSTER_FORMAT_VERSION=3`、`UCN_CLUSTER_MESSAGE_BYTES=32`；
- `src/extended/ucn_cluster.c` 的 v3 收发、Authority、持久化或 FSM；
- M05-04+ 的 Capability/混合版本准入、40 B Adapter 迁移、quorum/CRC 业务验证，以及 M07/M10/M11/M13。后续 05-03 的 private semantic builder 不改变本报告已经签署的 raw codec/production 隔离结论。

## 2. 实现核对

| 合同 | 实现 | 自审结果 |
|---|---|---|
| 格式唯一分派 | `ucn_cluster_wire_detect_format()` 只接受 `(32,3)` 或 `(40,4)` | PASS；没有 fallback decoder。 |
| v3 保留 | 泛型 decoder 的 v3 分支调用既有 `ucn_cluster_message_decode()` | PASS；现有 v3 路径未替换。 |
| v4 严格解析 | 独立 raw frame、网络序 6 × u32；Type/role/flags/零字段/ID/serial/duration/capability 结构校验 | PASS；失败时 output 不写回。 |
| READY 模式角色 | `target_cluster_id == header.cluster_id` 时只允许 BACKUP；否则只允许 HEAD | PASS；同/跨 Cluster 负例均覆盖。 |
| Encoder 关闭 | 正常 archive 编译开关为 0，encoder 返回 `UCN_ERR_CONFIG` 且不写 buffer | PASS；只有独立测试 executable 以开关 1 复现 golden bytes。 |
| Certificate pending | 固定一个 helper slot、Type 8 先建、满载异 key 拒绝、1000 ms、Epoch change reset | PASS；不持有 Authority/FSM 回调。 |
| R06 admission 前置 | `begin/fragment` 都要求 caller 传入 source 与 frozen Config 已准入的 receiver-side context；Stable/Joint 分别绑定 `C_old/C_new` | 外部复审 GO。 |
| R06-B deadline 边界 | Type33 的 admission/key/Config 无副作用核验先于 lazy expiry；公开 `pending_expire()` 保留为 timer-owner 回收入口 | 外部复审 GO。 |

首轮自审发现并修正一项代码遗漏：Type 12 `BACKUP_MEMBER_SYNC` 在 RFC4 中是 HEAD role，初始 role whitelist 漏列它，导致规范 vector 被拒绝。修复后再跑全矩阵。

## 3. 测试与证据

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `28/28` CTest PASS |
| Windows GCC Lite | `28/28` CTest PASS |
| Windows GCC Nano | `18/18` CTest PASS |
| WSL Full ASan/UBSan | `18/18` CTest PASS |
| WSL Full GCC `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest PASS |
| 空白检查 | `git diff --check` PASS（仅 Git 既有 CRLF 提示） |

`ucn_cluster_wire_v4_codec_tests` 覆盖：

1. Type `1..33` 的 test-encoder → decoder round trip；
2. 9 条 RFC4 正向 vector 的 decode 后逐字节重新编码比对；
3. v3 与 v4 唯一分派；
4. 错误 length/version/type/role/flags/reserved 字段的 fail-closed 与 output 不变；
5. 同 Cluster READY=HEAD、跨 Cluster READY=BACKUP 两条角色错误负例；
6. fragment 先到、slot 满、异 key、不一致重复片、deadline、active Epoch change；
7. source mismatch、未获 frozen-Config admission、Stable Config mismatch、Joint `C_old/C_new` set mismatch 均拒绝且 slot 不变；前三项在 `now == deadline` 仍断言 slot、deadline、fragment mask 不变，只有显式 `pending_expire()` 才释放 slot。

生产 `ucn_tests` 还覆盖了 default-disabled encoder，确保普通 `ucn_cluster` archive 不能发出 v4。

## 4. R06/R06-B 外审 P1 与整改

外审指出初版 helper 的公开 API 只有 raw frame 和时间，因此它无法证明 Type 8/33 已通过 outer source 与 frozen Config 准入。该结论成立：初版会让任意结构合法的 raw Type 8 创建 slot。

整改后的 API 要求 `ucn_cluster_wire_v4_certificate_admission_t`，它不是 wire 数据，包含经 RX gate 验证的 outer source、source/frozen-Config admission 标志、以及 frozen `C_old/C_new` 身份。helper 会拒绝没有该证明、source 不等于 Common Header Head、Stable anchor 错配、Joint anchor 错配，或 OLD/NEW fragment 与冻结 Config set 错配的请求。拒绝发生在 slot 写入前，且不会更改已有 slot 或 deadline。

外审随后指出一个有效的 deadline 边界：初版 `pending_accept_fragment()` 在比较 admission/key/Config 前先执行 lazy expiry，因此未准入分片在 `now_ms == deadline_ms` 能把 slot 清空。现已将 expiry 移到所有无副作用的 admission、key、set/config 匹配之后。故未准入分片在 deadline 边界仍只会收到拒绝；真正的 timer 回收由未来 owner 直接调用 `ucn_cluster_wire_v4_pending_expire()`。三种边界反例和显式回收均已回归。

该类型是“调用方必须显式提供准入结论”的接口合同，不是新的安全凭据，也不替代真实 transport/security/source/replay 或 Frozen ConfigState 验证；那些生产 RX gate/FSM 仍未授权实现。

## 5. 剩余边界

本报告不是 v4 可上线的声明。外部复审已对 **CLV2-05-02 的隔离范围**签署 GO，允许按任务表进入受限的 05-03；但 Certificate helper 当前仍是独立状态对象，后续获授权的 v4 RX owner 才能以每 Cluster 一个实例方式持有它。在此前它不会接收真实帧、更不会建立 Authority；生产接线、quorum/CRC/FSM、M07/M10/M11/M13 与实机门禁仍未签署，M05 整体继续 AUDIT HOLD。
