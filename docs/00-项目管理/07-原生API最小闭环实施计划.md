# 原生API首轮四项实施计划与最小合同

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成共同合同、最小send、静态Pub/Sub及有界应用分发、只读示例和C++/Rust薄外观四项。

**Architecture:** 可选Native Facade只拥有静态通道、实体和有界业务交接；复用真实Core/Adapter，
不复制Wire、Replay或Driver退休。C++封装C，Rust独立实现同语义，默认关闭Native时不增加Core状态。

**Tech Stack:** C99、C++17无异常/无堆、Rust 1.85+/no_std/forbid unsafe、CMake/Ninja、CTest/Cargo。

**Spec:** [接口设计](../10-理论与规划/建议方案/UCN_v6_原生发布订阅与统一用户API简化详细设计方案.md)、
[联合自审](../09-审计与整改/UCN_V6_原生API_RMW_设备对接三方案联合自审报告.md)。

## Global Constraints

- 原生MCU优先发布订阅，保留最小点对点发送；普通Core Header新增0 B。
- 所有新增接口保持固定容量、非阻塞、失败不部分写出；不新增Wire/Opcode/Record。
- Facade不直接调用Security/Route/Persistence/Adapter私有Owner，不与既有运行时抢Ingress。
- 同流别名必须匹配完整canonical配置；Latest按精确来源流，不按Service混掉不同生产者。
- 回调在协议gate外，只能提交复制型deferred业务；close不依赖普通数据队列空位。
- 本地接纳不是远端接收/动作完成；Unknown仍由真实Core/Adapter持有到物理退休。
- RMW/CDR/GID、动态发现、RPC、耐久动作、Timed严格源年龄不在本轮四项实现范围。
- 本轮不操作COM/USB/目标探针、固件或真实执行器，不安装ROS、不推送Git。
- 保留继承工作树；先保存精确源码快照，再记录本轮差异，不把旧修改归为本轮成果。

## Review Focus

- 数据池满后close、回调中自关闭与递归step：围栏仍成功，无悬空context，无递归Owner。
- 输入/输出/存储部分别名、错误Manifest与容量：拒绝时存储/out句柄/Sequence不变。
- 新旧来源交替、Replay窗口内较旧消息：不同来源不饥饿，旧值不覆盖新Latest。
- NOT_SUBMITTED/Unknown/迟到completion/停止：不重分配原帧序号，不虚报退休。
- 模块OFF、不同Profile/ABI/语言：Core不依赖Native，独立同字节/安装/资源回归。

## 1. 已核验基线与实施裁决

基线HEAD为`83eed914ea9ef547f435fbf91bc3fbca46293abb`，加当前完整工作树。
506项原始文件快照已生成并逐项校验：`.v6s-native-api/baseline-83eed91-20261106.zip`，
SHA256=`b425bd284b2a92d934be7b38b0856f744d9f7edce9f24e7d27d12773f94945b0`。
它包含500项源码/配置/测试/工具及6份相关文档，HEAD保留其余已提交资料。

Ruling: 原地复用当前非main分支，不创建会遗漏继承工作树的新worktree；使用精确快照区分
旧实现与本轮代码。成本是后续需要按快照审查差异，而不是只看git diff HEAD。

Ruling: 首轮C/Rust共同实际网络后端使用既有、显式可信静态网络C1/O0/H0/OneWay/
BestEffort；来源仅为产品配置绑定，不伪称密码认证、可信源采样时间或掉电Replay。
要求Origin认证、加密、RPC、可靠投递或严格源年龄的Native配置在写入前返回Unsupported。
已有安全/路由运行时不被删除；把这些后端接入同一Facade是后续独立能力，不假报完成。
这是API-00按真实底座能力冻结的第一子集，不是把保护要求静默降级。

Ruling: Native是独立caller-owned静态存储，挂接唯一既有Core。C初始化多一个attach步骤，
产品助手/C++/Rust外观隐藏装配；成本是一小块可裁剪本地状态，换取Core OFF零Native字段。
精确类型使用`ucn_native_t`，不为跟随候选伪代码而把Facade状态硬塞进所有`ucn_node_t`。

Ruling: Latest在首轮明确指本地队列KeepLatest策略，实际Wire仍BestEffort；不伪称支持
当前底座尚未接通的Wire DeliveryGuarantee::Latest或Reliable。所有Peer静态配置同Schema。

