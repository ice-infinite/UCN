#ifndef UCN_INTERNAL_STACK_H
#define UCN_INTERNAL_STACK_H
/* EN: Keep disjoint actor phases outlined: their temporary lifetimes must not
 * merge into the caller under Release/size optimization. Frames remain fully
 * measured by the unchanged stack gates; this does not suppress diagnostics.
 * ZH: 保持独立Actor阶段不内联，避免优化将临时量生命周期并入上层。
 * 所有帧仍由原门限完整测量，不关闭或抑制任何栈诊断。 */
#if defined(_MSC_VER)
#define UCN_I_STACK_PHASE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_I_STACK_PHASE __attribute__((noinline))
#else
#define UCN_I_STACK_PHASE
#endif
#endif
