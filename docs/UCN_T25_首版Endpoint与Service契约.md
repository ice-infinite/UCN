# UCN T25.0 首版 Endpoint 与 Service 契约

> 状态：**已冻结（R1 产品 ABI 基线）**。该契约已作为 T25.1 Router、T25.2 Protocol Task Bridge 与 T25.3 ESP32 静态 FreeRTOS Port 的输入；T25.3 仅完成构建验证，实机任务通信留 T25.4。
> 适用范围：MCU-first 的小型传感器/控制节点；一个 MCU 一个 UCN Node。
> 关联：[T25 详细执行方案](UCN_T25_节点内任务通信详细执行方案.md) · [更新后设计方案](UCN_更新后设计方案.md) · [任务表](00-任务表.md)。

## 1. 冻结原则

1. **Node 与 Service 分开。**MCU 对网络只暴露一个 Node ID；Service ID 仅供本机 Router 的来源审计、统计和本机 ACL 使用，不写入 v4 正常业务帧。
2. **Endpoint 是跨 MCU 的稳定 ABI。**Endpoint 编号、字段顺序、长度、单位、字节序、QoS 和消费者一经发布不得修改；变更布局必须使用新的空闲 Endpoint 和新版本名。
3. **首版单 Endpoint 单消费者。**一个已到达本 Node 的 Endpoint 只投递到一个本机 Service Inbox。需要本机和远端同时消费时，业务明确发两次；不隐式扇出、复制或订阅。
4. **所有字段显式编码。**禁止把 C/C++ 结构体的内存布局直接 `memcpy` 到网络；禁止指针、位域、平台相关 `enum`、浮点或未初始化填充字节进入首版 ABI。
5. **网络与本机的调用语义相同，执行路径不同。**同 Node 直投 Inbox；远端 Node 才交给现有 Core、Security、Policy、Path、Route 和 Link。

## 2. 编号空间与现有测试隔离

| 范围 | R1 规则 |
| --- | --- |
| `0x00..0x3F` | UCN Core 控制面保留，产品禁止使用。 |
| `0x40..0x4F` | 传感器原始/环境状态。 |
| `0x50..0x5F` | 电源、健康与安全状态。 |
| `0x60..0x6F` | 控制器与执行器命令；默认按 Q0 设计。 |
| `0x70..0xBF` | 后续静态产品 Endpoint，必须先登记再占用。 |
| `0xC0..0xFF` | 当前 Core 不接受为静态业务 Endpoint；不得用于本版本产品/测试注册。若未来扩展该范围，必须先修改并验证 Core Endpoint 契约。 |

ESP32 测试工程历史镜像的 `UCN_TEST_ENDPOINT_PING=0x40`、`UCN_TEST_ENDPOINT_THROUGHPUT=0x41` 是**独立测试镜像**中的临时值，不属于本 R1 产品 ABI，也不能与本契约的业务 Service 同时编译到同一产品固件。T25.3 已将当前宏迁至未占用的静态范围 `0xB0/0xB1`；`0xC0/0xC1` 不可用，因为当前 `ucn_node_set_endpoint_handler()` 只允许 `0x40..0xBF`。历史吞吐结果仍只属于旧 `0x40/0x41` 固件，新的 Endpoint 尚未烧录实测。

## 3. R1 Service 身份和 Endpoint 绑定

Service ID 不上线，也不等价于 FreeRTOS Task Handle；它只是 Router 配置中的稳定本机名字。

| Service ID | 名称 | 主要职责 |
| --- | --- | --- |
| `0x01` | `SENSOR` | 采样并发布 IMU、气压和温度状态。 |
| `0x02` | `POWER` | 采样并发布电源/健康状态。 |
| `0x03` | `CONTROL` | 消费状态数据，产生控制/执行器命令。 |
| `0x04` | `ACTUATOR` | 消费电机、舵机命令；执行本地限位、超时和安全回落。 |

| Endpoint | 名称 | 来源 Service | 目标 Service / Inbox | QoS / 本机容器 | 最大 Payload |
| --- | --- | --- | --- | --- | --- |
| `0x40` | `IMU0_RAW_V1` | `SENSOR` | `CONTROL` | Q1 / Latest | 24 B |
| `0x42` | `BAROMETER0_V1` | `SENSOR` | `CONTROL` | Q1 / Latest | 16 B |
| `0x43` | `TEMPERATURE0_V1` | `SENSOR` | `CONTROL` | Q1 / Latest | 12 B |
| `0x50` | `POWER_STATUS_V1` | `POWER` | `CONTROL` | Q1 / Latest | 16 B |
| `0x60` | `MOTOR0_COMMAND_V1` | `CONTROL` | `ACTUATOR` | Q0 / FIFO | 16 B |
| `0x61` | `SERVO0_TARGET_V1` | `CONTROL` | `ACTUATOR` | Q0 / FIFO | 16 B |

`0x41` 保留为下一颗同 Node IMU（`IMU1_RAW_V1`），以保持与既有总体设计的一致性；R1 不定义其字段。产品若不使用它，仍不得重新解释为吞吐或其他正式业务。

