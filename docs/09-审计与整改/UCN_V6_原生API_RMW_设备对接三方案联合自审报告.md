# UCN v6：原生 API、rmw_ucn 与设备对接三方案联合自审报告

> 日期：2026-11-06。
> 范围：完整复读三份候选设计，跨文档审查并修正文档；不审定不存在的实现。
> 状态：DOCUMENT SELF-REVIEWED / REMEDIATION APPLIED / AWAITING USER REVIEW。
> 不修改源码、Wire、API/ABI、Registry、Record、CMake、测试或配置，不操作实机。
> 不是独立外审 GO，不授权代码、依赖安装、Git 提交/推送或生产使用。

## 1. 结论和证据等级

三份方案的主分层可以保留：原生 MCU 的 Pub/Sub 优先与最小 send、独立可选 ROS
RMW、设备语义网关分别服务不同用户，复用同一通信合同，不是三套 Core 协议。

本轮从头到尾复读，不只复查上次改动。确认并收紧下述9组设计问题，包括生命周期、
跨Session语义及安全反例；其中有实际文本矛盾，也有原文未规定清楚的实施合同。
它们不是本轮复现的生产代码漏洞，不以P0/P1代码缺陷数量替代文档审查结果。

本报告的结案仅覆盖文档机械检查、源码未变核验及内部只读差异复核。接口布局、
容量、动态控制Body、Provider/设备版本等仍须在各自首阶段冻结，不能把本文当实现GO。

## 2. 审计对象和审前基线

仓库：`v6-simplified-rust@03eb7eb03098bfc7abd63f16e911e9d8fe45d26f`加完整继承工作树。
工作树有其他历史未提交内容；本轮保留，不以HEAD单独代表三方案或协议源码。

发布边界：用户后续授权的本次文档提交只包含三方案、报告、导航与相关台账段，不包含
继承工作树的C/Rust/共同合同更新。部分源码及共同合同链接仅在完整本地工作树中可用，
尚不在远端对应路径；应结合后续代码提交核验，不把文档提交SHA当作实现审计基线。

| 审计对象 | 审前行数 | 审前文件SHA256 |
| --- | ---: | --- |
| [原生接口方案](../10-理论与规划/建议方案/UCN_v6_原生发布订阅与统一用户API简化详细设计方案.md) | 769 | de8e868175e4758f087485ee40ee7a9f6fd2995458f3e9ec52bc3e7b33c80dfd |
| [rmw_ucn方案](../10-理论与规划/建议方案/UCN_v6_rmw_ucn_ROS2中间件后端详细设计方案.md) | 859 | dbbcd0296e544008576a2b942b1ee9ac68e7f66f1fd0a86764154360c957eb14 |
| [设备对接方案](../10-理论与规划/建议方案/UCN_v6_无人机机器人与其他设备详细对接方案.md) | 920 | ec38fcf62a92d90e088310e14a76aba7c9cfaadcb7e67f8052c957289653f85c |

逐段读取到EOF；出现工具输出截断时单独补读，不能用章节标题扫描充当全文审查。
另外核对当前C公共API、Driver/配置入口、Rust Runtime、RPC/Origin共同合同及已有
Owner/回调限制。Obsidian只读辅助确认MCU-first/静态资源/Host可选，旧v4/v5细节不用。

## 3. 发现、反例与整改结果