Ruling: 本轮只读业务16 B健康消息、普通字节单帧；用户明确要四项连续完成，不再停等计划
确认。C/Rust合同先统一，主执行者逐项实现/TDD，最后新鲜只读代码与架构审查。

## 2. API-00最小共同合同

### 2.1 边界与配置

- 可选CMake开关`UCN_FEATURE_NATIVE`默认OFF，ON才发布`UCN::native`、Native/C++头。
- C公共`ucn_publish(node,target,...)`一次迁名为`ucn_send`，不保留旧目标式兼容别名。
- 新`ucn_publish(native,publisher,bytes,len)`属于可选Native，不改变普通帧格式。
- `ucn_native_init(storage,bytes,node,config,out_native)`只挂接INITIALIZED、Endpoint未使用过
  的Core，完整预检后才写Native存储。Core和Native使用不同的短锁，调用方永久保活。
- 配置包含Native API/Layout/编译Manifest、实例号、复制后不可变的channel列表和短锁。
- Channel含local_id、Service、schema_id/revision、固定长度、Q1..Q3、FIFO/KeepLatest、
  安全下限、最大本地寿命、精确Source列表及固定远端Target列表；没有字符串逐帧发送。
- OneWay同分派键唯一Schema；同Service别名完全一致才共享流。未知枚举/保留值拒绝。
- 每项容量进入Native编译Manifest；Core API修订至2，追加只读接收顺序/静态身份信息，
  并同步C/C++ ABI断言。Native API/Layout自有版本1，不把二者混成Wire Version。

### 2.2 生命周期与调用

| 接口族 | 合同 |
| --- | --- |
| publisher/subscription create | 返回精确实例/slot/generation句柄；失败不写out |
| publish / native_send | 同步复制到固定命令池，UCN_OK只表本地接纳；资源不足不消耗Core序号 |
| native_step(now,budget) | 唯一协议推进；有界轮转处理命令/cleanup及Core，背压保留原期限 |
| native_dispatch(now,budget,out) | 单写应用Executor，锁外回调；同一调用最多budget次，禁止递归 |
| publisher/subscription close | 固定latch围栏，不靠数据队列；清未开始回调，活动context等退出 |
| source_fence | 精确来源永久失效，不清来源高水位；排队消息不复活 |
| native_stop | 围栏新业务；step推进Core真实停止，Unknown不清除 |
| native_status/deinit | 明确Native逻辑清理和Core物理quiescence；未满足不得回收内存 |

Core只增加必要只读View：当前生命周期/实例/时间/Endpoint历史/待退休TX，以及按地址
查静态Binding；不把Owner可变布局安装给Facade。Endpoint消息追加实际Origin Sequence和
配置Principal digest，后者不是密码Principal证明。Native Sample显式标为静态可信或本机
来源；没有有效Origin认证事实时，受保护通道不可创建。

### 2.3 排队与所有权

- Profile确定channel/entity/source/target/command/event上限；Payload首期上限64 B。
- 一次fanout复制一份不可变业务正文，冻结目标集；本地接纳预留所需命令/本机交付空间。
  多远端开始发送后可部分失败，单次返回不承诺分布式全有或全无。
- 来源流保存顺序水位，KeepLatest不得被旧合法帧覆盖；FIFO满拒绝，不伪造接纳。
- 本机事件同样检查来源规则/Schema/容量；本机顺序域与线上Origin域明确分开。
- 接收时记录原本地RX锚点，dispatch前检查来源围栏/订阅generation和半开本地期限。
  不把该局部期限声称跨节点源年龄，不刷新旧消息时间。
- Core接纳后拥有真实TX副本；Native不得清其Token。Native close可结束自身逻辑引用，
  deinit仍要求Core真实quiescence与所有Callback退出。
- Rust永远借用静态Core/Adapter/Driver/Inbox，Drop/forget不能归还仍被引用内存；首版
  为单任务运行期，不能把RefCell包装伪称跨线程安全。C/C++context按声明保活。

## 3. 文件结构

| 位置 | 职责 |
| --- | --- |
| include/ucn/ucn_core.h、src/runtime/ucn_node.c | 最小send命名、只读真实事实，不存Native表 |
| include/ucn/ucn_native.h、src/native/ | C99静态Facade与本地资源，只有公共Core依赖 |
| include/ucn/ucn.hpp | C++17 typed薄包装、无堆无异常、明确借用/析构 |
| rust/crates/ucn-native/ | Rust独立no_std静态Facade，复用ucn-core/Adapter而非FFI |
| examples/native/、Rust examples/ | 只读健康Codec/三节点软件例子，不接设备或Vendor |
| tests/simplified/test_native*.c/.cpp | 真实Core/Adapter Fake Driver闭环、负向、并发/资源 |
| tools/v6/check_native_boundaries.py | 架构/裁剪、同字节及实际consumer门禁，不以grep代替行为 |

