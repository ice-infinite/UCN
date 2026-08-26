# CMake 目标与选项表

主要选项见[官方 CMake 文档](../../official/08-配置与资源/03-CMake选项、静态库与Feature开关.md)。关键边界：测试/规模模拟默认可开；M10 Takeover、M11 Handover、M13 Rekey 实验 archive 默认 OFF；Wire v4 release gate 与 encoder 不能泄漏到生产 target。

精确 target 名称和 source list 以当前 `CMakeLists.txt` 为准，发布证据应保存 CMake configure 输出。