| ID | 类别 | 审前问题/反例 | 修正位置与规则 |
| --- | --- | --- | --- |
| CJ-01 | 回调与使用时授权 | 设备文档笼统禁止回调内publish，接口文档允许应用回调post；实现者可能放宽协议重入，或误封SDK。排队后的领取点也需与撤权/close同步。 | 接口§15、设备§5、RMW§15：低层回调受原scope限制；SDK回调在协议gate外，只能deferred post。Coordinator核发精确交接许可并线性化领取，执行副作用前仍复验。 |
| CJ-02 | 生命周期与cleanup | 数据队列满时close/Drop不能排入命令，后续事件可能引用已析构context；C初始化示例又假定start失败必未提交。 | 接口§10/11/16：独立预留围栏/cleanup额度，Root持有闭包/退休责任；错误返回不是零注册证明，Ports与存储须活到真实退休。 |
| CJ-03 | 样本身份与ROS信息 | Writer序号跨Session保留，但Reader可能随Token重建丢水位，取过100后又取100/99；普通UINT64_MAX还会撞ABI哨兵。 | RMW§11/12/22：Reader/Writer GID水位跨Token/Session保留，last_taken单调；正常序号1..UINT64_MAX-1，补本地reception sequence、原接收时间和真实from_intra_process。 |
| CJ-04 | RPC恢复 | 保留ROS请求身份容易被误写成把旧易失Operation移到新Session再发送，从而重复外部动作；任意ROS服务也没有统一IN_DOUBT响应字段。 | RMW§13.4/21、接口§13：旧可能提交事务保留原键并停发，不伪造业务Response；耐久只用已有Resume观察结果，未知动作不重交执行器。 |
| CJ-05 | 时间与缓存 | RMW只明确入站时不刷新寿命，未明确入历史后take、TransientLocal或重传时到期；桥内已知排队时间也被混写为uncertainty。 | RMW§16/21/22、设备§13：保存原年龄锚点/时间域，所有实际交付点复验；确定经过时间算年龄，未知误差算uncertainty，不重盖时间。 |
| CJ-06 | 控制域防ABA与执行水位 | 业务任务重启、UCN Session仍live，epoch从1重用，新grant challenge不能阻止未见旧目标撞入；接纳和执行若共用一个严格“大于”水位，首个合法执行还可能被自身拒绝。 | 设备§6.4/7.3/18/20：产品安全Owner的单调epoch高水位，或真实新Origin域/耐久恢复；无法证明则禁控制。last_admitted/last_applied分开，只有实际采用的新目标刷新执行看门狗。 |
| CJ-07 | 静态通道完整性 | “一Service一Schema”未区分RPC的Request/Result；同Service两个Publisher若采用不同策略，同流别名会出现双解释；Replay允许的旧帧也不应覆盖Latest。 | 接口§6/7/22、设备§6：按Service+方向/Role固定Schema对；别名canonical策略完全一致；Latest保持来源内顺序，不由旧包刷新状态。 |
| CJ-08 | 目录所有者与恢复 | 可选第三方汇聚缺原所有者证明，不能凭邻居认证转授实体身份；Host目录重启后catalog revision及旧active集合也不能自行复用。 | RMW§5/8.3/22：首版仅接纳原Host自有公告或原样端到端路由；第三方重新封装未审则禁。catalog发布高水位耐久连续，恢复无live证明先围栏。 |
| CJ-09 | 实施依赖与证据范围 | 三份独立阶段表容易形成循环前置，或把原生C/Rust设备互通当完整ROS后端互通；显式桥并不必须等待rmw_ucn。 | 接口§21.1为共同依赖联表，另两篇引用；按实际能力放行，设备桥可采用既有合格RMW，不继承旧代码/硬件签字。 |

本轮没有选择“为所有普通帧新增一个通用代际、Topic头或安全大头”解决上述问题。
优先使用既有认证父域、产品本地状态与明确生命周期；必要专用Schema的替代方案
只由选用该业务的产品计费，且必须另审，不能自动进入普通通信基座。

## 4. 关键反例推演与后续正式测试要求

### 4.1 数据队列满与自关闭

```text
普通命令池满 -> Subscription close/Drop
    -> 独立close latch围栏，取消未开始事件
    -> 当前回调正常退出，Root仍持有context与物理退休描述符
    -> 固定cleanup预算推进 -> 真实quiescence后才可回收
```

不得在自身回调中阻塞等待close完成。C原始user指针需调用方保活；C++闭包和Rust
静态context由对应Root槽持有。Rust即使mem::forget运行句柄也不能悬空Driver内存，
但可能停止推进；“内存不悬空”不是“故障后保证继续工作”。

### 4.2 旧消息与新短绑定

```text
Reader R 已take Writer W 的publication=100
    -> UCN Session/短Token换代
    -> R/W GID未变，last_taken仍为100
    -> 新Token下99/100拒绝取样，101按当前权限/年龄规则判断
```

