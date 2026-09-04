# UCN RT-03 Time Domain FSM 自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 范围：默认不链接、调用者拥有的实验 Time Domain

## 1. 实现结果

- 建立 `UNSYNCED / ACQUIRING / LOCKED / HOLDOVER / FAULT` 五态模型；
- 有效同步样本和诊断样本使用显式 kind 分流，诊断样本只计数，不改变 offset、rate、窗口、有效样本数或状态；
- 使用最多 5 项固定 offset 窗口与中位数建立初始锁定，不使用堆内存；
- LOCKED 后对 offset 跳变、rate、每样本 slew 实施有界检查；
- 以 64-bit 本地单调时间计算 Domain Time，所有有符号修正、比例换算、uncertainty 增长均检查溢出；
- 对外时间单调不减；本地时间倒退、算术异常和过大 offset 跳变进入 FAULT；
- 精确定义 `sync_timeout` 边界进入 HOLDOVER、`sync_timeout+max_holdover` 边界进入 UNSYNCED；
- Master 重启只接受严格更大的 generation，清空旧样本/输出历史；generation 到 `0x7FFFFFFF` 后返回 `UCN_ERR_EXHAUSTED`。

## 2. 阶段自审整改

首轮源码自审发现并当场修复：

1. 两个任意 `int64_t offset` 直接相减可能产生未定义溢出，改为 checked difference；
2. `uint64_t` 采样间隔强转 `int64_t` 可能改变符号，超出 `INT64_MAX` 时现直接 FAULT；
3. 首个有效样本在达到锁定样本数之前也必须建立 offset 参考，否则第二个样本会错误地与零比较；现已修正。

## 3. 定向验证与资源

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，RT-01～RT-03 | 3/3 PASS |
| Windows GCC Full Release，RT-01～RT-03 | 3/3 PASS |
| 诊断样本不推动 LOCKED | PASS |
| 三有效样本中位数锁定 | PASS |
| HOLDOVER 起点/终点精确边界 | PASS |
| Master rebind、旧 generation、no-wrap | PASS |
| 时间倒退与 offset 跳变 | FAULT，输出不写回 |
| Host `sizeof(ucn_time_domain_t)` | 200 B |
| Release 对象 | `text=4888 B, data=0 B, bss=0 B` |

## 4. 边界

本阶段不含真实四报文同步、不含 Timestamp Driver、不含持久化 generation witness，也没有接入生产 Node/Service。RT-06 才负责 STATIC_MASTER 启动的 witness/state reload 证明；RT-08/09 才能给出目标 MCU 精度与资源结论。

## 5. 外审 RT-A02/A03 整改

- RT-A02：Domain 配置增加 `oscillator_uncertainty_known`，同步样本增加
  `uncertainty_known`；零值不再代表“完美时钟”，未知或零 uncertainty 不能进入
  有效滤波窗口。
- RT-A03：`HOLDOVER` 达到最大时长转入 `UNSYNCED` 时，统一调用 acquisition
  清理逻辑，清空固定样本窗口、`sample_count/cursor`、旧 offset/rate/reference、
  `has_valid_sample` 和连续样本数。配置、统计以及 RT-A08 要求的同 generation
  high-water 仍保留。
- 反例回归先以 offset=100 锁定并超时，再注入 offset=900 的新源；第一个新样本
  只能进入 ACQUIRING，第二个样本才重新 LOCKED，最终输出必须为 900，不能复用
  旧中位数 100。

## 6. 外审 RT-A08/A08-B 整改

- 把“采集/滤波状态”与“同 generation 单调/重放高水位”拆开：进入 UNSYNCED
  仍会销毁窗口和当前解，但保留 `last_sample_local_us`、`last_output_local_us`、
  `last_output_domain_us`、`has_output` 与 `has_sample_high_water`。
- 重新获取的样本必须严格晚于已接受样本高水位；不能用 UNSYNCED 清理重放旧采样。
- 同 generation 重锁时先在 `ingest_sample()` 内按候选 offset/rate 计算 Domain
  Time，再决定能否写入 `LOCKED`。若候选值低于已经发布的 Domain Time，不做静默
  clamp，不瞬时暴露错误 `LOCKED`，而是当场进入 `FAULT`、返回
  `UCN_ERR_STATE`、保持 `lock_transitions` 不增加。只有经过更大
  `domain_generation` 的合法 master rebind 才建立新的高水位域。
- 定向反例复现外审的 `old=11000, candidate=2000, generation=7`：现在返回
  `UCN_ERR_STATE`、phase=`FAULT`，旧输出高水位保留，不再发布时间倒退。
