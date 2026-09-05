# UCN 用户手册

这套手册面向“要把 UCN 用进产品”的开发者。它不要求先理解路由状态机、Frame 编解码实现或源码内部 helper，只围绕下面的问题展开：

- 我该选择 Nano、Lite 还是 Full？
- 节点、Link、Endpoint 和运行循环怎么初始化？
- 怎样发送一条消息、接收一类数据并正确理解返回值？
- UART、CAN、Wi-Fi、USB 等驱动怎样接到统一接口？
- 怎样使用自动路由、指定路径、负载均衡、Service、Transfer 和安全能力？
- 出现 `NOT_FOUND`、`NO_SPACE`、`LINK_DOWN` 或超时时怎样排查？

> 当前适用版本：UCN 5.0.0 / Core Wire v5。代码和公共头文件是最终事实源。本手册会明确区分“库已实现”“需要产品接驱动”“仅 Host 软件验证”和“仍处于受限实验阶段”。

## 推荐阅读路线

### 第一次接入，只想尽快收发数据

1. [00-先读这里：五分钟快速开始](00-先读这里-五分钟快速开始.md)
2. [01-选择构建档次与功能](01-选择构建档次与功能.md)
3. [02-初始化节点与运行 Protocol Owner](02-初始化节点与运行Protocol-Owner.md)
4. [03-定义 Endpoint 与收发消息](03-定义Endpoint与收发消息.md)
5. [04-接入 UART、CAN、Wi-Fi 等通信介质](04-接入UART-CAN-WiFi等通信介质.md)

读完这五篇，应能完成一个固定两节点或一跳直连产品。

### 需要自动组网或多链路能力

6. [05-自动入网与自动路由](05-自动入网与自动路由.md)
7. [06-多链路、指定路径与负载均衡](06-多链路-指定路径与负载均衡.md)

读完以后，应能区分直连 Link、Neighbor、Route、Path 和 Policy，并知道什么时候使用自动路由、固定主链或 Q1 负载均衡。

### 需要任务通信、大消息或安全

8. [07-任务间与跨 MCU Service 通信](07-任务间与跨MCU-Service通信.md)
9. [08-32 B 到 8 KiB Transfer](08-32B到8KiB-Transfer.md)
10. [09-安全 Provider 与受保护通信](09-安全Provider与受保护通信.md)

### 准备做产品化和现场运维

11. [10-裸机与 RTOS 接入](10-裸机与RTOS接入.md)
12. [11-诊断、状态查询与故障排查](11-诊断-状态查询与故障排查.md)
13. [12-资源、性能与参数调优](12-资源-性能与参数调优.md)
14. [13-完整产品场景教程](13-完整产品场景教程.md)
15. [14-API 与错误码速查](14-API与错误码速查.md)
16. [15-Cluster 当前使用边界](15-Cluster当前使用边界.md)

## 三条最重要的使用原则

1. **一个 Node 只有一个 Protocol Owner。** Driver ISR 只投递数据和通知，业务任务不并发操作 `ucn_node_t`。
2. **`UCN_OK` 只说明当前 API 层接受成功。** 它不自动证明物理发送完成、远端收到或远端任务执行成功。
3. **UCN 不替产品初始化外设。** UART 引脚和波特率、CAN bit timing、Wi-Fi peer/Socket、USB 枚举都由 BSP/SDK 完成；UCN统一的是上层地址、消息和选路语义。

## 功能成熟度速览

| 功能 | 用户可以怎样使用 | 仍需产品完成 |
| --- | --- | --- |
| Core、Endpoint、固定 Link/Route | 默认可用 | 真实 Driver、引脚和队列 |
| Lite/Full 自动 Mesh | 已有协议实现和软件测试 | 真实介质的发现、压力和断链验收 |
| Full Path/Policy/负载均衡 | 已有 API 和软件测试 | 产品授权、路径规划、实机调参 |
| Service | Service 开关开启时可用 | 任务队列、锁、业务 Result 语义 |
| Transfer | 单独链接 `ucn_transfer` | 静态 RAM、窗口、目标链路压力测试 |
| Security | Lite/Full 有 Provider 合同 | 生产密钥、AEAD、RNG、持久化和安全审计 |
| Cluster | 独立 Archive | 当前仍有发布阻断，普通产品不要默认启用 |

## 与其他文档的关系

- 本目录：告诉用户“怎样调用和集成”；
- [`official/`](../official/README.md)：保存正式架构、协议、API 和兼容性事实；
- [`源码阅读指南/`](../源码阅读指南/README.md)：面向阅读内核和人工审计；
- [`calltree/`](../calltree/README.md)：面向函数调用导航；
- [`evidence/`](../evidence/README.md)：保存与提交、测试和实机条件绑定的证据。

遇到冲突时，以当前公共头、CMake、源码和测试为准，再修正文档。