这里的“来源 Service”表示本机默认合法生产者，“目标 Service”表示该 Node 接收数据后唯一投递者。远端帧的发端身份仍由 `source Node ID + Endpoint Security/ACL` 决定；T25.0 不把一个远端 Node 伪装成本机 Service ID。

## 4. 所有 Payload 的统一编码规则

- 多字节无符号整数使用 **big-endian**；有符号整数使用 two's-complement big-endian。
- 字段以表中偏移连续编码，Payload 不包含对齐填充。
- `sequence` 以无符号 32 位自然回绕；消费者只能在同一来源 Node + Endpoint 内比较。
- `sample_time_us_low` 是源 MCU 的低 32 位单调时间，只用于样本排序和诊断；尚未定义跨 Node 同步时间，不得直接与本机时间相减得出时延。
- Q0 的 `valid_for_ms` 由目标 ACTUATOR 在**本机接收时刻**开始计时；超时必须进入产品定义的安全状态，不得因为网络重新连通而恢复过期命令。
- Q1 报文不得等待旧样本被消费；Latest 覆盖时由 Router 记录覆盖计数，而不是把它当成无痕可靠传输。

### 4.1 `IMU0_RAW_V1` (`0x40`, 24 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `sequence` | `u32` | 采样序号。 |
| 4 | `sample_time_us_low` | `u32` | 源 MCU 单调微秒计数低 32 位。 |
| 8 | `accel_x_mg` | `i16` | 毫重力加速度。 |
| 10 | `accel_y_mg` | `i16` | 同上。 |
| 12 | `accel_z_mg` | `i16` | 同上。 |
| 14 | `gyro_x_cdeg_s` | `i16` | 0.01 degree/s。 |
| 16 | `gyro_y_cdeg_s` | `i16` | 同上。 |
| 18 | `gyro_z_cdeg_s` | `i16` | 同上。 |
| 20 | `temperature_centi_c` | `i16` | 0.01 °C。 |
| 22 | `status` | `u16` | 传感器驱动定义的有效性/故障位；未知位必须保留。 |

### 4.2 `BAROMETER0_V1` (`0x42`, 16 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `sequence` | `u32` | 采样序号。 |
| 4 | `sample_time_us_low` | `u32` | 源时间低 32 位。 |
| 8 | `pressure_pa` | `u32` | 帕。 |
| 12 | `temperature_centi_c` | `i16` | 0.01 °C。 |
| 14 | `status` | `u16` | 驱动状态。 |

### 4.3 `TEMPERATURE0_V1` (`0x43`, 12 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `sequence` | `u32` | 采样序号。 |
| 4 | `sample_time_us_low` | `u32` | 源时间低 32 位。 |
| 8 | `temperature_centi_c` | `i16` | 0.01 °C。 |
| 10 | `status` | `u16` | 驱动状态。 |

### 4.4 `POWER_STATUS_V1` (`0x50`, 16 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `sequence` | `u32` | 采样序号。 |
| 4 | `voltage_mv` | `u16` | 毫伏。 |
| 6 | `current_ma` | `i32` | 毫安；正负方向由产品电流采样定义。 |
| 10 | `temperature_centi_c` | `i16` | 0.01 °C。 |
| 12 | `state_of_charge_permille` | `u16` | 0..1000；未知使用 `0xFFFF`。 |
| 14 | `status` | `u16` | 电源/故障状态。 |

### 4.5 `MOTOR0_COMMAND_V1` (`0x60`, 16 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `command_sequence` | `u32` | 命令序号。 |
| 4 | `valid_for_ms` | `u16` | 目标执行器允许命令存活的最长时长。 |
| 6 | `mode` | `u8` | `0=disabled`、`1=normalized_target`；其他值拒绝。 |
| 7 | `flags` | `u8` | bit0=enable；其余位必须为 0。 |
| 8 | `target_0_permille` | `i16` | -1000..1000，超范围拒绝。 |
| 10 | `target_1_permille` | `i16` | 同上。 |
| 12 | `target_2_permille` | `i16` | 同上。 |
| 14 | `target_3_permille` | `i16` | 同上。 |

### 4.6 `SERVO0_TARGET_V1` (`0x61`, 16 B)

| 偏移 | 字段 | 类型 | 单位 / 说明 |
| --- | --- | --- | --- |
| 0 | `command_sequence` | `u32` | 命令序号。 |
| 4 | `valid_for_ms` | `u16` | 目标本机的命令有效时间。 |
| 6 | `channel_mask` | `u8` | bit0..bit3 表示 4 个目标字段是否有效。 |
| 7 | `flags` | `u8` | bit0=enable；其余位必须为 0。 |
| 8 | `servo_0_us` | `u16` | 微秒脉宽；具体安全范围由本机硬件配置限制。 |
| 10 | `servo_1_us` | `u16` | 同上。 |
| 12 | `servo_2_us` | `u16` | 同上。 |
| 14 | `servo_3_us` | `u16` | 同上。 |

