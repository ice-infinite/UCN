# 发送消息与可靠 Transfer

## 1. 先选语义

每条业务先独立选择 Traffic Class、Delivery Guarantee 和 Interaction Role。紧急程度不代表
可靠性；Request 不必一定可靠；Result 也可使用 Latest。需要跨重启避免重复副作用时分配
Operation ID 并使用 durable Journal。

## 2. 再选大小

选择能覆盖业务数据的最小 Message Class：32、64、128、256、512 B、1、2、4、8 KiB。
目标和 Path Capability 必须支持该档。Message Class 是重组上限，单帧可用 Payload 要扣除
Wire、扩展、安全 Tag 和 Fragment 头。

## 3. 单帧发送

校验 Message Context，查询目标 RouteSet 和 Path Budget，构造 semantic frame，调用 Security
保护后进入 QoS。QoS 返回 selection 后交给 Adapter；只有提交成功才 complete selection，
否则按 API 合同回退或重试。

## 4. Transfer

调用 `ucn_v6_transfer_send_begin()` 创建固定 TX slot；循环取得 `next_fragment()`，为每片完成
安全和排队，提交后记录。收到 SACK 后只重传未确认片。Credit 不足时等待新 Credit/超时，
不要忙等或扩大无界缓存。

Session、Binding 或 Path Generation 变化时调用相应 invalidate/rebind。旧 ACK 不能确认新
事务。目标端只有 `copy_completed()` 成功后才读取完整消息，处理完再 retire。

## 5. 操作结果

Durable Request 在外部执行前进入 PREPARED/EXECUTING。若执行后持久结果不确定，返回
IN_DOUBT 并由产品对账，不能盲目重做。业务应把 Operation ID 贯穿 Request、Result、日志和
外设幂等接口。
