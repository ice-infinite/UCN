# UCN V6 简化版 IMPL-04 Dynamic Admission 与 Capability 实施及全体自审报告

> 日期：2026-09-21  
> 状态：`IMPLEMENTED / SELF-REVIEW PASS / EXTERNAL REVIEW HOLD`  
> 证据范围：C 简化版私有 Host 软件实现；不代表真实 MCU、生产密码、Flash 掉电、物理 Bearer
> 或产品发布通过。

## 1. 本阶段解决什么问题

IMPL-01 提供静态明文通信，IMPL-02 提供统一 Persistence Foundation，IMPL-03 提供静态安全
Session。但未知设备仍不能安全加入 Realm，地址绑定也不能在掉电后证明没有回退；已经认证的
Peer 之间还没有规范方式交换能力、选择共同 Profile 或判定某个 Contract 是否可执行。

IMPL-04 因此增加三个职责互斥的 Owner：

1. Admission Owner 处理一跳 Bootstrap、反资源耗尽 Cookie、双方认证顺序和最终 Session
   Requirement；
2. Identity/Binding Owner 管理地址绑定、Authority lease 事实与 persist-before-publish；
3. Capability Owner 管理认证 Peer 的 canonical 能力快照、Profile 协商和 Contract Resolver。

三者没有互相持有 Owner 指针，也不直接调用 Persistence 或 Security。所有跨模块数据均为
Coordinator 可路由的 immutable requirement、proof、view 或 event。

## 2. 实施范围与后置项

本阶段实现：

- HELLO、Cookie Challenge、HELLO+Cookie 的严格 big-endian codec；
- stateless Cookie、认证前 per-Link 限流、固定 pending 槽；
- Authority/Device 双向证明和不可跳步 Admission FSM；
- 96 B Identity Binding Record、Authority lease 当前视图和 Persistence 桥；
- 68 B Capability Record、24 B Summary、20 B Query；
- 固定容量 Capability cache、Profile Select/Ack 和确定性 Resolver；
- SHA-256/128 canonical digest 以及 C/Rust 共同向量；
- Full/Lite/Nano 固定资源、Feature-OFF、栈、并发和发布隔离门禁。

本阶段不实现：

- 公共 `ucn_node`/用户 API 接线或真实网络自动入网；
- 生产签名、MAC、AEAD、Key Store 或硬件安全模块；
- 真实 Flash/Witness、断电注入和 Authority failover；
- C2/H2 Dynamic Route/Flow、Reliable/Transfer、Service、Realtime、Group、Cluster；
- MCU 实际栈水位、射频/串口/CAN Bearer 和实机性能。

## 3. 模块与解耦边界

```text
Driver/Runtime facts
        |
        v
   Coordinator
    |    |    |
    |    |    +------ immutable Security peer view ------> Capability Owner
    |    +----------- immutable Authority/Binding view --> Admission Owner
    +---------------- Identity/Persistence requirement --> Persistence Owner
                                |
                                +---- exact proof/event --> Coordinator
                                                        --> Identity Owner

Admission final result -- immutable Session Requirement --> Coordinator --> Security Owner
```

硬边界如下：

- Admission target 不链接 Security 或 Persistence；
- Identity target 不链接 Persistence；
- Capability target 不链接 Security，只消费已认证 Peer 的冻结 view；
- 三个 target 均为私有 `EXCLUDE_FROM_ALL`，不安装、不导出、不进入公共聚合 archive；
- digest 永远不作为 equality authority，复用前仍精确比较 canonical 字段。

## 4. Admission 状态机

```text
EMPTY
  |
  | HELLO -> stateless Cookie Challenge
  | HELLO+valid Cookie -> reserve fixed pending slot
  v
COOKIE_VALIDATED
  |
  | verify Authority proof and transcript
  v
AUTHORITY_PROVED
  |
  | verify joining Device proof
  v
DEVICE_PROVED
  |
  | exact Address Offer
  v
ADDRESS_OFFERED
  |
  | exact Device Commit
  v
DEVICE_COMMITTED
  |
  | durable Final Commit + live Identity/Authority facts
  v
FINAL_DURABLE
  |
  | emit immutable Session Requirement
  v
ADMITTED ---- expire/fence ----> RETIRABLE ----> EMPTY(new generation)
```

