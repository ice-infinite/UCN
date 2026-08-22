# CLV2-06-08 Capacity Semantics 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 容量合同

| 项目 | 定义 | 计数对象 | M06 行为 |
|---|---|---|---|
| Runtime capacity | `member_capacity` | `primary_members` 的 occupied remote slot | provisional 与 committed 都占用 |
| Voter capacity | `voter_capacity`，**包含 Head** | canonical `active_voter_set` | provisional 不占用；真正 Config Commit 后才可占用 |

- `voter_capacity=0` 仅对 head-capable config 使用默认 `member_capacity + 1`；非 Head 的非零 voter capacity 配置会 fail-closed。
- `ucn_cluster_get_member_capacity_view()` 只读返回两种 capacity/used/available；没有 active voter set 时 voter used=0。
- post-validation provisional admission 可返回明确的 `ARGUMENT / NOT_HEAD / RUNTIME_CAPACITY / MEMBER_CONFLICT` 原因。
- `cluster_preflight_provisional_voter_commit()` 只是 M07 future-owner 的无副作用预检：检查 v4 provisional、canonical active set 与 voter capacity，报告 `VOTER_CAPACITY` 或 `CONFIG_UNAVAILABLE`；它绝不修改 status、voting 或 voter set。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full -R "ucn_tests|ucn_cluster_membership_model_tests" --output-on-failure
  ucn_tests                              PASS
  ucn_cluster_membership_model_tests     PASS
```

模型建立 Runtime=1、Voter=2（`{Head=2, voter=7}`）后接纳 provisional node 9：Runtime 变满而 active voter set 不变；第二个 provisional 得到 `RUNTIME_CAPACITY`。对 node 9 的 future-commit preflight 得到 `VOTER_CAPACITY`；将 Voter cap 提高到 3 后预检通过，但 node 9 仍为 non-voting、voter set 仍不变。只读 capacity view 的 NULL 错误路径保持 caller 哨兵不写回。

## 边界核对

本项没有 `active_voter_set` 写入者、没有 `voting=true` 的 Config Commit 路径，也没有 production v4 RX/TX/FSM/Adapter 引用。M07 才负责真实 voter set transaction，M10 才负责 quorum/certificate Authority。
