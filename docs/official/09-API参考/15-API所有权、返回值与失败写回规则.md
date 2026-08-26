# API 所有权、返回值与失败写回规则

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

## 所有权

- `init` 接收的 storage、ops、binding 和 buffer 默认由调用者长期持有；
- API 明确复制的配置才可在返回后释放；
- RX completion handle 在 release 前由组件拥有；
- Provider 异步请求必须由 Provider 复制。

### 常见对象分类

| 形态 | 所有者 | 生命周期 |
| --- | --- | --- |
| `ucn_node_t`/Router/Transfer/Runtime | 调用者提供 storage，组件管理内容 | 从 init 到停止网络 |
| `ops`/config table/binding table | 通常借用 | 必须覆盖对象整个生命周期 |
| send payload | Node/Service 可能复制，Transfer 大消息借用 | 以具体 API 合同为准 |
| RX frame view/payload | 接收栈临时拥有 | handler 返回前复制所需内容 |
| Transfer RX handle | Transfer slot 拥有 | 应用 release 或 hold timeout |
| `get_stats()`/`find_*()` const 指针 | 组件内部 | 下一次状态推进前只读使用 |

不能仅凭参数是 `const` 判断是否复制。`const` 只表示组件不修改该内存，可能仍长期借用。

## 上下文

状态机 API 只由 Protocol Owner 调用。ISR 入口仅允许文档标明的 `*_from_isr`/push/notify；应用 handler 不应递归调用 Owner 状态机。

建议为每个 API 在产品封装中标注：INIT、OWNER、ISR、APP-TASK、CALLBACK 哪些上下文允许。跨任务调用通过 Service/command queue 投递给 Owner，不能只加一个 mutex 就让任意任务操作 Node——callback 重入、状态机顺序和持久化 continuation 仍可能被破坏。

### 回调规则

- Link send/Provider/Service validator 必须有界；
- 回调不得销毁正在调用它的对象；
- Provider `load/submit/poll` 不得递归 Cluster/Config owner；实现会 fail-closed，但产品仍应避免；
- Endpoint/Transfer receive handler 应复制或消费数据，不长期保存临时指针；
- completion 可能在 Owner 上同步触发，应用不得假设一定来自另一个任务。

## 失败写回

验证输入和容量后再写 output。返回错误时 output、事务和已提交状态保持不变，除非接口明确说明已进入可观察的 partial/pending 状态。

测试 output 不写回应保存整个对象/哨兵副本并 `memcmp`，不能只检查首尾字节。状态对象的原子失败同样应逐字节或逐字段验证。

`PENDING` 不是普通错误：它表示组件已经接管 operation，调用者不能重复构造另一个请求；后续由 `poll/step` 完成。相同 operation ID+相同 fingerprint 可幂等重放，相同 ID+不同状态必须拒绝。

## 完成语义

本地 `UCN_OK` 可能只表示已入队、已接受或已 durable；远端送达、解密、服务执行和业务确认是不同层级，调用者应选择相应 completion API。

| 层级 | 可能的证据 | 不能证明 |
| --- | --- | --- |
| API accepted | `send()==UCN_OK` | Link 已发送 |
| Link queue accepted | outbound event | 远端收到 |
| Core delivered | Endpoint handler | 业务执行完成 |
| Transfer delivered | complete reassembly ACK | 应用已执行 |
| Service remote inboxed | Result stage | 命令成功 |
| Service remote executed | terminal Result | 物理动作绝对安全/正确 |
| Persistence committed | reload+journal match | 对端收到 ACK |

## 初始化、停机与重配

初始化只在对象未运行时完成；运行中重配必须使用公开 setter/transaction。停机顺序一般为：停止新业务→停 Driver IRQ/DMA→唤醒 Owner drain/取消事务→关闭 Link→导出 stats→释放上层对象。静态 storage 虽不需要 free，也不能在回调仍可能到达时复用。

## API 兼容

新增公共结构字段会影响位置初始化和 `-Wmissing-field-initializers -Werror`；预发布阶段可做破坏性升级，但必须 bump API/size contract、更新所有 Port/应用并增加旧用法门禁。正式发布后则需要明确的扩展结构、版本字段或迁移层。