每一步都绑定 Realm、Link ID/Generation、transaction ID、双方 Principal、Identity Digest、
Cookie、Suite、Transcript 和绝对 Deadline。不能跳步、回退或换 Link；过期、事实漂移、错误
proof、错误 transcript 和 slot generation 都在输出/Owner 写入前拒绝。

Cookie 阶段不分配 pending 槽。Cookie Provider 的输入由 Owner 冻结，Provider 回调使用共享、
caller-owned Gate；递归或跨 Owner 回调失败关闭。只有 Cookie 验证成功且速率/容量允许，才消耗
固定 pending 资源。

## 5. Identity/Binding 持久化闭环

### 5.1 记录和租约

Binding Record 固定 96 B、big-endian，包含 Realm、Device/Authority Principal、Address、
Binding Generation、Lease ID、Authority Generation、challenge start、absolute deadline、
Foundation Transaction ID、Record Generation 和必要的 canonical 保留字段。

Authority challenge 起点由 Owner 在本地捕获并进入证明域。安装时要求：

```text
challenge_start <= trusted_now < deadline
```

调用方不能用新的“当前时间”重置旧证明的租约起点；未来起点、过期证明、错误 Authority、旧
Generation 或旧 Transaction 均拒绝。

### 5.2 持久化流程

```text
prepare_binding
  -> freeze 96 B body + transition fingerprint
  -> expose immutable persistence requirement
Coordinator
  -> Persistence submit/step/readback/witness/reload
  -> exact durability proof
activate_binding
  -> recheck proof + current facts + lease
  -> publish immutable binding view
```

Foundation Transaction ID 必须由前一 durable Transaction checked-next 得到；非 factory 流程不
允许缺少前驱，也不允许相等、回退或回绕。提交失败不能只凭一个 Handle 释放资源，而必须提供
精确失败证据：Handle、continuation、Domain、Transaction、Fingerprint、Runtime/Owner/Schema/
Operation/Generation、`FAILED` 状态和负终态结果全部一致。

## 6. Capability 与 Profile 协商

### 6.1 Canonical 数据

| 对象 | 固定长度 | 用途 |
| --- | ---: | --- |
| Capability Record | 68 B | 完整能力、Profile/Contract 位图、限制与代际 |
| Capability Summary | 24 B | 摘要通告和代际判断 |
| Capability Query | 20 B | 请求特定代际或摘要 |
| Capability Digest | 16 B | SHA-256 前 16 B，摘要/预筛选 |

所有多字节字段均为 big-endian；非法 reserved、枚举、长度、Profile/Contract 组合、零/回绕代际
或输入输出别名均失败且输出不写回。

缓存键绑定认证 Peer Principal、Session Generation、Binding Generation 和 Link Generation。
这些父事实任一变化，旧 Capability 立即不可使用。满表不驱逐；maintenance 通过持久旋转游标
返回精确 expired reference，由调用方显式退休。

### 6.2 Profile 和 Resolver

Profile Select 只从双方交集选择，并保留调用方声明的 required fields。Profile Ack 必须逐字段
匹配原 proposal transcript，不能只比较 digest。Resolver 对相同 Requirement 与 Capability
给出确定性结果；第一个不满足的 typed dependency 也固定，便于跨实现比较和审计。

## 7. 固定资源与栈

| Profile | Admission Owner | Identity Owner | Capability Owner | 合计 | Pending/Binding/Peer 槽 |
| --- | ---: | ---: | ---: | ---: | --- |
| Nano | 2160 B | 2600 B | 712 B | 5472 B | 2 / 4 / 4 |
| Lite | 3344 B | 5128 B | 1352 B | 9824 B | 4 / 8 / 8 |
| Full | 5712 B | 10184 B | 2632 B | 18528 B | 8 / 16 / 16 |

Admission/Capability/Identity/SHA 实现不使用动态内存。SHA-256 压缩使用 16-word 环形 schedule，
避免 64-word 自动对象；Security protect 的控制暂存移动到 caller-owned Workspace。正常 GCC
单函数栈门禁为 256 B，Full/Lite/Nano 与 Feature-OFF 均通过。