## 5. R1 固定资源 Profile

R1 只为 T25.1 的纯 C Router 冻结一组**测试默认值**；不同 MCU 可在编译期选择更小或更大 Profile，但不得在运行时无限扩容。

| 配置 | T25.1 默认 | 含义 |
| --- | --- | --- |
| `UCN_SERVICE_MAX_BINDINGS` | 6 | 对应本文件的 6 个 Endpoint。 |
| `UCN_SERVICE_MAX_PAYLOAD_BYTES` | 32 B | 大于当前最大 24 B Payload，留出小幅 ABI 余量；并非 Core 的全帧 Payload 上限。 |
| Remote TX Q0 深度 | 4 | 远端执行器命令的有界请求槽。 |
| Remote TX Q1 深度 | 4 | 远端状态/传感器请求槽。 |
| 每个 Q0 Inbox 深度 | 4 | `MOTOR0`、`SERVO0` 各自 FIFO。 |
| 每个 Q1 Inbox 深度 | 1 | 每个 Endpoint 一个 Latest 槽。 |
| 首版 Payload 所有权 | 固定副本 | 禁止保存调用者裸指针。 |

这不是对所有目标板的 RAM 承诺。T25.3 的 ESP32 Port 已完成静态构建：S3 A/B 各为 47,180 B RAM / 594,783 B Flash，WROOM 为 49,320 B / 621,271 B；尚未上传，因此 Heap、Protocol Task 栈和各业务 Task 栈高水位仍需 T25.4 实测。RAM 紧张的最小 Profile 可以先缩减为 4 个 Binding、Q0=2、Q1=2、Payload=24 B，但只能删除未使用的 Endpoint，不能截断已冻结 ABI。

## 6. 安全和失联安全边界

- `0x60`、`0x61` 在生产节点应由既有 Endpoint Security Policy 配置为受保护/受 ACL 控制；目前生产 AEAD 与产品 ACL 表仍属于 T15，不将测试明文 Profile 写成安全结论。
- 本机 Fast Path 不产生网络密文，但 T25.1 预留 `source Service -> Endpoint` 静态 ACL 位置；首版最小规则是 `SENSOR/POWER` 不得向执行器 Endpoint 发送。
- ACTUATOR 必须独立检查模式、范围、标志、序号与 `valid_for_ms`，并在超时/未就绪/队列满/远端不可达时进入本机安全状态。UCN 的 Q0 只保证有界投递，不替代硬件互锁、PWM 限幅或 FOC 安全闭环。
- Q1 传感器状态的损失或覆盖不能自动触发旧值重放；CONTROL 应按本地接收时间检测数据新鲜度。
- Task 进入 `ready=false` 时，Router 会清空该 Binding 的 Q0 FIFO/Q1 Latest；未 ready 时不能读取旧消息，重新 ready 后只接受新消息。
- `ucn_service_send_ex()` 可区分 `LOCAL_DELIVERED` 与 `REMOTE_ENQUEUED`；后者只表示本机 Router 拥有副本，不表示 Bridge/Core/Link 已接受，更不表示远端执行。
- 高风险 Q0 可选用 12 B `ucn_service_command_guard` 业务前缀（`command_id/issued_at_ms/valid_for_ms/result_endpoint`）。它不增加所有 Service 消息的固定 RAM；跨 Node 使用 `issued_at_ms` 时必须由产品提供共享时间域，否则应使用产品自己的 Generation/Lease 规则，不能假定两块 MCU 的启动时钟天然同步。

## 7. T25.1 的直接实现输入

T25.1 必须从本表生成或等价维护 `ucn_service_binding_t[6]`，并验证：

1. 仅列出的 Endpoint/QoS/最大长度被接受；任何其他产品 Endpoint 返回配置或未找到错误。
2. 本 Node 的 `0x40/0x42/0x43/0x50` 进入 CONTROL 的 Q1 Latest Inbox；`0x60/0x61` 进入 ACTUATOR 的 Q0 FIFO。
3. 目标为远端 Node 时，Router 只写 Remote TX Request；此时不调用 Link，也不直接触碰 `ucn_node_t`。
4. Protocol Task 随后才把 Request 交给 `ucn_node_send_endpoint()`；现有 Endpoint Security、Policy/Path、Q1 自动寻路和 Q0 不寻路规则继续生效。
5. 任何 ABI 字段更新都必须新增 Endpoint/版本，并更新本契约、测试向量、任务表和操作记录。

## 8. 本轮未冻结的事项

- `IMU1_RAW_V1 (0x41)` 的字段、其他传感器实例和多消费者静态扇出。
- 动态服务目录、订阅租约、Q2/Q3、可靠传输、分片和大块 DMA 数据。
- 生产身份、AEAD、密钥轮换和完整远端 Node ACL。
- 具体执行器的物理限位、PWM 频率、电机控制算法和实时周期；它们属于目标产品的硬件控制层。

这些未冻结项不得阻塞 T25.1；Router 首版只需正确处理本文件已列出的固定大小消息。
