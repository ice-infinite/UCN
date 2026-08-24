# UCN Cluster Timer Algebra

## 统一规则

所有 Cluster deadline 使用 `uint32_t` 单调毫秒时钟和 half-range 代数：

```text
duration: 1 .. INT32_MAX
deadline = now + duration  （结果 0 非法时原子拒绝）
expired  = (int32_t)(now - deadline) >= 0
```

禁止裸 `now >= deadline`、禁止把超过 `INT32_MAX` 的 duration 接入 deadline、禁止用 `UINT32_MAX -> 0/1` 表示 serial 轮转。

## Owner 纪律

1. RX、TX、Federation 和公开发布入口在产生副作用前执行时间/Authority preflight；
2. 配置切换先撤销旧授权，再按新 Stable/Joint voter lease 重算；
3. durable terminal 不因后续 step、迟到 vote 或 timeout 退回非终态；
4. rejected replay 在 exact deadline 边界不得破坏 pending slot；超时释放由显式 timer owner 处理；
5. callback 重入不得绕过 I/O active gate。

## 参数来源

产品参数位于 `ucn_cluster_config_t`，包括 observation、election、advertise、join retry、keepalive、member/head lease、recovery、provisional 等时间。初始化必须验证参数域；动态 Cost/Link 时间不属于 Cluster Timer Algebra。
