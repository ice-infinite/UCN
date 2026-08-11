# UCN V5-02 Node 固定域档位实现报告

> 日期：2026-08-11  
> 范围：固定发送 Wire Profile、最大接收 Profile、字段与 MTU 门禁；不包含自动选档和跨域 Alias。
>
> 当前一致性说明：本文描述固定模式。Auto 控制面、per-Link RX Ceiling、Ingress Peek 与运行期 Hop Scope 已分别由 V5-12/V5-13/V5-20/V5-25 补齐，不能再用本文的阶段性顺序描述整个当前接收链。

## 结论

Node 初始化继续默认 W3，保持既有三字段 `ucn_config_t` 调用安全。产品可在注册 Link 或安装 Security Provider 前调用 `ucn_node_set_wire_profiles()`，固定本节点新发帧档位与允许接收上限；Core 不分配动态内存，也不截断超范围值。

接收上限可以大于发送档位，例如 W0 TX/W3 RX 的边缘节点；发送档位不能大于接收上限。已注册 Link 或已安装 Security 后禁止改变固定域，避免运行中静默改变地址/Session 语义。

## 失败关闭门禁

- TX：固定模式下所有新发业务和控制帧使用 Node 固定档位；Auto 模式下当前 HELLO/Heartbeat、RREQ/RREP、RERR、Path 与诊断控制面按 V5-12/V5-15 的规则选择或继承档位。透明中继始终保留入站明确档位。
- RX：当前 Full/Lite/Nano 都先用 3 B Prefix Peek 按 Node/per-Link RX Ceiling 早拒绝，再做完整 Decode/CRC；Network 通过后还要在 Security/状态修改前执行运行期 Hop Scope。
- 范围：Network、Node、Hop、Plain/Provider Session 按发送档描述符校验；W0 单播 Node 只允许 1～254。
- MTU：Link 注册下限按最大接收档基础头计算，不再固定要求 30 B。
- Route Epoch：W0 本地产生值约束在 1 B；更宽档保持 2 B。
- Nano：与 Lite/Full 使用同一公共 API、固定档位与门禁语义。

## 软件证据

`test_node_wire_profile.c` 覆盖 W0/W1/W2/W3 双节点编码长度与解码档位、默认 W3、错误档位组合、配置冻结、Session/Network/Node/Hop 越界、按最大接收档 MTU和 W0/W0 拒绝 W1。V5-08 又补充了 W0 TX/W3 RX Node 依次接收并分发 W0～W3 静态 Endpoint 指令，Nano/Lite/Full 均执行同一行为测试。

`test_aodv_lite.c` 已改为 W0 固定域，覆盖 A→B→C 的 RREQ/RREP、业务转发、断链 RERR 与重新寻路。Windows Debug 结果：Full/Service ON 2/2、Lite/Service ON 2/2、Nano/Service OFF 1/1。

上述 `2/2` 是 V5-02 当时的里程碑。V5-33 当前回归为 Full/Lite `10/10`、Nano `1/1`，WSL Full ASan+UBSan 与配置契约 `13/13`，另有 GCC `-fanalyzer` 与配置契约 `13/13`；固定档语义仍在正式测试中持续覆盖。

这些是 Host 软件证据；ESP32/STM32 的实际 RAM、Flash、栈、吞吐、无线丢包和长时间稳定性仍需 V5-07/S06 实机验证。
