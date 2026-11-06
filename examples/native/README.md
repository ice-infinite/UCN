# 原生接口首轮：最小发送、静态发布订阅与只读健康消息

这是可以编译运行的**首轮静态后端**，不是全部API方案或`rmw_ucn`的完成声明。
C99、C++17、Rust复用各自真实Core/Adapter。示例完全软件回环，不打开串口、无线设备或执行器。

## 1. 先选正确入口

| 需要 | 入口 | 本轮边界 |
| --- | --- | --- |
| 最小点对点 | C `ucn_send(core,target,bytes,len,NULL,NULL)`；Rust `CoreNode::send` | 复制、BestEffort、OneWay、静态Path；C默认Q2/TTL1 |
| 普通业务发布订阅 | C `ucn_publish(native,publisher,bytes,len)`；C++ `Publisher<Codec>`；Rust `Publisher<Message>` | 预配置通道/Schema、固定fanout、独立应用执行器 |
| Callback中点对点 | C `ucn_native_send`；Rust `Inbox::send` | 只向通道已登记的目标复制deferred命令 |
| 认证、加密、可靠传输、动态发现/路由、RPC、严格源年龄、ROS | 后续后端/阶段 | **没有在此接口闭环；不得把O0静默当作这些能力** |

本轮只有产品明确声明的隔离可信静态网络：`C1/O0/H0/BestEffort/OneWay`。
`OriginAuth`、`Encrypted`的Native配置返回`Unsupported`；它们不是实现未准备好时的降级开关。
`STATIC_TRUSTED`及`principal_digest`只表示精确产品配置绑定，**不是密码认证事实**。
已有高级Owner/crate没有被删除，也没有因新增外观就自动接入它。

## 2. 编译和运行

Windows选择确实为ASCII的staging目录，不依赖用户名或`TEMP`一定为ASCII：

```powershell
cmake -S . -B build-native -G Ninja -DUCN_FEATURE_NATIVE=ON -DUCN_ASCII_STAGING_ROOT=E:/ucn-staging
cmake --build build-native
ctest --test-dir build-native -R ucn_native --output-on-failure
```

Linux可省略staging参数。`UCN_FEATURE_NATIVE`默认OFF：不生成/安装`UCN::native`、Native/C++头，
也不在Core对象中增加通道、Publisher或Subscription表。ON时只需链接`UCN::native`，其唯一
协议依赖是公共`UCN::simplified`，不直接调用私有Security、Route、Persistence或Coordinator。

```powershell
cd rust
cargo test -p ucn-native --locked
cargo run -p ucn-native --example static_health
```

Rust库为`no_std + forbid(unsafe_code)`，无`alloc`。Host示例为了创建永久借用的模拟对象使用
`Box::leak`；这是测试装配，不是库的堆依赖。裸机产品用自己的永久静态存储装配。

## 3. 装配一次，再只处理业务

1. 产品Port初始化唯一Core、静态身份/Binding和真实Link/Path。
2. 配置通道，C用全零`ucn_native_storage_t`调用`ucn_native_init`；Rust用永久静态Backend和Inbox
   调用`NativeRoot::new`。初始化必须独占；Core Endpoint从未使用过。同Service的多Schema/别名
   本轮一律拒绝，不自动猜测。
3. 创建Publisher/Subscription，启动一次。
4. 协议任务有界调用`native_step`；应用执行器单独有界调用`native_dispatch`。
5. 新业务只负责编码和publish；回调只读消息，并可复制提交下一条业务或close。

挂接后Facade独占Core的推进与控制；除Driver ingress和只读查询外，不要再从别的任务直接
`start/step/stop/deinit/send/endpoint_add`同一Core。最小裸Core用户不挂接Facade，仍可独立使用。
这是一条C调用所有权合同，不宣称能阻止调用方原始内存覆盖。Rust永久借用进一步由类型系统约束。

### 通道的每一项配置

| 字段 | 用途/约束 |
| --- | --- |
| local channel ID | 本机查表，不逐帧发送名称 |
| Service ID | 复用既有C1 Service字段，静态分派键 |
| schema ID/revision/bytes | 两端产品一致；本轮固定长度1～64 B，typed封装精确匹配 |
| traffic class | 应用仅Q1～Q3，不能借此抢占协议Q0 |
| queue policy | FIFO按精确来源流保持顺序；KeepLatest只替换同channel/source/locality |
| source list | 精确address/binding/configured principal，不可区分的同地址别名拒绝 |
| target list | 固定远端集合；本机订阅隐式交付，不把本机再配置成远端Target |
| max local age | 本地RX/接纳到处理的半开寿命，不是跨节点采样年龄或时钟同步 |
| security minimum | 本轮只允许显式StaticTrusted，其余保护要求拒绝 |

静态Schema ID本轮没有上Wire，因此产品必须确保同Service的两端配置一致；接收方不能凭长度
证明一个未认证发送者使用了正确Schema。动态Schema协商和认证能力仍属后续阶段。
64 B是业务池上限，不是每条Link的Payload保证；完整C1头加正文仍必须适配真实Path MTU。

## 4. C/C++/Rust业务侧怎样写

产品初始化可直接参照[完整C三节点例子](static_health.c)。下面只省略已经装配好的Root/句柄：

```c
uint8_t bytes[16];
ucn_health_encode(&health, bytes, sizeof(bytes));
ucn_result_t r = ucn_publish(native, health_publisher, bytes, sizeof(bytes));
/* r==UCN_OK: bytes已复制，可立即复用；不代表远端收到，更不代表设备动作。 */
```

