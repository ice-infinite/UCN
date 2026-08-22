# CLV2-07-01：Config State 值模型自审报告

日期：2026-08-23  
状态：**CODE COMPLETE / SELF-AUDIT PASS；外审并入 M07 final。**

## 实现

- 新增独立公共值模型 `ucn_cluster_config_state_t`：`config_id`、`phase`、`C_old`、`C_new`、两个 canonical voter-set hash。
- `STABLE` 强制 `old_set == new_set == config_id`；`JOINT` 强制 `old_set.config_id + 1 == new_set.config_id == state.config_id`。
- 所有集合均复用 M06 的有界、升序、无重复 voter set；Config ID 只能在 no-wrap serial 域内前进。
- 提供纯 `init_stable`、`init_joint`、`promote_joint`、state hash 与固定长度 canonical serialization。所有失败路径不写 output。

## 自审边界

模块不引用 Wire v4、RX/TX、Cluster FSM、Authority、Adapter 或 Runtime member table。它只链接为 production `ucn_cluster` archive 的值模型；独立 `ucn_cluster_config_state_tests` 不携带 test bridge 或 v4 encoder 定义。

## 定向验证

- 无序 voter 输入生成相同的 Stable set、hash 和逐字节 serialization；
- `C_old={1,4,9}` → `C_new={1,4,9,21}` 的 Joint state 严格绑定 `41 → 42`，promote 后 Stable 两个 set 均为 `42`；
- `config_id==0`、rotation threshold 上再建 Joint、hash 篡改和 serialize invalid state 都失败且 output 不变；
- public-header 目标可独立包含该 header。

| 门禁 | 结果 |
|---|---|
| Windows GCC Full/Lite/Nano | 各 `5/5` PASS |
| WSL ASan/UBSan | `5/5` PASS |
| production v4/FSM/Authority 扫描 | 新模块无调用；仅既有 M05 codec test target 含 encoder 宏 |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

## 限制

07-01 没有创建 `config_tx`、改变 quorum、持久化 Config、发送 CONFIG_* 或修改 member status。07-02 才建立一次一个的 bounded transaction；M05 顶层继续 `AUDIT HOLD`。
