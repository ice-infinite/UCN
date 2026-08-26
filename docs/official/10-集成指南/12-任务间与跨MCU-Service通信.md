# 任务间与跨 MCU Service 通信

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

为传感器、执行器或算法任务定义 Service ID/Operation。调用方使用统一请求/结果对象：目标在本机时走 Service Fast Path/任务队列，目标在远端时由 Bridge 封装进 UCN。

任务不是伪造的网络 Node；本机通信不必完整经历路由和 Wire 编解码。统一的是地址、服务、请求 ID、权限和结果语义。

实时 IMU 建议发布最新样本或固定速率流；舵机命令使用 deadline、sequence 和应用级确认，超时命令不得晚到执行。

## 地址模型

网络地址仍是 Node ID；Node内部用 Service ID/Endpoint区分任务和数据类型。任务不拥有独立路由表，也不参与 HELLO/选举。调用者只需指定：

```text
destination_node + endpoint/service operation + traffic class + payload
```

Router判断 destination是否本机，选择Fast Path或remote queue。

## 传感器实时流

例如 Node C 同时提供：IMU Endpoint 0x40、气压计0x41、温度0x42。Node A订阅IMU时只消费0x40；三类数据可共用同一 A→B→C Route。

本机 estimator task与远端控制器使用相同消息结构。IMU采用Q1 Latest：若消费者来不及处理，覆盖旧样本而不是累积延迟。payload建议携带采样timestamp、sample sequence和单位/版本。

## 远程命令/结果

```text
Node A Task
  → command {command_id, issued_at, valid_for, result_endpoint, args}
  → UCN/Bridge
  → Node C actuator task inbox
  → 校验来源、replay、deadline、当前安全状态
  → 执行动作
  → Result {command_id, REMOTE_EXECUTED, status, detail}
  → Node A
```

命令应幂等或具备去重记录。发送方超时后不能简单假定“未执行”并用新ID重复危险动作；可查询状态或重发同一 command ID。

## 本机Fast Path边界

Fast Path省掉Frame编码、路由和物理传输，但仍执行binding、payload、traffic、source ACL和ready检查。关键命令的业务validator不能因为本机来源就全部跳过；只是远端Core Security不适用。

## 与Transfer组合

Service固定payload适合小命令/样本。参数文件、地图、固件块等先由Transfer可靠传输，完成后再发Service命令引用对象ID/hash。不要把8KiB内容硬塞进Service queue。

## 任务设计

- Endpoint一个明确owner task；
- Q0 task及时take，队列满有背压/告警；
- Q1只保存最新值，消费者检查timestamp是否过期；
- task重启时`set_ready(false)`清旧inbox，初始化完成再true；
- handler不直接执行长耗时动作，只投递任务队列。

## 验收

本机/远端相同输入应产生相同业务结果；覆盖任务not-ready、Q0满、Q1覆盖、远端断链、重复命令、过期命令、Result丢失和任务重启。
