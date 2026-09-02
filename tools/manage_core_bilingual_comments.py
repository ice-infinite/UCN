#!/usr/bin/env python3
"""Check or add bilingual comments for every non-Cluster UCN function.

The inventory is taken from GCC ``-aux-info`` under the real Nano, Lite and
Full feature profiles.  This avoids treating disabled ``#if`` bodies as
documentation-complete and keeps Cluster outside the current maintenance
scope.  ``--apply`` performs one deterministic mechanical insertion; the
default mode is a read-only release gate.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile
from dataclasses import dataclass


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "src"
HEADER_ROOT = ROOT / "include" / "ucn"
PROFILE_VALUES = (1, 2, 3)
AUX_PATTERN = re.compile(
    r"^/\* (?P<source>.+):(?P<line>\d+):NF \*/ "
    r"(?P<visibility>static|extern) (?P<signature>.*?); /\*"
)
FUNCTION_NAME_PATTERN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
INLINE_PATTERN = re.compile(
    r"^\s*static\s+inline\s+.*?\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\("
)


@dataclass(frozen=True, order=True)
class FunctionDefinition:
    source: str
    line: int
    name: str
    visibility: str


MODULES: tuple[tuple[str, str, str, tuple[str, ...]], ...] = (
    ("src/core/ucn_core.c", "Core configuration", "Core 配置", ("ucn_",)),
    ("src/core/ucn_endpoint.c", "Endpoint classification", "Endpoint 分类", ("ucn_",)),
    ("src/core/ucn_frame.c", "Wire frame codec", "Wire 帧编解码", ("ucn_frame_", "frame_", "ucn_")),
    ("src/core/ucn_link_cost.c", "Link Cost", "Link Cost", ("ucn_link_cost_", "ucn_")),
    ("src/transport/ucn_adapter.c", "Adapter", "Adapter", ("ucn_adapter_", "hello_", "queue_", "ucn_")),
    ("src/transport/ucn_event_runtime.c", "Event Runtime", "Event Runtime", ("ucn_event_runtime_", "runtime_", "ucn_")),
    ("src/transport/ucn_standard_adapter.c", "standard Adapter", "标准 Adapter", ("ucn_standard_adapter_", "standard_", "ucn_")),
    ("src/transport/ucn_protocol_owner.c", "Protocol Owner", "Protocol Owner", ("ucn_protocol_owner_", "protocol_owner_", "ucn_")),
    ("src/adapters/can/ucn_can_source.c", "CAN Source", "CAN Source", ("ucn_can_source_", "ucn_can_", "can_", "ucn_")),
    ("src/adapters/stream/ucn_stream_source.c", "Stream Source", "Stream Source", ("ucn_stream_source_", "ucn_stream_", "stream_", "ucn_")),
    ("src/service/ucn_service.c", "Service Router", "Service Router", ("ucn_service_", "service_", "ucn_")),
    ("src/service/ucn_service_bridge.c", "Service Bridge", "Service Bridge", ("ucn_service_protocol_bridge_", "ucn_service_", "bridge_", "service_", "ucn_")),
    ("src/node/ucn_node_nano.c", "Nano Node", "Nano Node", ("ucn_node_", "nano_", "ucn_")),
    ("src/node/ucn_node.c", "Lite/Full Node", "Lite/Full Node", ("ucn_node_", "ucn_")),
    ("src/node/ucn_profile_stubs.c", "Profile compatibility Stub", "Profile 兼容 Stub", ("ucn_node_", "ucn_policy_", "ucn_path_", "ucn_")),
    ("src/routing/ucn_path.c", "Path forwarding", "Path 转发", ("ucn_path_", "path_", "ucn_")),
    ("src/routing/ucn_policy.c", "routing Policy", "路由 Policy", ("ucn_node_", "ucn_policy_", "policy_", "ucn_")),
    ("src/extended/ucn_transfer.c", "Transfer", "Transfer", ("ucn_transfer_", "transfer_", "ucn_")),
    ("src/ports/ucn_port_bare_metal.c", "bare-metal Port", "裸机 Port", ("ucn_bare_metal_port_", "ucn_")),
    ("src/ports/ucn_port_freertos.c", "FreeRTOS Port", "FreeRTOS Port", ("ucn_freertos_port_", "ucn_")),
    ("src/ports/ucn_port_host_fake.c", "Host fake Port", "Host fake Port", ("ucn_host_fake_port_", "ucn_")),
    ("src/ports/ucn_port_nuttx.c", "NuttX Port", "NuttX Port", ("ucn_nuttx_port_", "ucn_")),
    ("src/ports/ucn_port_rtthread.c", "RT-Thread Port", "RT-Thread Port", ("ucn_rtthread_port_", "ucn_")),
    ("src/ports/ucn_port_zephyr.c", "Zephyr Port", "Zephyr Port", ("ucn_zephyr_port_", "ucn_")),
    ("include/ucn/ucn_link.h", "Link contract", "Link 合同", ("ucn_link_", "ucn_")),
    ("include/ucn/ucn_port.h", "Port contract", "Port 合同", ("ucn_port_", "ucn_")),
    ("include/ucn/ucn_time.h", "wrap-safe time", "回绕安全时间", ("ucn_",)),
)


EXACT_SUMMARIES: dict[str, tuple[str, str]] = {
    "ucn_version": (
        "Returns the immutable UCN library version string.",
        "返回不可变的 UCN 库版本字符串。",
    ),
    "ucn_validate_config": (
        "Validates the immutable Node identity and hop-limit configuration.",
        "验证不可变的 Node 身份与跳数上限配置。",
    ),
    "ucn_endpoint_is_static": (
        "Checks whether an Endpoint belongs to the statically assigned application range.",
        "检查 Endpoint 是否属于静态分配的应用区间。",
    ),
    "ucn_message_type_is_control": (
        "Checks whether a message type belongs to the UCN control plane.",
        "检查消息类型是否属于 UCN 控制面。",
    ),
    "ucn_crc16_ccitt": (
        "Calculates the incremental CRC-16/CCITT value used by Core frames.",
        "计算 Core 帧使用的可增量 CRC-16/CCITT 值。",
    ),
    "ucn_node_step": (
        "Advances one bounded Node maintenance and transmit scheduling cycle.",
        "推进一次有界的 Node 维护与发送调度周期。",
    ),
    "ucn_node_init": (
        "Initializes a Node instance from validated caller-owned configuration and fixed storage.",
        "使用经验证的调用方配置与固定存储初始化一个 Node 实例。",
    ),
    "ucn_node_send": (
        "Validates and submits one application message to the Node transmit and routing pipeline.",
        "验证一个应用消息，并将其提交到 Node 发送与路由流水线。",
    ),
    "ucn_node_receive": (
        "Validates one received frame and routes, forwards, or locally delivers it.",
        "验证一个接收帧，并对其进行路由、转发或本地投递。",
    ),
    "ucn_transfer_init": (
        "Initializes the bounded Transfer sender and reassembly state around an existing Node.",
        "围绕现有 Node 初始化有界的 Transfer 发送与重组状态。",
    ),
    "ucn_transfer_send": (
        "Starts one bounded message transfer and chooses its class, fragmentation, and retry state.",
        "启动一次有界消息传输，并确定其等级、分片与重试状态。",
    ),
    "ucn_transfer_step": (
        "Advances at most one bounded Transfer fragment, retry, or completion action.",
        "推进至多一个有界的 Transfer 分片、重试或完成动作。",
    ),
    "ucn_link_effective_mtu": (
        "Returns the smaller usable MTU reported by the Link and its current status.",
        "返回 Link 静态上限与当前状态上限中较小的可用 MTU。",
    ),
    "ucn_elapsed_at_least": (
        "Checks a wrap-safe elapsed interval against a validated duration.",
        "使用回绕安全算法检查已过时间是否达到合法时长。",
    ),
    "ucn_stream_source_free_bytes": (
        "Returns the current free-byte capacity of the Stream Source ring.",
        "返回 Stream Source 环形缓冲当前可用的字节容量。",
    ),
    "version_profile_byte": (
        "Builds the packed protocol-version and Wire-Profile header byte.",
        "构造协议版本与 Wire Profile 组合后的头部字节。",
    ),
    "ucn_link_cost_ewma_update": (
        "Updates a Link Cost sample with the fixed-point EWMA rule.",
        "使用定点 EWMA 规则更新 Link Cost 样本。",
    ),
    "remap_neighbor_egress_references": (
        "Remaps routes and Path references after a neighbor changes its primary Bearer.",
        "在邻居切换主 Bearer 后重映射 Route 与 Path 引用。",
    ),
    "switch_neighbor_primary": (
        "Atomically switches a neighbor to a verified primary Bearer and remaps dependents.",
        "把邻居原子切换到已验证的主 Bearer，并重映射依赖项。",
    ),
    "include_link_in_path_capability": (
        "Reduces a Path capability to the bottleneck imposed by one additional Link.",
        "根据新增 Link 的瓶颈收缩 Path 能力。",
    ),
    "path_expires_at": (
        "Creates the wrap-safe expiration deadline for a Path lease.",
        "为 Path 租约生成回绕安全的到期时间。",
    ),
    "policy_diagnostic_build_reply": (
        "Builds one bounded Policy diagnostic reply page.",
        "构造一页有界的 Policy 诊断响应。",
    ),
    "ucn_node_wire_profile_auto": (
        "Returns whether route-aware automatic Wire-Profile selection is enabled.",
        "返回是否已启用路由感知的自动 Wire Profile 选择。",
    ),
    "auto_balance_select_path": (
        "Selects an eligible Path from the bounded automatic-balance candidate set.",
        "从固定容量的自动均衡候选集中选择合格 Path。",
    ),
    "nano_link_status": (
        "Reads and validates the current status of a Nano Link.",
        "读取并验证 Nano Link 的当前状态。",
    ),
    "policy_ewma": (
        "Applies the fixed-point EWMA used by routing Policy quality samples.",
        "应用路由 Policy 质量样本使用的定点 EWMA。",
    ),
    "ucn_service_acceptance_stage": (
        "Maps a synchronous Service acceptance result to its asynchronous stage.",
        "把同步 Service 接受结果映射为对应的异步阶段。",
    ),
    "ucn_service_command_guard_validate": (
        "Validates a command against the bounded Service idempotency guard.",
        "使用固定容量的 Service 幂等 Guard 验证命令。",
    ),
    "ucn_service_protocol_bridge_endpoint_rx": (
        "Receives a Core Endpoint frame and submits it to the Service Bridge ingress path.",
        "接收 Core Endpoint 帧并提交到 Service Bridge 入站路径。",
    ),
    "ucn_service_bridge_replay_accept_command": (
        "Checks and records a remote command in the Service replay window.",
        "在 Service 重放窗口中检查并记录远端命令。",
    ),
    "ucn_service_bridge_replay_rotate_session": (
        "Rotates the Service replay window to a strictly newer peer Session.",
        "把 Service 重放窗口切换到严格更新的对端 Session。",
    ),
    "hello_random_next": (
        "Advances the deterministic PRNG used for HELLO scheduling jitter.",
        "推进 HELLO 调度抖动使用的确定性伪随机序列。",
    ),
    "hello_seed": (
        "Initializes the deterministic HELLO scheduler random state.",
        "初始化 HELLO Scheduler 的确定性随机状态。",
    ),
    "hello_retry_delay": (
        "Calculates the bounded randomized delay before the next HELLO retry.",
        "计算下一次 HELLO 重试前的有界随机延迟。",
    ),
    "ucn_adapter_hello_scheduler_restart": (
        "Restarts HELLO scheduling with a new seed and current timestamp.",
        "使用新种子和当前时间重新启动 HELLO 调度。",
    ),
    "rearm_source": (
        "Re-arms pending Source events after an Event Runtime budget boundary.",
        "在 Event Runtime 预算边界后重新挂起 Source 事件。",
    ),
    "rearm_owner": (
        "Re-arms pending Owner work after an Event Runtime budget boundary.",
        "在 Event Runtime 预算边界后重新挂起 Owner 工作。",
    ),
    "ucn_event_runtime_run": (
        "Runs one bounded multi-Source drain and Protocol-Owner cycle.",
        "运行一次有界的多 Source 排空与 Protocol Owner 周期。",
    ),
    "ucn_event_runtime_task_cycle": (
        "Waits when idle and then runs one bounded Event Runtime task cycle.",
        "空闲时等待，然后运行一次有界的 Event Runtime 任务周期。",
    ),
    "ucn_standard_preset_resolve": (
        "Resolves a standard Bearer preset into deterministic Link defaults.",
        "把标准 Bearer Preset 解析为确定性的 Link 默认参数。",
    ),
    "ucn_standard_link_config_resolve": (
        "Merges product overrides with a standard Link preset and validates the result.",
        "合并产品覆盖项与标准 Link Preset，并验证最终配置。",
    ),
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default="gcc", help="GCC executable used for -aux-info")
    parser.add_argument("--apply", action="store_true", help="insert missing comments")
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="replace existing four-line generated comments",
    )
    parser.add_argument("--list", action="store_true", help="list every discovered definition")
    return parser.parse_args()


def is_cluster_path(path: pathlib.Path) -> bool:
    relative = path.relative_to(ROOT).as_posix().lower()
    return "/cluster/" in relative or "ucn_cluster" in path.name.lower()


def source_files() -> list[pathlib.Path]:
    return [
        path
        for path in sorted(SOURCE_ROOT.rglob("*.c"))
        if not is_cluster_path(path)
    ]


def profiles_for(source: str) -> tuple[int, ...]:
    if source.endswith("src/node/ucn_node_nano.c"):
        return (1,)
    if source.endswith("src/node/ucn_profile_stubs.c"):
        return (1, 2)
    if source.endswith("src/node/ucn_node.c"):
        return (2, 3)
    if "/routing/" in source:
        return (3,)
    return PROFILE_VALUES


def collect_source(gcc: str, source: str, profile: int,
                   temp_dir: pathlib.Path) -> list[FunctionDefinition]:
    aux = temp_dir / (
        source.replace("/", "_").replace(".c", "") + f"_{profile}.aux"
    )
    command = [
        gcc,
        "-std=c99",
        "-Iinclude",
        f"-DUCN_PROFILE={profile}",
        "-DUCN_FEATURE_SERVICE=1",
        "-aux-info",
        str(aux),
        "-fsyntax-only",
        source,
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    definitions: list[FunctionDefinition] = []
    for raw_line in aux.read_text(encoding="utf-8", errors="replace").splitlines():
        match = AUX_PATTERN.match(raw_line)
        if match is None:
            continue
        recorded = match.group("source").replace("\\", "/")
        if recorded != source:
            continue
        name_match = FUNCTION_NAME_PATTERN.search(match.group("signature"))
        if name_match is None:
            raise RuntimeError(f"cannot extract function name: {raw_line}")
        definitions.append(
            FunctionDefinition(
                source=source,
                line=int(match.group("line")),
                name=name_match.group(1),
                visibility=match.group("visibility"),
            )
        )
    return definitions


def collect_c_definitions(gcc: str) -> list[FunctionDefinition]:
    unique: set[FunctionDefinition] = set()
    with tempfile.TemporaryDirectory(prefix="ucn_core_comments_") as raw_temp:
        temp_dir = pathlib.Path(raw_temp)
        for path in source_files():
            source = path.relative_to(ROOT).as_posix()
            for profile in profiles_for(source):
                unique.update(collect_source(gcc, source, profile, temp_dir))
    return sorted(unique)


def collect_inline_definitions() -> list[FunctionDefinition]:
    definitions: list[FunctionDefinition] = []
    for path in sorted(HEADER_ROOT.rglob("*.h")):
        if is_cluster_path(path):
            continue
        relative = path.relative_to(ROOT).as_posix()
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            match = INLINE_PATTERN.match(line)
            if match is not None:
                definitions.append(
                    FunctionDefinition(
                        source=relative,
                        line=line_number,
                        name=match.group("name"),
                        visibility="inline",
                    )
                )
    return definitions


def module_for(source: str) -> tuple[str, str, tuple[str, ...]]:
    for path, english, chinese, prefixes in MODULES:
        if source == path:
            return english, chinese, prefixes
    raise RuntimeError(f"missing module metadata for {source}")


def operation_name(name: str, prefixes: tuple[str, ...]) -> str:
    for prefix in prefixes:
        if name.startswith(prefix):
            return name[len(prefix):]
    return name


def subject(operation: str, prefix: str = "", suffix: str = "") -> str:
    value = operation
    if prefix and value.startswith(prefix):
        value = value[len(prefix):]
    if suffix and value.endswith(suffix):
        value = value[:-len(suffix)]
    return value.strip("_") or operation


def generated_summary(definition: FunctionDefinition) -> tuple[str, str]:
    if definition.name in EXACT_SUMMARIES:
        return EXACT_SUMMARIES[definition.name]
    module_en, module_cn, prefixes = module_for(definition.source)
    operation = operation_name(definition.name, prefixes)

    module_en_text = f"the {module_en} module"
    module_cn_text = f"{module_cn} 模块"
    if operation == "init":
        return (
            f"Initializes the {module_en} object from validated caller-owned configuration without heap allocation.",
            f"使用经验证的调用方配置初始化 {module_cn} 对象，且不使用堆内存。",
        )
    patterns: tuple[tuple[bool, str, str], ...] = (
        (operation.endswith("_is_valid") or operation.endswith("_are_valid"),
         f"Checks whether `{subject(operation, suffix='_is_valid' if operation.endswith('_is_valid') else '_are_valid')}` satisfies {module_en_text}'s validity rules.",
         f"检查 `{subject(operation, suffix='_is_valid' if operation.endswith('_is_valid') else '_are_valid')}` 是否满足 {module_cn_text}的合法性规则。"),
        (operation.startswith("is_") or operation.startswith("are_"),
         f"Checks the `{operation}` predicate against current {module_en} state.",
         f"根据当前 {module_cn} 状态检查 `{operation}` 条件。"),
        (operation.startswith("validate_"),
         f"Validates `{subject(operation, prefix='validate_')}` before {module_en} state is used or changed.",
         f"在使用或修改 {module_cn} 状态前验证 `{subject(operation, prefix='validate_')}`。"),
        (operation.startswith(("enter_", "exit_", "lock_", "unlock_")),
         f"Enters or leaves the bounded `{operation}` critical section for {module_en}.",
         f"进入或退出 {module_cn} 的有界 `{operation}` 临界区。"),
        (operation.startswith("find_") or operation.endswith("_find"),
         f"Searches bounded {module_en} state for `{subject(operation, prefix='find_', suffix='_find')}`.",
         f"在固定容量的 {module_cn} 状态中查找 `{subject(operation, prefix='find_', suffix='_find')}`。"),
        (operation.startswith("get_") or operation.endswith("_get"),
         f"Returns the current `{subject(operation, prefix='get_', suffix='_get')}` view from {module_en} state.",
         f"从 {module_cn} 状态返回当前 `{subject(operation, prefix='get_', suffix='_get')}` 视图。"),
        (operation.startswith("copy_"),
         f"Copies `{subject(operation, prefix='copy_')}` from {module_en} into caller-owned storage.",
         f"把 {module_cn} 中的 `{subject(operation, prefix='copy_')}` 复制到调用方存储。"),
        (operation.startswith("init") or operation.endswith("_init") or operation.startswith("initialize_"),
         f"Initializes `{operation}` for {module_en} using caller-owned fixed storage.",
         f"使用调用方提供的固定存储初始化 {module_cn} 的 `{operation}`。"),
        (operation.startswith("reset_") or operation.endswith("_reset"),
         f"Resets `{subject(operation, prefix='reset_', suffix='_reset')}` to its canonical {module_en} state.",
         f"把 `{subject(operation, prefix='reset_', suffix='_reset')}` 重置为规范的 {module_cn} 状态。"),
        (operation.startswith("clear_") or operation.startswith("purge_"),
         f"Clears `{subject(operation, prefix='clear_')}` from {module_en} without allocating memory.",
         f"从 {module_cn} 中清除 `{subject(operation, prefix='clear_')}`，且不进行动态分配。"),
        (operation.startswith("set_") or operation.startswith("configure_"),
         f"Validates and sets `{subject(operation, prefix='set_')}` in {module_en} state.",
         f"验证并设置 {module_cn} 状态中的 `{subject(operation, prefix='set_')}`。"),
        (operation.startswith(("add_", "register_", "bind_", "install_", "admit_")),
         f"Validates and installs `{operation}` into bounded {module_en} state.",
         f"验证 `{operation}` 并将其安装到固定容量的 {module_cn} 状态中。"),
        (operation.startswith(("remove_", "revoke_", "reject_", "release_")),
         f"Removes or releases `{operation}` from {module_en} state with bounded work.",
         f"以有界工作量从 {module_cn} 状态移除或释放 `{operation}`。"),
        (operation.startswith(("encode", "serialize")) or operation.endswith("_encode"),
         f"Encodes `{operation}` into its bounded {module_en} wire representation.",
         f"把 `{operation}` 编码为有界的 {module_cn} 线格式。"),
        (operation.startswith(("decode", "deserialize")) or operation.endswith("_decode"),
         f"Decodes and validates `{operation}` from its {module_en} wire representation.",
         f"从 {module_cn} 线格式解码并验证 `{operation}`。"),
        (operation.startswith("peek_"),
         f"Inspects `{subject(operation, prefix='peek_')}` without completing a full {module_en} decode.",
         f"在不完成完整 {module_cn} 解码的情况下检查 `{subject(operation, prefix='peek_')}`。"),
        (operation.startswith("write_"),
         f"Writes `{subject(operation, prefix='write_')}` in the canonical {module_en} byte order.",
         f"按规范的 {module_cn} 字节序写入 `{subject(operation, prefix='write_')}`。"),
        (operation.startswith("read_"),
         f"Reads `{subject(operation, prefix='read_')}` from the canonical {module_en} byte order.",
         f"按规范的 {module_cn} 字节序读取 `{subject(operation, prefix='read_')}`。"),
        (operation.startswith(("send", "transmit", "emit")),
         f"Validates and submits `{operation}` through the bounded {module_en} transmit path.",
         f"验证 `{operation}` 并将其提交到有界的 {module_cn} 发送路径。"),
        (operation.startswith("enqueue") or "_enqueue" in operation,
         f"Copies `{operation}` into a bounded {module_en} queue.",
         f"把 `{operation}` 复制到固定容量的 {module_cn} 队列。"),
        (operation.startswith(("receive", "handle_", "process_", "dispatch_")),
         f"Validates and processes `{operation}` in the {module_en} receive path.",
         f"在 {module_cn} 接收路径中验证并处理 `{operation}`。"),
        (operation.startswith(("forward", "deliver")),
         f"Forwards or delivers `{operation}` through the bounded {module_en} path.",
         f"通过有界的 {module_cn} 路径转发或投递 `{operation}`。"),
        (operation.startswith("step") or operation.endswith("_step"),
         f"Advances one bounded `{operation}` state-machine step in {module_en}.",
         f"在 {module_cn} 中推进一次有界的 `{operation}` 状态机步骤。"),
        (operation.startswith(("poll", "service", "drain", "pump")),
         f"Processes one bounded batch of `{operation}` work for {module_en}.",
         f"为 {module_cn} 处理一批有界的 `{operation}` 工作。"),
        (operation.startswith(("select", "resolve", "choose")),
         f"Selects or resolves `{operation}` using deterministic {module_en} rules.",
         f"按照确定性的 {module_cn} 规则选择或解析 `{operation}`。"),
        (operation.startswith(("update", "refresh", "record", "observe", "learn", "mark")),
         f"Updates `{operation}` in bounded {module_en} state.",
         f"更新固定容量 {module_cn} 状态中的 `{operation}`。"),
        (operation.startswith(("schedule", "arm", "reschedule")),
         f"Schedules `{operation}` using the wrap-safe {module_en} time domain.",
         f"使用回绕安全的 {module_cn} 时间域调度 `{operation}`。"),
        (operation.startswith(("expire", "prune")) or operation.endswith("_expired"),
         f"Checks or removes expired `{operation}` state in {module_en}.",
         f"检查或移除 {module_cn} 中已过期的 `{operation}` 状态。"),
        (operation.startswith(("compare", "equal")) or operation.endswith("_equal"),
         f"Compares `{operation}` using the canonical {module_en} identity rules.",
         f"按照规范的 {module_cn} 身份规则比较 `{operation}`。"),
        (operation.startswith(("calculate", "compute", "score", "cost")),
         f"Calculates `{operation}` with bounded, deterministic {module_en} arithmetic.",
         f"使用有界且确定性的 {module_cn} 算术计算 `{operation}`。"),
        (operation.startswith(("build", "make", "prepare")),
         f"Builds `{operation}` in caller-provided storage for {module_en}.",
         f"在调用方存储中为 {module_cn} 构造 `{operation}`。"),
        (operation.startswith(("apply", "commit", "activate")),
         f"Applies `{operation}` after validating the current {module_en} state.",
         f"验证当前 {module_cn} 状态后应用 `{operation}`。"),
        (operation.startswith(("verify", "authorize", "allow")),
         f"Verifies whether `{operation}` is authorized by the {module_en} contract.",
         f"验证 `{operation}` 是否获得 {module_cn} 合同授权。"),
        (operation.startswith(("notify", "signal")),
         f"Records `{operation}` and notifies the bounded {module_en} owner path.",
         f"记录 `{operation}` 并通知有界的 {module_cn} Owner 路径。"),
        (operation.startswith(("open", "close")),
         f"Performs the bounded `{operation}` lifecycle action for {module_en}.",
         f"为 {module_cn} 执行有界的 `{operation}` 生命周期动作。"),
        (operation.startswith(("next", "advance")),
         f"Derives `{operation}` without unbounded work or allocation in {module_en}.",
         f"在 {module_cn} 中以无动态分配的有界方式推导 `{operation}`。"),
        (operation.startswith(("has_", "can_", "should_", "needs_")),
         f"Checks the `{operation}` condition in current {module_en} state.",
         f"检查当前 {module_cn} 状态中的 `{operation}` 条件。"),
        (operation.startswith("round_") or operation.endswith("_rounded_length"),
         f"Rounds `{operation}` to the next representation accepted by {module_en}.",
         f"把 `{operation}` 向上取整为 {module_cn} 可接受的下一种表示。"),
        (operation.endswith(("_size", "_count", "_length", "_offset")),
         f"Calculates the bounded `{operation}` value used by {module_en}.",
         f"计算 {module_cn} 使用的有界 `{operation}` 值。"),
        ("_from_" in operation or operation.startswith("from_"),
         f"Derives `{operation}` with the canonical {module_en} conversion rules.",
         f"按照规范的 {module_cn} 转换规则推导 `{operation}`。"),
        (operation.startswith("default_") or operation.endswith("_default"),
         f"Returns the deterministic default `{operation}` for {module_en}.",
         f"返回 {module_cn} 的确定性默认 `{operation}`。"),
        (operation.endswith(("_ready", "_pending", "_available", "_registered", "_known", "_better")),
         f"Checks the current `{operation}` condition in {module_en} state.",
         f"检查当前 {module_cn} 状态中的 `{operation}` 条件。"),
        (operation.startswith(("take", "pop_")),
         f"Removes and returns `{operation}` from a bounded {module_en} queue or slot.",
         f"从固定容量的 {module_cn} 队列或槽位中移除并返回 `{operation}`。"),
        (operation.startswith(("append", "push_")),
         f"Appends `{operation}` to bounded {module_en} storage.",
         f"把 `{operation}` 追加到固定容量的 {module_cn} 存储中。"),
        (operation.startswith(("increment", "saturating_")),
         f"Updates `{operation}` with saturating {module_en} arithmetic.",
         f"使用饱和算术更新 {module_cn} 的 `{operation}`。"),
    )
    for matched, english, chinese in patterns:
        if matched:
            return english, chinese

    words = set(operation.split("_"))
    if words.intersection(
        {
            "is", "has", "can", "should", "needs", "accepts", "matches",
            "supported", "meets", "ready", "pending", "available",
            "registered", "known", "better", "eligible", "usable",
            "congested", "valid", "compatible", "present",
        }
    ):
        return (
            f"Checks the `{operation}` condition against current {module_en} state.",
            f"根据当前 {module_cn} 状态检查 `{operation}` 条件。",
        )
    if words.intersection({"find", "lookup", "peek", "search"}):
        return (
            f"Looks up `{operation}` in bounded {module_en} state without allocation.",
            f"在固定容量的 {module_cn} 状态中查找 `{operation}`，且不进行动态分配。",
        )
    if "get" in words:
        return (
            f"Returns the current `{operation}` view from {module_en} state.",
            f"从 {module_cn} 状态返回当前 `{operation}` 视图。",
        )
    if words.intersection(
        {"max", "maximum", "size", "count", "length", "offset", "penalty",
         "crc", "crc32", "cost", "score", "rounded", "accumulate", "adjust",
         "mask", "interval", "timeout", "deadline", "epoch", "quality"}
    ):
        return (
            f"Calculates `{operation}` with bounded, deterministic {module_en} arithmetic.",
            f"使用有界且确定性的 {module_cn} 算术计算 `{operation}`。",
        )
    if words.intersection({"allocate", "reserve"}):
        return (
            f"Allocates `{operation}` from fixed {module_en} slots without heap use.",
            f"从 {module_cn} 的固定槽位分配 `{operation}`，不使用堆内存。",
        )
    if words.intersection({"complete", "finish"}):
        return (
            f"Completes `{operation}` and records its terminal {module_en} result.",
            f"完成 `{operation}` 并记录其 {module_cn} 终态结果。",
        )
    if words.intersection({"remember", "note", "record", "touch", "mark"}):
        return (
            f"Records `{operation}` in bounded {module_en} state or statistics.",
            f"在固定容量的 {module_cn} 状态或统计中记录 `{operation}`。",
        )
    if words.intersection({"begin", "start", "ensure", "prepare"}):
        return (
            f"Starts or prepares `{operation}` after validating {module_en} prerequisites.",
            f"验证 {module_cn} 前置条件后启动或准备 `{operation}`。",
        )
    if words.intersection({"clear", "reset", "remove", "revoke", "release",
                           "invalidate", "unregister", "purge", "prune"}):
        return (
            f"Clears or releases `{operation}` from bounded {module_en} state.",
            f"从固定容量的 {module_cn} 状态中清除或释放 `{operation}`。",
        )
    if words.intersection({"set", "add", "register", "bind", "install",
                           "admit", "configure", "activate"}):
        return (
            f"Validates and installs `{operation}` in bounded {module_en} state.",
            f"验证 `{operation}` 并将其安装到固定容量的 {module_cn} 状态中。",
        )
    if words.intersection({"enqueue", "queue", "push", "append", "submit"}):
        return (
            f"Copies or submits `{operation}` to a bounded {module_en} queue.",
            f"把 `{operation}` 复制或提交到固定容量的 {module_cn} 队列。",
        )
    if words.intersection({"take", "pop"}):
        return (
            f"Removes and returns `{operation}` from bounded {module_en} storage.",
            f"从固定容量的 {module_cn} 存储中移除并返回 `{operation}`。",
        )
    if words.intersection({"write", "encode", "serialize"}):
        return (
            f"Writes `{operation}` using the canonical bounded {module_en} representation.",
            f"使用规范且有界的 {module_cn} 表示写入 `{operation}`。",
        )
    if words.intersection({"read", "decode", "parse"}):
        return (
            f"Reads and validates `{operation}` from the canonical {module_en} representation.",
            f"从规范的 {module_cn} 表示中读取并验证 `{operation}`。",
        )
    if words.intersection({"send", "transmit", "emit", "broadcast", "probe",
                           "discover", "request"}):
        return (
            f"Builds and submits `{operation}` through the bounded {module_en} transmit path.",
            f"构造 `{operation}` 并将其提交到有界的 {module_cn} 发送路径。",
        )
    if words.intersection({"receive", "handle", "process", "dispatch", "deliver",
                           "forward", "handler"}):
        return (
            f"Validates and processes `{operation}` in the {module_en} receive path.",
            f"在 {module_cn} 接收路径中验证并处理 `{operation}`。",
        )
    if words.intersection({"step", "service", "pump", "drain", "poll", "maintain"}):
        return (
            f"Processes one bounded `{operation}` work unit for {module_en}.",
            f"为 {module_cn} 处理一个有界的 `{operation}` 工作单元。",
        )
    if words.intersection({"wait"}):
        return (
            f"Waits for `{operation}` using the scheduler contract of {module_en}.",
            f"按照 {module_cn} 的调度合同等待 `{operation}`。",
        )
    if words.intersection({"protect", "verify", "authorize"}):
        return (
            f"Validates `{operation}` against the {module_en} security or authorization contract.",
            f"按照 {module_cn} 的安全或授权合同验证 `{operation}`。",
        )
    if words.intersection({"classify"}):
        return (
            f"Classifies `{operation}` using deterministic {module_en} rules.",
            f"按照确定性的 {module_cn} 规则对 `{operation}` 分类。",
        )
    return (
        f"Implements the bounded `{operation}` helper for {module_en}.",
        f"为 {module_cn} 实现有界的 `{operation}` 辅助操作。",
    )


def read_lines(path: pathlib.Path) -> tuple[list[str], str, bool]:
    raw = path.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    text = raw.decode("utf-8")
    return text.splitlines(), newline, text.endswith(("\n", "\r"))


def has_bilingual_comment(lines: list[str], definition: FunctionDefinition) -> bool:
    start = max(0, definition.line - 9)
    window = "\n".join(lines[start : definition.line - 1])
    return "EN:" in window and "中文：" in window


def generated_comment_range(
    lines: list[str], definition: FunctionDefinition
) -> tuple[int, int] | None:
    end = definition.line - 1
    start = end - 4
    if start < 0:
        return None
    block = lines[start:end]
    if (
        len(block) == 4
        and block[0] == "/*"
        and block[1].startswith(" * EN:")
        and block[2].startswith(" * 中文：")
        and block[3] == " */"
    ):
        return start, end
    return None


def apply_comments(
    definitions: list[FunctionDefinition], refresh: bool
) -> int:
    by_source: dict[str, list[FunctionDefinition]] = {}
    for definition in definitions:
        by_source.setdefault(definition.source, []).append(definition)

    inserted = 0
    for source, source_definitions in sorted(by_source.items()):
        path = ROOT / source
        lines, newline, had_final_newline = read_lines(path)
        pending = [
            definition
            for definition in source_definitions
            if refresh or not has_bilingual_comment(lines, definition)
        ]
        for definition in sorted(pending, key=lambda item: item.line, reverse=True):
            english, chinese = generated_summary(definition)
            comment = ["/*", f" * EN: {english}", f" * 中文：{chinese}", " */"]
            existing = generated_comment_range(lines, definition) if refresh else None
            if existing is None:
                lines[definition.line - 1 : definition.line - 1] = comment
            else:
                lines[existing[0] : existing[1]] = comment
            inserted += 1
        if pending:
            output = newline.join(lines)
            if had_final_newline:
                output += newline
            path.write_bytes(output.encode("utf-8"))
    return inserted


def missing_comments(definitions: list[FunctionDefinition]) -> list[FunctionDefinition]:
    lines_by_source: dict[str, list[str]] = {}
    missing: list[FunctionDefinition] = []
    for definition in definitions:
        if definition.source not in lines_by_source:
            lines_by_source[definition.source] = read_lines(ROOT / definition.source)[0]
        if not has_bilingual_comment(lines_by_source[definition.source], definition):
            missing.append(definition)
    return missing


def main() -> int:
    args = parse_arguments()
    definitions = collect_c_definitions(args.gcc) + collect_inline_definitions()
    definitions = sorted(set(definitions))
    if args.list:
        for definition in definitions:
            print(
                f"{definition.source}:{definition.line}:"
                f"{definition.visibility}:{definition.name}"
            )
    if args.apply or args.refresh:
        inserted = apply_comments(definitions, refresh=args.refresh)
        print(f"updated={inserted}")
        definitions = collect_c_definitions(args.gcc) + collect_inline_definitions()
        definitions = sorted(set(definitions))
    missing = missing_comments(definitions)
    print(f"definitions={len(definitions)} missing={len(missing)}")
    for definition in missing:
        print(f"MISSING {definition.source}:{definition.line}:{definition.name}")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