C++的Codec提供`message_type/schema_id/revision/wire_size/encode/decode`，不使用`std::function`：

```cpp
ucn::Publisher<HealthCodec> publisher;
publisher.open(native, HealthChannel);
publisher.publish(health);
// Subscription<HealthCodec>::open(native, channel, callback, context)
```

Codec/typed调用的完整可运行实现见[C++例子和回归](../../tests/simplified/test_native_cpp.cpp)。
包装对象禁止复制/移动，open/close/publish由一个应用所有者串行调用；协议step可以在其他任务。
注册前已经准备好callback/context；普通析构会close。若析构遇到平台锁故障，或订阅回调仍在
执行，默认`std::terminate`而不是返回后留下悬空`this`；不隐式等待。嵌入式产品可统一设置
terminate诊断策略。常规做法是固定地址保存实体，先显式close，再确认quiescent/release。

Rust实现业务`Message`，再创建`Publisher<M>`；订阅用永久`SampleHandler`，其中解码产品正文：

```rust,ignore
let publisher = Publisher::<Health>::new(root.commands(), HealthChannel)?;
publisher.publish(&health)?;
root.step(now_us, 8)?;       // 协议推进
root.dispatch(now_us, 8)?;   // 锁外应用分发
```

详细装配和真实软件Driver见[Rust三节点测试/示例共享装配](../../rust/crates/ucn-native/tests/static_pubsub.rs)。
Inbox有意为`!Sync`，本轮只支持单任务Actor；不能自行unsafe包装为跨线程对象。
Root/Backend/Driver/Inbox永久借用，`Drop`或`mem::forget`都不能归还仍被物理I/O引用的存储。

## 5. 排队、回调和退休，不要混淆

- publish成功只代表本地固定池接纳；随后按原目标集、原期限逐目标交给Core。fanout可以部分
  成功，不承诺分布式原子性。父命令按同channel接纳次序推进，不因等待某个目标物理完成而
  阻塞所有其余目标；真实Token/帧副本由Core独占。
- 队列满/参数错误不消耗Core Origin Sequence。`NOT_SUBMITTED`由原Core按原帧/序号重试。
- close是独立latch，不依赖数据队列空位；允许回调自关闭。已经复制的发布命令不因Publisher
  close而撤回。Subscription close清未开始回调，活动context要等quiescent才能释放。
- SourceFence永久屏蔽该精确来源，清排队接收事件，并阻断尚未交给Core的本机命令；**不能追认
  撤回已经Core接纳的物理发送**。重开订阅不清来源高水位，不复活旧Latest。
- FIFO多订阅也必须让每个订阅先收到旧样本；KeepLatest不合并不同来源，不让Replay窗口内
  未见过的旧序号覆盖新值。轮转游标跨step/dispatch保留。
- IRQ可能在step捕获`now`之后到来。样本保留原RX时间，寿命锚点取`min(RX time, admission now)`；
  不在RX时刻之前交付，也不凭未来时间延长排队寿命。到达边界即过期。
- stop围栏新业务并清应用事件，继续step，直到Core真实quiescent；未start的Root也能关闭。
  `Unknown`不能靠清Root、超时、Drop或数字改名伪造退休。
- C后端对Unknown保守要求Driver的旧实例物理Fence事实；产品只能在旧硬件确实排空/失效后
  发布`LINK_REOPENED`，不是简单递增软件变量。本例只模拟这条事实，不证明真实硬件。
  Rust新增非冲突Unknown的精确迟到completion排空；矛盾完成仍隔离，Fault排空后禁止重新start。

### 统计单位

C Native `accepted`=接纳的父Job数，`core_admitted`=逐目标Core接纳次数，`failed`=未交给Core的
目标失败次数，`delivered`=逐订阅原始callback次数。`dropped`只记部分接收/到期拒绝，不包括
所有close、stop或Latest替换。它们**不是同一数据漏斗，不能直接做差计算网络丢包**。
链路实际完成/失败另查Core统计；远端业务成功需要后续协议/业务ACK，不能从这些数字推断。

## 6. 费用、容量和还未完成的部分

健康正文为16 B：电压u32、电流i32、温度i16、模式u8、flags u8、故障u32，全部显式big-endian。
本例A0 `C1/O0/H0`普通头11 B，总27 B；Topic名、Schema、Publisher ID、Context、回调句柄均未新增
到Wire。A发B/C是两次27 B发送，不把fanout总费用说成一次。

| Profile | channel/pub/sub/job/event各上限 | 每通道Source/Target | C额外静态存储 |
| --- | --- | --- | --- |
| Nano | 4 | 4 / 2 | 2048 B |
| Lite | 8 | 8 / 4 | 6144 B |
| Full | 16 | 16 / 8 | 12288 B |

这是显式可裁剪的额外业务状态，不包含Core、Driver和产品context。Rust按`Inbox<4/8/16>`静态
裁剪，实际布局不同，不声称与C字节数相同；资源数据见本轮报告。没有修改普通帧头、Opcode或Record。

本轮不提供ROS Graph/GID/CDR、RMW/DDS替换、MAVLink动作代理、动态匹配、受保护Native后端、
可靠/RPC/耐久业务、严格Timed控制或实机签字。这些沿用方案中的独立阶段，不因示例可运行而完成。