## 4. 四个可验收任务

### Task 1: 共同合同与基线

**Files:** 本计划、任务表/操作记录、本轮进度台账；不改旧签字哈希。

**Interfaces:** 固定第2节；供后三任务引用，不扩张Wire或RMW范围。

- [x] 保存并逐文件校验当前源码快照，核对分支/继承变更。
- [x] 运行改动前Full CTest与Rust workspace；失败须归因并保留。
- [x] 自审共同类型/上限/安全与生命周期，记录每项裁决与成本。

### Task 2: 最小send与Core事实

**Files:** ucn_core.h、ucn_config.h、ucn_node.c、Rust ucn-core、现有调用者/ABI/索引工具。

**Interfaces:** 产生`ucn_send`、Core只读View及接收顺序事实，供Native使用。

- [x] RED：最小send符号/默认策略/输出零写、回调真实来源序号、View生命周期断言。
- [x] GREEN：迁名全部实际调用者，无兼容别名；补必要只读入口与API版本/ABI。
- [x] 复跑既有Core/全CTest、Rust核心回归与Manifest/安装/栈门禁；失败项逐项保留，不等同全部PASS。
- [x] 自审错误输出、门禁/序号/NOT_SUBMITTED/Unknown，保存差异和证据，不混合旧修改。

### Task 3: C静态Pub/Sub与有界应用分发

**Files:** ucn_native.h、src/native/*.c、Native测试、CMake/导出/架构门禁。

**Interfaces:** 消费Task2真实Core，不直接导入私有Owner；产出第2节的Native接口。

- [x] RED：初始化别名/Manifest/不支持保护模式、A发B/C收、copy后源缓冲失效、固定fanout。
- [x] RED：来源隔离/Latest旧序号/公平、Callback中publish/self-close/递归step、队列满close。
- [x] RED：到期/围栏后不交付、Driver背压/Unknown晚到完成、stop/deinit真实排空。
- [x] GREEN：最少固定数组实现；短锁不跨Core调用/用户回调，避免巨型自动副本。
- [x] 执行Full/Lite/Nano与OFF、ABI/安装、ASan/UBSan/分析器/可运行并发门禁并自审；旧栈/分析器问题见报告，未放宽门限。

### Task 4: 只读示例与语言外观、整体回归

**Files:** ucn.hpp、ucn-native crate、C/Rust只读例子及独立共同fixture、使用文档与报告。

**Interfaces:** C++薄封装Task3，Rust同语义独立实现；Health正文16 B显式大端，不发struct内存。

- [x] RED：C++实际typed publish/subscribe/close，Rust真实Core+Adapter闭环及静态借用约束。
- [x] GREEN：封装与只读三节点例子；点对点与Pub/Sub共存，不引入ROS或新头。
- [x] 独立C/Rust健康向量/原始帧检查，验证实际收到的数据、来源、费用和拒绝副作用。
- [x] 全CTest/Cargo Debug/Release、fmt/clippy/rustdoc、Cortex编译、OFF/资源/安装回归；Rust11类通过，C遗留失败明确登记。
- [x] 全量自审后新鲜只读代码/架构审查，重要问题以区分性RED→GREEN修复并复跑套件。
- [x] 更新完成/未支持范围与真实证据；不提前宣称RMW、控制动作、生产或实机完成。

## 5. 完成定义和记录

这四项的完成是“首轮静态原生用户接口可运行、有回归、有审查”，不是API-00～09所有
高级能力已完成。每步在独立进度台账记命令/退出码、RED/GREEN、自审和裁决；最后报告
实际C/Rust/安装/资源矩阵与未通过项。快照与日志保留，不删除用户任何原有文件。
用户未要求本轮提交推送，因此先交付完整工作树及差异证据，后续另行授权Git发布。

实际收口见[实施与整体自审报告](../08-实现与验证/版本演进/UCN_V6S_原生API首轮四项实施与整体自审报告.md)。
上述完成勾选表示实现/验证动作执行完毕，不把第7节仍失败的全局资源/分析器门禁改成通过。
