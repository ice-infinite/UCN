# UCN V6-03 Core Wire 隔离 Codec 实现报告

> 状态：实现与分项自审完成；最终统一外审延期。
> 生产状态：default-OFF，不接入 v5 Node/Adapter/Service RX/TX。

## 1. 实现内容

新增 `ucn_v6_wire` 静态库、公共 semantic frame 和严格 Codec。实现固定：

- A0～A3 地址宽度和 40/42/44/46 B 基础 Frame 开销；
- 8 B 公共前缀、固定 Realm/双 Binding/Session/Sequence/Payload Length；
- Peer Hop、Group、E2E、Protocol、Message、Route、Path、Hop Budget 的唯一扩展顺序；
- Payload、16 B E2E Tag、16 B Peer/Group Tag、CRC32C 的唯一 Trailer 顺序；
- 80 B canonical E2E AAD；
- Bootstrap、Group HELLO 和普通 Peer Frame 三类保留地址/上下文规则；
- 65,669 B 理论最大 Frame 的输入扫描上限。

精确 Offset、Flag、Golden 和 AAD 表见
[UCN v6 Core Wire 精确格式 RFC](../../10-理论与规划/建议方案/UCN_v6_Core_Wire_精确格式_RFC.md)。

## 2. 关键安全边界

Decoder 的处理顺序是最小长度/Magic/Version、理论最大长度、CRC32C、结构读取、semantic
合同和精确总长度。所有解析先写局部对象，最终成功才覆盖调用方输出。

Encoder 先完成全部 semantic 校验和精确容量计算，再写入 output。非法 Class/Type/Role、
保留 Traffic bit、双 Peer/Group、双 Route/Path、缺失 Context 的脏字段、零 selector、非法
Operation ID、反向 Budget 和保留地址误用均失败关闭。

当前 `suite_id=1` 只冻结 selector 和 16 B Tag 长度；Codec 不验证 Tag。V6-07 才拥有真实
密码 registry、Key lookup、Replay Window、ACL 和生产接线。任何模块不得把“结构合法”解释为
“已认证”或“有 Authority”。

## 3. Golden 与 AAD

四条 Bootstrap Golden 分别为 42/44/46/48 B，测试把 encoder 完整输出与独立常量逐字节
比较。复杂 Data fixture 同时覆盖 Peer、E2E、Message、Path 和 Budget，再对全部具名字段、
Payload 和两个 Tag 往返校验；另有 Peer Control+Route 与 Group HELLO 正向路径。

80 B AAD 使用独立固定向量。Hop Limit、Peer Key、remaining budget 和 Link Tag 改变时 AAD
保持不变；initial budget 改变时 AAD 必须改变。A0～A3 地址进入 AAD 前统一扩展为 32-bit，
Address Class 仍单独认证。

## 4. 分项自审整改

实现过程中自审并关闭：

1. 最初把基础固定字段误算为 30 B；逐字段 Offset 复核后修正为“不含地址的 22 B”，恢复
   RFC 冻结的 40/42/44/46 B。相较最初草案增加的 4 B 来自独立
   `origin_sequence` 与 `hop_sequence`：前者属于源到目标 E2E 重放域，后者由每一跳重签，
   避免同一下一跳承载多个最终目标时互相误判重放。
2. 最初 AAD 常量按 70 B 估算；逐 Offset 展开后确认完整固定序列为 80 B，并增加独立向量。
3. Decoder 的 payload/tag bounds 初版把 CRC 余量重复扣除，导致合法 Golden 被拒绝；现统一
   以 CRC 起点为 limit，最后再精确验证 `offset+4==input_length`。
4. 增加 32/64-bit 序列冻结上限和 CRC 前理论最大 Frame 门，避免回绕保留值和无界扫描。
5. absent Context 同时要求对应 semantic 字段与 Tag 为零，消除两种结构映射到同一 Wire 的
   非 canonical 表达。

## 5. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | 60/60 |
| Windows GCC Nano/Lite v6 Wire 定向 | 1/1、1/1 |
| Windows GCC Full Release 定向 | 1/1 |
| MSVC 19.29 `/W4 /WX` 定向 | 1/1 |
| WSL ASan/UBSan 定向 | 1/1 |
| WSL `-fanalyzer -Werror` 定向 | 1/1 |
| fixed-seed raw fuzz | 4096 次；失败不写回，合法输入逐字节 canonical 往返 |

默认 `UCN_BUILD_V6_EXPERIMENTAL=OFF` 时，v5 生产 archive 中仍无 `ucn_v6_*` 符号。V6-03
完成不解除生产门禁，也不代表真实密码、Carrier、MCU MTU 或实机链路已经验证。

## 6. 后续依赖

- V6-04 使用 Message/Endpoint Context，但不得改变已冻结 Offset。
- V6-05 提供 opaque storage、唯一 Manifest 和 Owner。
- V6-07 实现并外审 Security/JOIN 后，才允许评估生产 RX/TX 接线。
- 旧 v4/v5 Codec 仍为当前 v5 回归基座；只在 V6-15 最终切换后删除，Compatibility Manifest
  在此之前保持 PENDING，防止把“已有替代物”误写成“旧资产已销账”。