## 8. 对抗与失败副作用矩阵

定向测试覆盖：

- Cookie 篡改、过期、错误 Link/Realm/txid、速率超限和 pending 满载；
- Authority/Device proof 乱序、重复、错误 transcript、错误 Suite 和状态跳转；
- challenge 起点在未来、租约到期、延迟提交、Authority/Binding generation 漂移；
- Persistence Handle、continuation、Domain、Transaction、Fingerprint、request state 和结果变异；
- Capability 长度/reserved/摘要/代际/Profile/Contract 破坏；
- 同 digest 不同 canonical requirement 不得误复用；
- Owner/input/output、Workspace/output 的完整与部分别名；
- Provider 回调重入、双线程共享 Gate、错误 token/slot generation；
- 满表、过期、显式退休、旧引用重放与 no-wrap；
- Feature-OFF 无 Identity/Persistence integration target，公共 archive 无私有符号。

失败路径共同要求：首次 Provider/Persistence I/O 前完成可完成的预检；返回错误时调用方输出保持
哨兵，Owner 不发布 ADMITTED/BOUND/ACTIVE，不消耗不该消耗的槽、代际或 proof。

## 9. 两轮全体自审

### 9.1 第一轮：合同到实现

按 Admission、Identity、Capability、Coordinator/Persistence、CMake/符号顺序逐项核对冻结合同。
整改了 SHA 摘要强度、Identity 失败证据精确度、Provider 回调暂存、状态写入顺序、Owner/输入别名
和 Capability maintenance 输出别名。未发现跨 Owner 直接指针或公共发布面泄漏。

### 9.2 第二轮：故障到副作用

从坏字节、坏代际、坏 proof、超时、容量满、回调重入、线程竞争、Provider 失败、Persistence
失败和重启 proof 反向追踪。新增逐字段失败证据变异、同 digest/不同对象、旧 Session/Binding/
Link 事实、输出/Owner 重叠和 SHA Workspace 重叠用例。复查后未发现已知 Host 软件 P0/P1。

## 10. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 61/61 |
| Windows GCC Lite | 61/61 |
| Windows GCC Nano | 61/61 |
| Windows GCC Persistence-OFF | 50/50 |
| MSVC 19.29 Release `/W4 /WX` | 52/52 |
| WSL ASan/UBSan | 59/59；排除插桩不兼容的栈/安装门禁 |
| WSL GCC `-fanalyzer -Werror` | 66/66；排除调用链静态门禁 |
| WSL GCC TSan | Admission 双线程共享 Gate 通过；使用 `setarch x86_64 -R` 避免 WSL 地址映射冲突 |
| Rust `ucn-capability` | 6/6 |
| C/Rust Capability oracle | 7/7 |
| 私有 archive 导出 | 17/13/17 个定义符号，全部为 `ucn_i_*` |
| 公共 archive 泄漏 | IMPL-04 私有符号 0 |
| 动态内存/TODO/FIXME/HACK | 0 / 0 |
| `git diff --check` | 无空白错误，仅既有 CRLF 提示 |

ASan/UBSan 未运行静态栈和安装 consumer，是因为插桩会改变栈尺寸且安装消费者没有链接 Sanitizer
runtime；Analyzer 未运行 call-stack gate，是因为 `-fanalyzer` 会把部分帧标为 dynamic/unknown。
这些排除不覆盖语义测试；对应门禁已在普通 GCC/Nano 构建独立通过。

## 11. 最终结论与边界

```ini
IMPL-04 = IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD
KNOWN_HOST_SOFTWARE_P0_P1 = 0 found by current self-review
PUBLIC_RUNTIME = NOT CONNECTED
PRODUCTION_CRYPTO = NOT PROVEN
REAL_FLASH_POWERLOSS = NOT PROVEN
MCU_PHYSICAL_BEARER = NOT PROVEN
```

该结论允许按用户确定的流程继续内部实施 C IMPL-05，但不是外部签字，也不允许把测试 Provider、
Host Fake Persistence 或私有 archive 描述成可交付产品。后续任何涉及 Wire、Record、状态机、
Profile 容量或摘要算法的变化，都必须重新运行本报告的完整矩阵。
