# 配置、Profile 与对象初始化

## 1. 选 Profile

Nano 适合容量很小的端点，Lite 适合常规控制器，Full 适合网关/簇头。选择依据是需要保存的
Peer、Route、并发 Transfer 和消息上限，不是权限。低档节点仍能接收/解析高档节点的 v6
控制与数据帧；对端通过 Capability 得知其容量。

## 2. 产品配置头

在一个头中定义需要覆盖的 `UCN_V6_CONFIG_*`，再配置：

```powershell
-DUCN_USER_CONFIG_HEADER="my_product_ucn_config.h"
-DUCN_USER_CONFIG_INCLUDE_DIR="<配置头目录>"
```

同一固件的所有 Translation Unit 必须使用同一头。不要在 `.c` 文件前临时 `#define` 容量；
Manifest mismatch 会阻止初始化，绕开检查则属于未支持用法。

## 3. 静态分配

在全局或静态区声明所需 Storage，避免在任务栈一次分配大对象。只为启用的 Feature 和实际
角色分配对象。例如不使用 Cluster 的端点既关闭 CMake Feature，也不声明 Cluster Storage。

每个 `*_init_in_place()` 接收 Storage 地址、容量、Manifest/配置及 callback。先初始化 shared
gate 和 Provider，再初始化 Owner。任何初始化失败都应让产品保持 network-not-ready。

## 4. 建议顺序

Manifest → callback gate → Identity/Bootstrap → Owner → Security → Capability → Route →
Metric/QoS → Transfer → Optional Realtime/Cluster → Adapter/Port → Link ready。

持久化 Provider 必须先 load 并校验，再允许发出会形成承诺的消息。异步 PENDING 时保留完整
operation 和 Fence；不能因为重启或超时假定写入成功。

## 5. 固件证据

每个产品构建记录 Profile、Feature bits、Layout Hash、全部非默认容量、编译器、链接 map 和
固件 SHA-256。资源报告是 Host 上界，不替代目标板 BSS、栈 high-water 和运行峰值。