现有Core Replay只解决其自身Session序号域，不能替代ROS逻辑样本跨Session水位。
原生Latest则不自动承担ROS独立Writer语义，仍按其精确来源与产品Schema排序。
这是有界本地状态要求，不是给所有原生消息再加一个ROS序号。

### 4.3 已发送易失请求与Session后继

```text
旧Session下ROS Request -> UCN Operation可能已到Server
    -> 响应丢失，Session更换
    -> 保留原请求键/关联，停发；本地诊断/原超时收口
    -> 不把旧ID移植到新Session，不新建Operation重新执行
```

只有确证未提交的尝试才可按策略重新接纳。应用显式耐久操作使用原Resume/对账
机制，不能用“ROS接口方便”把不确定外部动作变成自动重试。

### 4.4 缓存与寿命

```text
源端原发布 -> 已验证年龄上界2s，Lifespan=10s，保存单调anchor
    -> Reader缓存中停留至少8s
    -> take时年龄上界已不小于10s，拒绝交付
    -> 不把take/新Token/历史补发时刻改成源发布时间
```

算术必须checked，并计入经过时间的慢钟/量化/读取误差。未证明源时钟或Domain时
拒绝有限Lifespan组合；默认ROS应用所需10s因此仍是实际实现门禁，不是文档保证。

### 4.5 业务任务重启与旧控制目标

```text
Root/Session仍live，旧授权epoch=1，旧合法目标(seq=100)尚未见过
    -> 业务控制任务重启，重新做新challenge授权
    -> 不允许epoch重新从1开始
    -> 产品安全Owner分配更大epoch；若高水位丢失则先禁控制
```

授权challenge本身不在20 B setpoint中，不能用“grant新鲜”推出“所有setpoint新鲜”。
新的真实Origin认证域或受保护耐久分配是可选恢复证明；仅本机drain/重开端点不够。
目标执行只比较last_applied并确认仍为当前Latest，不把已接纳水位当成第二次入队门。

## 5. 分层、身份、时间与费用交叉表

| 维度 | 原生API | rmw_ucn | 设备/网关 |
| --- | --- | --- | --- |
| 权威实体 | 本地句柄与固定Channel | ROS GID/Graph/绑定 | 产品设备/控制域/外部关联 |
| 线上认证来源 | 既有实际Origin/Session | 同一事实加精确ROS绑定 | 原生Origin或可信网关Principal，不伪称上游身份直通 |
| 调用入口 | C/C++/Rust薄外观或最小send | 标准RMW ABI+受控Host SPI | 原生业务或显式语义桥/受限隧道 |
| 协议状态 | 既有Owner，跨模块经Coordinator | 复用同一Owner，不复制Replay/Route | 不越过Coordinator直调其他Owner |
| 数据格式 | 固定紧凑Schema | 全RMW统一CDR+专用元数据 | 明确单位/坐标/时间，转换可有已声明信息损失 |
| 时间 | 按选用业务的既有合同 | 发布元数据不等于同步证明，Lifespan用时检查 | 源年龄、排队与误差完整保留，动作前再验 |
| 生命周期 | 逻辑close与物理退休分开 | 额外处理ROS对象/wait/Graph和退役arena | 额外保留外部命令/Action义务，不靠网络drain抹掉 |
| 普通Core Header增量 | 0 B | 0 B | 0 B |

复算保持不变：C1地址档基础头为11/13/15/17 B；A1/O1/H1固定45 B；原生健康16 B
合计61 B，4 B数据合计49 B；RMW前缀20 B、UInt32的候选CDR为8 B，合计73 B。
三个远端原生健康单播至少183 B；73 B样本10Hz给三个远端为2190 B/s。
RPC为既有9/11 B，Timed为选用合法组合时16 B；MAVLink隧道保留内层12/25 B。
不计PHY、Carrier、各跳、重试或控制消息时须明说，不能把Header不增加解释成无费用。

## 6. 内部独立复核与剩余工作

