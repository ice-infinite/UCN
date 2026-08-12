# UCN 测试地图

本轮保留测试文件的平铺路径，避免目录迁移与测试行为修改同时发生。分类、目标和覆盖边界见 [../docs/架构/04-构建目标与测试地图.md](../docs/架构/04-构建目标与测试地图.md)。

| 测试组 | 代表文件 | 覆盖内容 |
| --- | --- | --- |
| Foundation/Frame | test_core.c、test_frame.c、test_wire_profile.c、test_endpoint.c、test_link_cost.c | 配置、Frame、Wire、Endpoint、LC-1 C01～C10 |
| Node/Neighbor | test_node.c、test_hello_join.c、test_neighbor_*.c、test_duplicate_window.c | 生命周期、准入、邻居与去重 |
| Transport/Port | test_adapter*.c、test_standard_adapter.c、test_protocol_owner.c | Adapter 队列、Preset、六类 Port API |
| Routing/Policy | test_aodv_lite.c、test_route.c、test_candidate_route.c、test_neighbor_bearer.c、test_path_control.c、test_policy*.c | 自动寻路、指定路径、动态 Bearer/Candidate、策略与 Q1 Flow |
| Service | test_service.c、test_service_bridge.c | 本机任务 Service 与跨 MCU Bridge |
| 综合/压力 | test_integration.c、test_stress.c、test_dynamic_stress.c、tools/ucn_scale_sim.c | 多模块联动、固定资源边界、Host 拓扑模拟 |
| 配置合同 | test_config_contract.c、config/ucn_test_user_config.h | 全局公共配置覆盖、回退、非法组合 |

CMakeLists.txt 是测试实际选入规则的唯一权威；此表只用于导航。
