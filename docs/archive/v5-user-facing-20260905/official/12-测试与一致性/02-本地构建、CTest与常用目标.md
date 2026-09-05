# 本地构建、CTest 与常用目标

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

```powershell
cmake -S . -B build_gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUCN_BUILD_TESTS=ON
cmake --build build_gcc
ctest --test-dir build_gcc --output-on-failure
```

MSVC 多配置生成器运行 CTest 时加 `-C Debug` 或 `-C Release`。WSL sanitizer/analyzer 使用独立构建目录和工具链参数，不复用 Windows 对象。

Profile、Service-OFF、实验 Archive 和产品配置分别建目录。测试数会随开关变化，报告应记录实际发现的 tests，而非照抄历史数字。

## Windows GCC/Ninja

```powershell
cmake -S . -B build/full-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DUCN_PROFILE=FULL `
  -DUCN_BUILD_TESTS=ON
cmake --build build/full-debug
ctest --test-dir build/full-debug --output-on-failure
```

Release另建目录并设`CMAKE_BUILD_TYPE=Release`。不要只切变量复用旧对象。

## MSVC

```powershell
cmake -S . -B build/msvc -DUCN_BUILD_TESTS=ON
cmake --build build/msvc --config Debug
ctest --test-dir build/msvc -C Debug --output-on-failure
cmake --build build/msvc --config Release
ctest --test-dir build/msvc -C Release --output-on-failure
```

## Profile/Feature矩阵

```powershell
cmake -S . -B build/lite -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUCN_PROFILE=LITE
cmake -S . -B build/nano -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUCN_PROFILE=NANO
cmake -S . -B build/service-off -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DUCN_PROFILE=FULL -DUCN_FEATURE_SERVICE=OFF
```

每个目录分别build/ctest。实验M10/M11/M13只在命名明确的目录开启，对应默认OFF目录还要检查archive不含其符号。

## 选择测试

```powershell
ctest --test-dir build/full-debug -N
ctest --test-dir build/full-debug -R "cluster|wire_v4" --output-on-failure
ctest --test-dir build/full-debug --repeat until-fail:100
```

`-N`记录实际测试清单；`-R`用于定向复审；重复测试适合找调度/状态污染，但固定测试没有随机输入时重复通过不等于更高覆盖。

## 干净性

构建前记录`git status --short`和HEAD。测试生成物放build/results，不写源码目录。`git diff --check`只查空白；还要查看是否有意外新文件/生成源码被修改。

## 失败处理

保留失败目录和日志，先单跑失败test，再用相同配置重现。不要在Debug失败后换Release或删断言让矩阵变绿；修复后运行定向+全量+受影响Profile。
