# UCN V5 C05 四节点自动分簇实测报告

> 日期：2026-08-15  
> 状态：部分完成。自动选举、成员加入、容量边界、四轮 Head 故障恢复、120 秒稳定性和 MCU RAM/CPU 已实测；独立 Bearer 插拔、小时级长稳、功耗与生产安全未完成。

## 台架与固件

四块 ESP32-S3-N16R8 使用 3 Mbaud UART 组成单一路径：

```text
A/COM47 ── B/COM48 ── C/COM46 ── D/COM40
```

| 角色 | Node ID | UART 接线 | Head Score |
| --- | --- | --- | ---: |
| A | `0x10000001` | GPIO19/20 ↔ B GPIO20/19 | 7000 |
| B | `0x10000002` | GPIO19/20 ↔ A GPIO20/19；GPIO15/16 ↔ C GPIO16/15 | 9000 |
| C | `0x10000003` | GPIO15/16 ↔ B GPIO16/15；GPIO19/20 ↔ D GPIO20/19 | 8000 |
| D | `0x10000004` | GPIO19/20 ↔ C GPIO20/19 | 6000 |

测试使用 `codex/v5-adaptive-wire`、基线提交
`5936fc7447b6ea6fe8445198f4c13e44d152bba8` 加当时未提交的 C00～C04 工作区。
Cluster 使用 Endpoint `0xA0`、格式 1、固定 28 B Payload，不依赖 Linux。

常规容量 2 固件最终资源与 SHA-256：

| 角色 | RAM | Flash | `firmware.bin` SHA-256 |
| --- | ---: | ---: | --- |
| A | 57,012 B | 604,203 B | `617346DBC2BB94C3C805B737E1B13EAF3BE0E0EB3E62FA3B9333D337FC6DCF0E` |
| B | 50,748 B | 599,435 B | `8242210495516DEAA525C64ACDEFAB804C90C91AEF744AF318FB564DE01091D3` |
| C | 50,748 B | 599,439 B | `593658320BA9D0B6231D1C6D969D239AAFD244ABDDEB7CDC35BD3F013B99C034` |
| D | 56,996 B | 603,647 B | `CC21807CFB091D88A31FA66B7C715A355D758B87B9ECCFA2E02002BEC6576F12` |

四块板均完成整镜像写入和 Hash 校验；容量 1 专项结束后已恢复为常规容量 2 固件。
板上 `sizeof(ucn_cluster_t)=860 B`，而 Host x64 为 880 B，说明产品资源必须以目标 ABI 实测为准。

## 结果

### 自动选举与容量

- 约 5.31 s：四节点进入 Candidate；
- 约 8.31 s：一跳最高分 B 成为 Head；
- 约 8.33/8.84 s：A/C 加入 B；
- 约 16.32 s：D 成为独立 Head。

Cluster 只使用一跳已准入邻居。D 不能越过已加入 B 的 C 成为 B 的直接成员，但普通
UCN 数据面 A→D 仍可走三跳 Route。

容量改为 1 后，B 接纳 A 并在 Offer 中广播满员；C 不发送必然失败的 Join，而是成为
第二个 Head，D 加入 C。最终严格形成 `{B,A}` 与 `{C,D}`，两个 Head 均未超额。
显式满簇 `JOIN_REJECT` 仍由软件单测覆盖。

### Head 故障恢复

共执行一次 20 秒和三次独立 12 秒 B-Head 复位：

- 4/4 均恢复；
- 重复三轮中 A/C 在切断后约 7.2～7.8 s 发现 Head 租约过期；
- C 加入 D，A 重新成为 Head；
- B 释放后约 3.7 s 作为 Member 加入 A；
- 物理链路恢复后的 A→D 首个冷路由包为 264～266 ms，随后为 2～3 ms；
- malformed/security/stale/API failure 均为 0。

B 同时是 Cluster Head 和物理中继，保持 B 复位会真实分割 A 与 C/D；故障窗口内数据
不可达不是 Cluster 控制流额外造成的丢包。恢复后的高分 B 不立即抢占已工作的 A，
当前首阶段语义偏向稳定；是否在最低任期后恢复最优 Head 归 C07 生命周期策略。

### 120 秒稳定性与资源

- 收敛后没有额外角色变化、租约误过期或 Switch；
- A 的 156 个 Ping 发送拒绝为 0，156 个回复全部收到；
- 稳态 A→B 约 0～1 ms，A→D 约 2～3 ms，首次三跳寻路约 270 ms；
- UART dropped/no-space/decode/length/overflow 为 0；
- 最低 Heap A/B/C/D=`328140/333364/333068/328156 B`，无下降趋势；
- 115 秒快照 Cluster CPU 累计折算约 0.037%～0.069% 单核时间，单次最大 165～225 us。

每 5 秒同步输出大量诊断会使个别单跳 Ping 达 24～45 ms；这属于测试日志扰动，不是
无日志产品模式的硬实时结论。少量 `rejected_by_core` 与约 30 秒路由缓存刷新一致：帧已
成功入队，但 Core 对控制帧返回非 OK；它不是 RX Queue 满，本轮业务 Ping 未丢。后续
应继续按帧型/返回码细分该统计。

## 边界与后续

1. 每个邻居当前只有一条 UART Bearer；独立 Bearer 断开/恢复和冗余介质切换未实测。
2. 120 秒不替代 1 h/8 h/24 h 温度、持续负载与老化测试。
3. 未接功耗仪表，不能填写空闲、选举、稳态和故障恢复电流。
4. 测试关闭 Cluster 控制保护；生产身份、AEAD、密钥与抗重放仍受 S02/S26 门禁约束。
5. 备用 Head、优选 Head 恢复、拆分/合并和独立 Token Bucket 归 C07；簇间
   Locator/Directory/Tunnel 归 C06。

原始串口证据保存在外部 Bench 的 `test_results/c05_cluster_*_20260815.log`；Bench
侧报告还记录容量 1 固件 Hash 和逐文件日志索引。
