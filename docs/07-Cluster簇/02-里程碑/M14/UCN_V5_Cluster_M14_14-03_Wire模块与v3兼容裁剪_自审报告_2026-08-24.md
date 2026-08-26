# UCN V5 Cluster M14 / 14-03 Wire 模块与 v3 兼容裁剪自审报告

> 日期：2026-08-24  
> 状态：`MODULE SPLIT COMPLETE / SELF-AUDIT PASS / PRODUCTION SWITCH BLOCKED BY M05`

## 1. 已完成的软件范围

- `ucn_cluster_wire_v4` 是严格、独立的 RFC4 codec archive；推荐 Wire format 定义为 v4，但该推荐不启用生产 encoder、RX、FSM 或 Authority。
- `ucn_cluster_wire_v3_compat` 由 `UCN_CLUSTER_ENABLE_V3_COMPAT` 显式控制；关闭时不生成 v3 archive。
- 双格式检测/分派从 v4 codec 中移出，成为独立 `ucn_cluster_wire_dual_stack`。它只在 v3 compatibility 打开时生成并显式链接 v3/v4 两个 codec。
- Current Cluster FSM 仍使用冻结的 v3 生产协议。关闭 v3 后 Current FSM 明确不具备可链接的生产 Wire 路径；在 M05 外审解除前不以“推荐 v4”为理由偷偷接通 v4 生产路径。

## 2. 自审发现与整改

初次拆分仍把 `ucn_cluster_wire_decode()` 与 strict-v4 codec 编译在同一对象中。即使产品只链接 v4 archive，也可能因为该对象内的 `ucn_cluster_message_decode()` 引用而引入 v3 未解析依赖。

整改后：

- v4 codec 源码不再引用 `ucn_cluster_message_decode()`；
- v3-OFF 的 `libucn_cluster_wire_v4.a` 未解析符号表为空；
- dual-stack dispatcher 独立编译，仅在明确兼容构建中存在；
- source gate 固定上述模块边界，防止后续重新耦合。

## 3. 构建与测试

| 配置 | 结果 |
|---|---:|
| GCC Full + M10/M11/M13 实验 + O1/O2/O3 Wire gate | 47/47 |
| GCC Lite | 41/41 |
| GCC Nano | 31/31 |
| GCC Full / Service OFF | 41/41 |
| v3 OFF Release，仅构建 strict v4 archive | PASS，16110 B |
| v3 ON Release，v3/v4/dual 三 archive | PASS，4348/16110/1656 B |
| v3-OFF strict-v4 archive undefined symbols | none |
| Wire module source gate | PASS |

archive 字节数是 Windows Host GCC 静态库观测，不是 MCU Flash 占用。

## 4. 尚未完成的生产切换

14-03 不能标记为完整 `DONE`：M05 顶层仍处于 `AUDIT HOLD`，因此 production v4 RX/TX/FSM、Authority、默认 encoder 仍不得启用。当前只完成“可以物理裁剪/独立链接”的模块基础，生产 strict-v4 切换等待 M05 放行。

## 5. 结论

14-03 模块拆分范围自审通过；任务状态保持 `PARTIAL / BLOCKED BY M05`。该阻断不妨碍继续实施与生产 Wire 接线无关的 14-04 不变量引擎。
