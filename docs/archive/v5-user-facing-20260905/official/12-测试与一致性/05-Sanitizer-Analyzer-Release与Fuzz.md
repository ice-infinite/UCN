# Sanitizer、Analyzer、Release 与 Fuzz

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

- ASan/UBSan：独立干净构建，运行全部可用 tests；
- `-fanalyzer -Wall -Wextra -Werror`：检查未初始化、越界和错误路径；
- MSVC/GCC Release：覆盖 padding、未初始化输出和优化相关行为；
- deterministic fuzz：固定 seed，记录成功/失败 oracle；
- `git diff --check`：只检查空白，不代表逻辑正确。

曾经 Debug 通过而 Release 因 padding `memcmp` 失败，说明优化构建必须是正式门禁。比较公开结构时优先逐字段或 canonical 编码，不比较未定义 padding。

## Sanitizer

ASan发现越界/UAF，UBSan发现未定义算术、对齐、非法shift等。使用干净GCC/Clang构建，运行所有适用测试和模拟；若自定义allocator/embedded stub屏蔽sanitizer，报告必须注明。

```bash
cmake -S . -B build/asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure
```

Windows和WSL对象不可混用。

## Analyzer与Warnings

`-fanalyzer -Wall -Wextra -Wpedantic -Werror`用于错误路径、未初始化和资源泄漏。Analyzer误报需最小化并有注释/抑制理由，不能全局关闭类别。MSVC warning也应纳入，注意中文源文件编码警告与逻辑警告分开处理。

## Release/O1～O3

至少运行MSVC/GCC Release；Wire/serialization关键模块建议O1/O2/O3。Release会暴露未初始化padding、别名、越界优化和测试依赖Debug清零。所有输出对象先初始化，失败不写回用完整哨兵memcmp验证。

## Fuzz

确定性fuzz保存固定seed、迭代数和oracle。输入包括随机frame、合法frame单bit/字段变异、长度39/40/41、状态序列乱序。Fuzz成功路径也要验证输出合法，不是“没崩溃”即可。

发现case后把最小输入固化为named regression，避免只依赖随机再次命中。

## 静态对象比较

C结构padding内容未定义。协议对象一致性用逐字段或canonical encode；只有明确`memset`且结构是内部完全控制时才可memcmp。测试fixture每个可写字段使用不同合法值，防止parser/builder同时交换字段仍round-trip通过。

## 局限

Sanitizer不模拟ISR时序/Flash掉电；Analyzer不证明算法；Fuzz不能穷举32-bit状态；Release Host不代表MCU编译器。它们是互补门禁，不能互相替代。
