# Wire、API、ABI、Storage 兼容规则

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

- Wire：线上字节是否能被双方严格解析；
- API：源代码调用是否仍可编译；
- ABI：已编译对象的布局、符号和调用约定是否兼容；
- Storage：Flash/静态 opaque storage 是否能被新旧代码恢复。

任一层破坏都必须在 CHANGELOG 标注。预发布阶段允许破坏性升级，但必须全量重编译、刷新配置，并定义持久化迁移/擦除步骤。

## 四层为什么独立

| 层 | 兼容示例 | 不兼容示例 |
| --- | --- | --- |
| Wire | 新节点仍严格解析旧帧 | 固定payload由8 B改11 B且版本不变 |
| API | 旧源码用新头仍能编译 | 函数参数/枚举语义改变 |
| ABI | 旧对象与新库布局/调用一致 | 公共struct追加字段、宏改数组长度 |
| Storage | 新固件能恢复旧Record | writer升级后旧固件误读/丢Tombstone |

可能出现Wire兼容但ABI不兼容：网络节点仍能通信，但应用必须重编译。也可能API兼容但Storage不兼容：代码能编译，升级后旧Flash无法启动。

## 版本声明模板

发布清单应分别写：

```text
Project: 5.x.y
Core Wire: v5 / W0-W3
Public API: Node Storage/API ...; Port API v2
Cluster Current Wire/API: v3 / API v2
Cluster experimental Wire: v4 codec, encoder default OFF
Persistence Record writer: v4/388 B; readable: v1/v2/v3/v4
```

不能只写“UCN v5兼容”。

## 破坏性升级判断

以下变化默认视为破坏：线上字段长度/含义、CRC/AAD、枚举编号、公共函数签名、struct size/layout、配置宏改变ABI、Record schema/事务恢复语义。即使项目尚未发布，也要记录原因和迁移，防止测试板混用。

## 兼容实现方式

- Wire：提升version/format，strict双分派，能力协商；
- API：新增带size/version的扩展config，保留wrapper或明确重编译；
- ABI：opaque handle/storage版本，避免应用读私有布局；
- Storage：新schema双槽、旧decode→canonical migrate、向下回滚工具。

兼容层不得猜测malformed输入，也不得把未知高级语义降级为普通数据。

## 检查方法

Wire用golden/mixed-version；API用旧调用源码严格编译；ABI用sizeof/symbol/旧object链接（若承诺）；Storage用旧真实record、升级、重启、回滚/拒绝和掉电测试。

## 当前发布边界

当前处于预发布优化阶段，可以选择破坏性收益，但所有板卡/应用必须同步更新。Cluster v4仍未生产接线，因此不能把codec存在解释成已形成稳定兼容承诺。
