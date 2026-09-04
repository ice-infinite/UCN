# UCN v6 实机证据目录

本目录只接收能够绑定到具体 Git commit、固件哈希、板卡身份和原始日志哈希的实机证据。
Host Fake、模拟器、单元测试或口头观察不能写成实机 PASS。

每次实测应建立独立子目录，并至少保存：

- `manifest.json`：使用 `tools/v6/validate_v6_evidence.py` 校验；
- 固件构建信息与每块板实际刷入镜像的 SHA-256；
- 板卡型号、芯片、端口、连线、供电、Bearer 和速率配置；
- 原始串口/抓包/故障注入/功耗/温度日志；
- 测试起止时间、持续时长、通过阈值和实际结果；
- 断电、复位、断链、重连、丢包、乱序和并发场景的逐项结果。

`manifest.json` 的顶层格式为：

```json
{
  "schema": 1,
  "protocol": "UCN-v6",
  "source_commit": "40位小写Git提交哈希",
  "release_ready": false,
  "gates": [
    {
      "id": "esp32s3-uart-six-node-24h",
      "required": true,
      "status": "HOLD",
      "artifact": "",
      "sha256": ""
    }
  ]
}
```

只有 `PASS` 项可以引用 artifact，且必须填写该文件的 SHA-256。`release_ready` 必须与
“全部 required gate 均为 PASS”严格一致；缺少任一必需硬件证据时，校验器必须返回 HOLD，
不得生成 UCN 1.0 RC 结论。
