# 公共 API 符号表

| 头文件族 | 主要前缀 | 内容 |
| --- | --- | --- |
| `ucn.h/types/config/profile/time` | `ucn_` / `UCN_` | Core 基础合同 |
| `ucn_frame.h` | `ucn_frame_` | Wire encode/decode |
| `ucn_node.h` | `ucn_node_` | Node、send/RX/step、诊断 |
| `ucn_link.h` | `ucn_link_` | Link ops/metrics |
| route/path/policy/cost | `ucn_route_`、`ucn_path_`、`ucn_policy_` | 路由和选路 |
| adapter/standard adapter | `ucn_adapter_` | frame queue/preset/runtime |
| ports/sources | `ucn_port_`、`ucn_stream_source_`、`ucn_can_source_` | 平台与介质 |
| service/transfer/security | `ucn_service_`、`ucn_transfer_`、`ucn_security_` | 可选服务 |
| cluster | `ucn_cluster_` | Current 与受限组件 |

完整声明直接检索 `include/ucn/**/*.h`。本表只提供导航，不复制约 900 个声明。