本轮使用新鲜只读builder check，要求全文阅读三篇，独立检查能否形成实施计划。
初读复核同样识别控制域ABA、跨Token样本水位、旧易失RPC、缓存Lifespan和cleanup
五项重要边界；与主审另外的消歧/依赖修订一并写入对应方案。
修改后只读差异复核已返回“nothing important left”，未发现影响实施规划的重要矛盾；
其意见不替代源码与机械门禁，不视作外部审计。主审另外核对整改条款和验收反例，
不能把一次审阅意见当作对未来实现永久无问题的证明。

明确保留而不伪称已经完成的工作：

- 新公共DTO/ABI/句柄、Profile容量、生成器与Host SPI正式签名，仍在API-00等冻结。
- RMW应用控制Body/CDR独立Golden、QoS/Graph/wait、目录Schema与代码包，尚未实现。
- 动态匹配、第三方汇聚、可回收Rust存储/非静态回调等后置，不给出隐含fallback。
- 生产Provider、Flash/Witness、MCU真实栈、物理Bearer、掉电/控制安全、性能与长稳未验收。
- 这次不把文档检查通过当CTest/Cargo/ROS测试通过，不继承历史矩阵数字。

## 7. 机械核验与范围保护

本轮核验计划及最终结果在[OP-589操作记录](../00-项目管理/01-项目操作记录.md)登记：
全文/章节/阶段连续性、表格、围栏、全部相关本地链接、示例字节算术、现有文档门禁
及diff-check。三篇不在旧检查器自动glob中，必须显式扫描，不能只引用旧门禁数字。

最终复跑：显式8份文档/295条本地链接，坏链0；三方案24/24/22章、API/RMW/DEV
阶段10/10/9连续；表格/围栏/关键条款/3篇修订后哈希与字节算术通过。现有文档门禁
`155 documents / 461 links`通过，`git diff --check`通过。关键条款文本检查只能防止
本轮遗漏必要规则，不冒充未来代码的行为回归或任意语义错误自动检测。

代码范围保护：编辑前src/include/tests/tools/rust/protocol/hardware及构建配置共500项，
按排序路径加每个原文件SHA256摘要汇总，其基线指纹为：

```text
4af74aabb822c99aa3e34a0cd3763a69ae4da2da299071db1c777b6fefbbe0cc
```

最终重算相同，500项未变；真实Git暂存区初始和最后均为空。未提交和未跟踪的原有代码
不能被本轮遗漏/覆盖。新报告本身不加入任何历史已签字候选清单，也不更改旧审计哈希。

三篇修订后文件的原始字节指纹如下；这是本次文档快照标识，不是代码候选或外审签字：

| 文档 | 修订后行数 | 修订后SHA256 |
| --- | ---: | --- |
| 原生API | 828 | 81b2c71877b33554a95cf1a97706454ee8b22faa02daec6a778584b2ac27e7dd |
| rmw_ucn | 946 | 591cc7f5fca046178e791dc7937a175dede73ffdda6e60ca3c9689205792fd8f |
| 设备对接 | 970 | ccbe35af706b974aedf9fbe5f6231eb8fa673e24474d5e247ec193767fdd8b84 |

## 8. 官方核验和状态边界

本轮重新核验Jazzy的[RMW types.h](https://raw.githubusercontent.com/ros2/rmw/jazzy/rmw/include/rmw/types.h)、
[rmw.h](https://raw.githubusercontent.com/ros2/rmw/jazzy/rmw/include/rmw/rmw.h)、
[time.h](https://raw.githubusercontent.com/ros2/rmw/jazzy/rmw/include/rmw/time.h)及
[QoS说明](https://raw.githubusercontent.com/ros2/ros2_documentation/jazzy/source/Concepts/Intermediate/About-Quality-of-Service-Settings.rst)，
用于校核序号哨兵、消息信息、取样、异步发布借用和寿命，不照搬其他RMW实现习惯。
MAVLink关联边界对照[官方命令协议](https://mavlink.io/en/services/command.html)，
继续保留“不凭普通ACK猜精确外部事务”的限制。

官方活动分支/网页不是将来供应链冻结证据，实施时记录精确依赖commit。
本轮为设计自审与文档整改，最终仍为AWAITING USER REVIEW / NOT IMPLEMENTED。
