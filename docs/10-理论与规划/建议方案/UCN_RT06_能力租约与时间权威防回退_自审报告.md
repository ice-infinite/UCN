# UCN RT-06 能力租约与时间权威防回退自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 范围：默认不链接的实验 archive；Storage Provider 由产品实现

## 1. 实现结果

RT-06 分为两个互不强绑的可选组件：

1. `ucn_time_capability`：保存 Endpoint 级协商租约。租约完整绑定目标 Node/Session、Endpoint、Metadata Version、Time Mode、Domain/generation、能力位和双向固定 Path；默认 4 槽，静态分配，不使用动态内存。
2. `ucn_time_authority`：实现 `STATIC_MASTER` 的 persist-before-sync 启动门。它先通过独立 witness 保留 `H+1`，回读精确证明后再写当前 Authority State，并再次回读；两个证明都完成前 `is_ready()` 固定为 false。

能力缓存采用以下失败关闭规则：

- 活跃的同 Node/Endpoint 记录只允许在完整协商身份与能力位完全相同时延长租期；
- Session、generation、Path 或能力发生变化时，调用者必须先显式失效旧记录，再安装新协商结果；
- 缓存满不淘汰其他活跃租约；exact deadline 到达即失效；
- REQUIRED 调用必须精确匹配全部身份字段和所需 capability bit。

时间权威采用以下顺序：

```text
load witness H
    -> reserve H+1
    -> reload exact witness H+1
    -> store Authority State generation=H+1
    -> reload exact state
    -> READY / 才允许上层发布同步报文
```

若 witness 已写入而 state 写入、回读或供电失败，下一次启动直接从 witness 高水位继续保留更大的 generation，不复用已签发数值。已配网产品丢失 witness 必须失败关闭；只有显式的首次投产配置 `commissioned=false + allow_initial_commissioning=true` 才能从空存储建立 generation 1。

## 2. 分项自审整改

RT-06 第一轮实现和测试后又进行了独立代码自审，关闭了以下问题：

1. **租约重放覆盖**：最初允许同 Node/Endpoint 直接替换，旧 Session 或旧 Path 可能覆盖较新租约；现改为活跃租约只允许精确同身份向后续期，跨身份变化必须先失效。
2. **缓存内存损坏准入**：准入前会重新验证实际缓存项，带未知能力位或非法身份的项不能因部分字段碰巧匹配而通过。
3. **Path 双向绑定**：补充 `forward.owner == reverse.destination`，防止两条不属于同一对端关系的 Path 被拼成租约。
4. **首次投产歧义**：`commissioned` 与 `allow_initial_commissioning` 必须严格互斥；已配网产品即使误留 allow 标志，也不能把 witness 缺失解释成 Factory Empty。
5. **Owner 对象篡改**：`start/poll/is_ready` 重新验证配置、Provider、Context、operation ID 和 desired records，不能只信初始化时检查。
6. **回调重入与并发**：在任何 `load/reserve/store/poll` 进入外部 Provider 前建立对象内 `io_active` 和调用方持有的执行域共享 Gate；重入 `start/poll/init` 或另一 Authority 同时进入均在二次 I/O 前拒绝。旧的无同步进程静态 Owner 已删除。
7. **物理独立性边界**：删除以两个 Context 指针不同来“证明”存储独立的错误假设。API 只提供两个独立合同；产品仍必须用硬件计数器、安全元件或经过审计的独立追加式 witness 证明真实 anti-rollback。
8. **重启 Session 绑定**：持久状态保存的是上一启动 Session，不能要求它等于本次新 Session；恢复只以 Domain+Master Node 判断同一 Authority，随后把本次 Session 与新 generation 一起写入并回读。新增 `Session 100 -> 101` 的重启回归。
9. **过期槽惰性回收**：安装新租约时允许复用已经到期的槽，但必须先完成新租约全部验证，失败不改变原缓存；任何仍活跃的槽继续禁止隐式淘汰。
10. **准入夹具区分度**：Session 不匹配回归使用自身完全合法的租约，只让调用方期望 Session 不同，确保拒绝来自 exact admission，而不是租约结构先天非法。

## 3. 测试证据

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，RT-01～RT-06 | 7/7 PASS |
| Windows MSVC VS2019 Full Release，全仓库当前测试集 | 58/58 PASS |
| Capability exact identity / missing bit / expiry | PASS |
| 满 4 槽不淘汰、续期、旧 Session/generation/Path 重放 | PASS |
| 首次投产 generation 1、Session 100→101 重启 generation 2 | PASS |
| witness 已保留但 state 失败，重启跳到 generation 2 | PASS |
| 损坏 state + witness=11，失败重试后不复用 12、发布 13 | PASS |
| 同步与多次异步 PENDING、operation 错配、回读不一致 | PASS |
| Provider 回调重入 | `UCN_ERR_STATE`，无二次 I/O |
| 双线程双 Authority 共享 Gate | WSL TSan PASS，无 data race；第二 Authority 零 I/O、零写回 |
| Host 固定对象 | capability cache 304 B；authority owner 112 B；shared gate 32 B |
| MinGW Release 对象 | authority `text=5040 B, data=0 B, bss=0 B` |

## 4. 外审 RT-A11 整改

- 原 `provider_callback_owner` 是普通静态指针，两个 Authority 即使各自由不同 Owner
  和 Provider 驱动，也会在没有共同锁的情况下并发读写，属于 C 数据竞争。
- 现在由集成方在执行域级创建一个 `ucn_time_authority_callback_gate_t`，并把同一
  Gate 传给所有可能并发的 Authority。Gate 只接受具备任务临界区回调的兼容
  `ucn_port_ops_t`，Provider 回调不是 ISR API。
- `init/start/poll` 与四类 Provider I/O 都检查该 Gate；Gate 的 active owner 只在
  公共物理锁下读写，回调返回后按精确 Owner 释放。
- POSIX 回归用两个 pthread、两个 Authority、两套独立 Provider Store 和一个共享
  mutex Gate：Authority A 的 load 保持阻塞时，Authority B 的 start 返回
  `UCN_ERR_STATE`，四个 Provider 计数保持 0，完整 B 对象逐字节不变；A 返回后两者
  均可正常 READY。相同目标在真正启用的 TSan 下通过。

## 5. 仍未完成的产品边界

- 当前 fake Provider 验证的是 API/状态机，不是 ESP32 Flash、OTP/eFuse、安全元件或真实掉电；
- Provider 必须把“完全擦除”返回为 `UCN_ERR_NOT_FOUND`，把存在但认证/CRC 损坏返回为 `UCN_ERR_CRC`，不能把损坏伪装为空；
- witness 与普通状态双槽必须处于独立的 anti-rollback 故障域，C 类型和 Context 地址无法证明这一点；
- 本模块尚未接生产 Service、Node RX/TX、Time Endpoint 或任何 Authority 发送路径；
- generation READY 后如何初始化 Time Domain、清空旧 pending 与开始 ACQUIRING，将在 RT-07 集成模拟验证，真实硬件仍归 RT-08/09。

因此，RT-06 的结论仅为“软件模型与合同阶段自审通过”，不能写成生产时间权威或掉电安全已经验收。
