#ifndef UCN_SIMPLIFIED_INSTALL_ABI_CONTRACT_H
#define UCN_SIMPLIFIED_INSTALL_ABI_CONTRACT_H

#include <ucn/ucn_simplified.h>

#include <stddef.h>
#include <stdint.h>

/* EN: These assertions are compiled by both the installed C99 and C++17
 * consumers.  They freeze every public DTO's size, alignment, and field
 * offsets for the two supported pointer-width ABIs.
 * 中文：这些断言会由安装后的 C99 与 C++17 consumer 同时编译，用于冻结两种
 * 受支持指针宽度 ABI 下每个公共 DTO 的大小、对齐和全部字段偏移。 */
#define UCN_ABI_ALIGN_PROBE(type_) ucn_abi_align_probe_##type_
#define UCN_ABI_DECLARE(type_)                                               \
    struct UCN_ABI_ALIGN_PROBE(type_) { unsigned char lead; type_ value; }
#define UCN_ABI_ALIGNOF(type_)                                               \
    offsetof(struct UCN_ABI_ALIGN_PROBE(type_), value)
#define UCN_ABI_SIZE_ALIGN(type_, size_, align_)                             \
    UCN_STATIC_ASSERT(sizeof(type_) == (size_), type_##_size);               \
    UCN_STATIC_ASSERT(UCN_ABI_ALIGNOF(type_) == (align_), type_##_alignment)
#define UCN_ABI_OFFSET(type_, field_, offset_)                               \
    UCN_STATIC_ASSERT(offsetof(type_, field_) == (offset_),                  \
                      type_##_##field_##_offset)

UCN_ABI_DECLARE(ucn_handle_t);
UCN_ABI_DECLARE(ucn_static_binding_t);
UCN_ABI_DECLARE(ucn_config_t);
UCN_ABI_DECLARE(ucn_rx_meta_t);
UCN_ABI_DECLARE(ucn_tx_meta_t);
UCN_ABI_DECLARE(ucn_link_event_meta_t);
UCN_ABI_DECLARE(ucn_tx_port_vtable_t);
UCN_ABI_DECLARE(ucn_lock_ops_t);
UCN_ABI_DECLARE(ucn_link_port_t);
UCN_ABI_DECLARE(ucn_ports_t);
UCN_ABI_DECLARE(ucn_target_t);
UCN_ABI_DECLARE(ucn_callback_scope_t);
UCN_ABI_DECLARE(ucn_endpoint_message_t);
UCN_ABI_DECLARE(ucn_endpoint_config_t);
UCN_ABI_DECLARE(ucn_static_path_t);
UCN_ABI_DECLARE(ucn_send_options_t);
UCN_ABI_DECLARE(ucn_send_view_t);
UCN_ABI_DECLARE(ucn_step_budget_t);
UCN_ABI_DECLARE(ucn_step_result_t);
UCN_ABI_DECLARE(ucn_stats_t);
UCN_ABI_DECLARE(ucn_storage_t);
#if UCN_FEATURE_PERSISTENCE_ENABLED
UCN_ABI_DECLARE(ucn_persist_domain_key_t);
UCN_ABI_DECLARE(ucn_persist_manifest_entry_t);
UCN_ABI_DECLARE(ucn_persist_manifest_t);
UCN_ABI_DECLARE(ucn_persist_io_completion_t);
UCN_ABI_DECLARE(ucn_persist_witness_view_t);
UCN_ABI_DECLARE(ucn_persistence_provider_vtable_t);
UCN_ABI_DECLARE(ucn_persistence_provider_t);
UCN_ABI_DECLARE(ucn_persist_domain_binding_t);
UCN_ABI_DECLARE(ucn_persistence_config_t);
UCN_ABI_DECLARE(ucn_persistence_storage_t);
UCN_ABI_DECLARE(ucn_persist_gate_storage_t);
UCN_ABI_DECLARE(ucn_persistence_digest_workspace_t);
#endif

UCN_STATIC_ASSERT(sizeof(ucn_result_t) == 4U, result_size);
UCN_STATIC_ASSERT(sizeof(void *) == 4U || sizeof(void *) == 8U,
                  supported_pointer_width);

UCN_ABI_SIZE_ALIGN(ucn_handle_t, 12U, 4U);
UCN_ABI_OFFSET(ucn_handle_t, runtime_instance, 0U);
UCN_ABI_OFFSET(ucn_handle_t, owner_instance, 4U);
UCN_ABI_OFFSET(ucn_handle_t, slot, 6U);
UCN_ABI_OFFSET(ucn_handle_t, generation, 8U);
UCN_ABI_OFFSET(ucn_handle_t, object_kind, 10U);
UCN_ABI_OFFSET(ucn_handle_t, reserved_zero, 11U);

UCN_ABI_SIZE_ALIGN(ucn_static_binding_t, 24U, 8U);
UCN_ABI_OFFSET(ucn_static_binding_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_static_binding_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_static_binding_t, address, 4U);
UCN_ABI_OFFSET(ucn_static_binding_t, binding_generation, 8U);
UCN_ABI_OFFSET(ucn_static_binding_t, principal_digest, 16U);

#if UINTPTR_MAX == UINT64_MAX
#define UCN_ABI_CONFIG_SIZE 56U
#define UCN_ABI_CONFIG_BINDINGS 40U
#define UCN_ABI_CONFIG_BINDING_COUNT 48U
#define UCN_ABI_CONFIG_ADDRESS_WIDTH 50U
#define UCN_ABI_CONFIG_TRUSTED 51U
#define UCN_ABI_CONFIG_RESERVED 52U
#define UCN_ABI_TX_VTABLE_SIZE 24U
#define UCN_ABI_TX_VTABLE_ALIGN 8U
#define UCN_ABI_TX_VTABLE_SUBMIT 8U
#define UCN_ABI_TX_VTABLE_CANCEL 16U
#define UCN_ABI_LOCK_SIZE 32U
#define UCN_ABI_LOCK_ALIGN 8U
#define UCN_ABI_LOCK_CONTEXT 8U
#define UCN_ABI_LOCK_ENTER 16U
#define UCN_ABI_LOCK_LEAVE 24U
#define UCN_ABI_LINK_PORT_SIZE 48U
#define UCN_ABI_LINK_PORT_ALIGN 8U
#define UCN_ABI_LINK_PORT_CONTEXT 16U
#define UCN_ABI_LINK_PORT_TX 24U
#define UCN_ABI_PORTS_SIZE 88U
#define UCN_ABI_PORTS_ALIGN 8U
#define UCN_ABI_PORTS_LINKS 8U
#define UCN_ABI_PORTS_LINK_COUNT 16U
#define UCN_ABI_PORTS_RESERVED 18U
#define UCN_ABI_PORTS_STATE_LOCK 24U
#define UCN_ABI_PORTS_DRIVER_GATE 56U
#define UCN_ABI_ENDPOINT_MESSAGE_SIZE 56U
#define UCN_ABI_ENDPOINT_MESSAGE_ALIGN 8U
#define UCN_ABI_ENDPOINT_PAYLOAD 16U
#define UCN_ABI_ENDPOINT_PAYLOAD_BYTES 24U
#define UCN_ABI_ENDPOINT_TIMESTAMP 32U
#define UCN_ABI_ENDPOINT_CALLBACK_SCOPE 40U
#define UCN_ABI_ENDPOINT_CONFIG_SIZE 24U
#define UCN_ABI_ENDPOINT_CONFIG_ALIGN 8U
#define UCN_ABI_ENDPOINT_CONFIG_RECEIVE 8U
#define UCN_ABI_ENDPOINT_CONFIG_CONTEXT 16U
#define UCN_ABI_SEND_OPTIONS_SIZE 56U
#define UCN_ABI_SEND_OPTIONS_CONTEXT 32U
#define UCN_ABI_SEND_OPTIONS_COMPLETION 40U
#define UCN_ABI_SEND_OPTIONS_TRAFFIC 48U
#define UCN_ABI_PERSIST_MANIFEST_SIZE 56U
#define UCN_ABI_PERSIST_MANIFEST_ENTRIES 24U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE_SIZE 64U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE_ALIGN 8U
#define UCN_ABI_PERSIST_PROVIDER_SIZE 40U
#define UCN_ABI_PERSIST_PROVIDER_CONTEXT 8U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE 16U
#define UCN_ABI_PERSIST_CONFIG_SIZE 96U
#define UCN_ABI_PERSIST_CONFIG_MANIFEST 16U
#define UCN_ABI_PERSIST_CONFIG_BINDINGS 24U
#define UCN_ABI_PERSIST_CONFIG_BINDING_COUNT 32U
#define UCN_ABI_PERSIST_CONFIG_PROVIDER 40U
#define UCN_ABI_PERSIST_CONFIG_LOCK 48U
#define UCN_ABI_PERSIST_CONFIG_GATE 80U
#define UCN_ABI_PERSIST_CONFIG_DIGEST_WORKSPACE 88U
#define UCN_ABI_PERSIST_REQUEST_BODY 56U
#else
#define UCN_ABI_CONFIG_SIZE 56U
#define UCN_ABI_CONFIG_BINDINGS 40U
#define UCN_ABI_CONFIG_BINDING_COUNT 44U
#define UCN_ABI_CONFIG_ADDRESS_WIDTH 46U
#define UCN_ABI_CONFIG_TRUSTED 47U
#define UCN_ABI_CONFIG_RESERVED 48U
#define UCN_ABI_TX_VTABLE_SIZE 12U
#define UCN_ABI_TX_VTABLE_ALIGN 4U
#define UCN_ABI_TX_VTABLE_SUBMIT 4U
#define UCN_ABI_TX_VTABLE_CANCEL 8U
#define UCN_ABI_LOCK_SIZE 16U
#define UCN_ABI_LOCK_ALIGN 4U
#define UCN_ABI_LOCK_CONTEXT 4U
#define UCN_ABI_LOCK_ENTER 8U
#define UCN_ABI_LOCK_LEAVE 12U
#define UCN_ABI_LINK_PORT_SIZE 28U
#define UCN_ABI_LINK_PORT_ALIGN 4U
#define UCN_ABI_LINK_PORT_CONTEXT 12U
#define UCN_ABI_LINK_PORT_TX 16U
#define UCN_ABI_PORTS_SIZE 44U
#define UCN_ABI_PORTS_ALIGN 4U
#define UCN_ABI_PORTS_LINKS 4U
#define UCN_ABI_PORTS_LINK_COUNT 8U
#define UCN_ABI_PORTS_RESERVED 10U
#define UCN_ABI_PORTS_STATE_LOCK 12U
#define UCN_ABI_PORTS_DRIVER_GATE 28U
#define UCN_ABI_ENDPOINT_MESSAGE_SIZE 48U
#define UCN_ABI_ENDPOINT_MESSAGE_ALIGN 8U
#define UCN_ABI_ENDPOINT_PAYLOAD 16U
#define UCN_ABI_ENDPOINT_PAYLOAD_BYTES 20U
#define UCN_ABI_ENDPOINT_TIMESTAMP 24U
#define UCN_ABI_ENDPOINT_CALLBACK_SCOPE 32U
#define UCN_ABI_ENDPOINT_CONFIG_SIZE 16U
#define UCN_ABI_ENDPOINT_CONFIG_ALIGN 4U
#define UCN_ABI_ENDPOINT_CONFIG_RECEIVE 8U
#define UCN_ABI_ENDPOINT_CONFIG_CONTEXT 12U
#define UCN_ABI_SEND_OPTIONS_SIZE 48U
#define UCN_ABI_SEND_OPTIONS_CONTEXT 28U
#define UCN_ABI_SEND_OPTIONS_COMPLETION 32U
#define UCN_ABI_SEND_OPTIONS_TRAFFIC 36U
#define UCN_ABI_PERSIST_MANIFEST_SIZE 48U
#define UCN_ABI_PERSIST_MANIFEST_ENTRIES 24U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE_SIZE 32U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE_ALIGN 4U
#define UCN_ABI_PERSIST_PROVIDER_SIZE 28U
#define UCN_ABI_PERSIST_PROVIDER_CONTEXT 4U
#define UCN_ABI_PERSIST_PROVIDER_VTABLE 8U
#define UCN_ABI_PERSIST_CONFIG_SIZE 56U
#define UCN_ABI_PERSIST_CONFIG_MANIFEST 16U
#define UCN_ABI_PERSIST_CONFIG_BINDINGS 20U
#define UCN_ABI_PERSIST_CONFIG_BINDING_COUNT 24U
#define UCN_ABI_PERSIST_CONFIG_PROVIDER 28U
#define UCN_ABI_PERSIST_CONFIG_LOCK 32U
#define UCN_ABI_PERSIST_CONFIG_GATE 48U
#define UCN_ABI_PERSIST_CONFIG_DIGEST_WORKSPACE 52U
#define UCN_ABI_PERSIST_REQUEST_BODY 56U
#endif

UCN_ABI_SIZE_ALIGN(ucn_config_t, UCN_ABI_CONFIG_SIZE, 8U);
UCN_ABI_OFFSET(ucn_config_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_config_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_config_t, storage_layout, 4U);
UCN_ABI_OFFSET(ucn_config_t, manifest_reserved_zero, 6U);
UCN_ABI_OFFSET(ucn_config_t, compiled_manifest_hash, 8U);
UCN_ABI_OFFSET(ucn_config_t, runtime_instance, 16U);
UCN_ABI_OFFSET(ucn_config_t, realm_id, 20U);
UCN_ABI_OFFSET(ucn_config_t, local_address, 24U);
UCN_ABI_OFFSET(ucn_config_t, local_binding_generation, 28U);
UCN_ABI_OFFSET(ucn_config_t, local_principal_digest, 32U);
UCN_ABI_OFFSET(ucn_config_t, bindings, UCN_ABI_CONFIG_BINDINGS);
UCN_ABI_OFFSET(ucn_config_t, binding_count, UCN_ABI_CONFIG_BINDING_COUNT);
UCN_ABI_OFFSET(ucn_config_t, address_width, UCN_ABI_CONFIG_ADDRESS_WIDTH);
UCN_ABI_OFFSET(ucn_config_t, trusted_o0_network, UCN_ABI_CONFIG_TRUSTED);
UCN_ABI_OFFSET(ucn_config_t, reserved_zero, UCN_ABI_CONFIG_RESERVED);

UCN_ABI_SIZE_ALIGN(ucn_rx_meta_t, 24U, 8U);
UCN_ABI_OFFSET(ucn_rx_meta_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_rx_meta_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_rx_meta_t, timestamp_us, 8U);
UCN_ABI_OFFSET(ucn_rx_meta_t, sender_discriminator, 16U);
UCN_ABI_OFFSET(ucn_rx_meta_t, reserved_zero, 20U);

UCN_ABI_SIZE_ALIGN(ucn_tx_meta_t, 24U, 8U);
UCN_ABI_OFFSET(ucn_tx_meta_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_tx_meta_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_tx_meta_t, timestamp_us, 8U);
UCN_ABI_OFFSET(ucn_tx_meta_t, reserved_zero, 16U);

UCN_ABI_SIZE_ALIGN(ucn_link_event_meta_t, 12U, 4U);
UCN_ABI_OFFSET(ucn_link_event_meta_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_link_event_meta_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_link_event_meta_t, new_link_instance, 4U);
UCN_ABI_OFFSET(ucn_link_event_meta_t, reserved_zero, 8U);

UCN_ABI_SIZE_ALIGN(ucn_tx_port_vtable_t, UCN_ABI_TX_VTABLE_SIZE,
                   UCN_ABI_TX_VTABLE_ALIGN);
UCN_ABI_OFFSET(ucn_tx_port_vtable_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_tx_port_vtable_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_tx_port_vtable_t, submit, UCN_ABI_TX_VTABLE_SUBMIT);
UCN_ABI_OFFSET(ucn_tx_port_vtable_t, cancel, UCN_ABI_TX_VTABLE_CANCEL);

UCN_ABI_SIZE_ALIGN(ucn_lock_ops_t, UCN_ABI_LOCK_SIZE, UCN_ABI_LOCK_ALIGN);
UCN_ABI_OFFSET(ucn_lock_ops_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_lock_ops_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_lock_ops_t, context, UCN_ABI_LOCK_CONTEXT);
UCN_ABI_OFFSET(ucn_lock_ops_t, enter, UCN_ABI_LOCK_ENTER);
UCN_ABI_OFFSET(ucn_lock_ops_t, leave, UCN_ABI_LOCK_LEAVE);

UCN_ABI_SIZE_ALIGN(ucn_link_port_t, UCN_ABI_LINK_PORT_SIZE,
                   UCN_ABI_LINK_PORT_ALIGN);
UCN_ABI_OFFSET(ucn_link_port_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_link_port_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_link_port_t, link_instance, 4U);
UCN_ABI_OFFSET(ucn_link_port_t, frame_mtu, 8U);
UCN_ABI_OFFSET(ucn_link_port_t, reserved_zero, 10U);
UCN_ABI_OFFSET(ucn_link_port_t, context, UCN_ABI_LINK_PORT_CONTEXT);
UCN_ABI_OFFSET(ucn_link_port_t, tx, UCN_ABI_LINK_PORT_TX);

UCN_ABI_SIZE_ALIGN(ucn_ports_t, UCN_ABI_PORTS_SIZE, UCN_ABI_PORTS_ALIGN);
UCN_ABI_OFFSET(ucn_ports_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_ports_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_ports_t, links, UCN_ABI_PORTS_LINKS);
UCN_ABI_OFFSET(ucn_ports_t, link_count, UCN_ABI_PORTS_LINK_COUNT);
UCN_ABI_OFFSET(ucn_ports_t, reserved_zero, UCN_ABI_PORTS_RESERVED);
UCN_ABI_OFFSET(ucn_ports_t, state_lock, UCN_ABI_PORTS_STATE_LOCK);
UCN_ABI_OFFSET(ucn_ports_t, driver_callback_gate, UCN_ABI_PORTS_DRIVER_GATE);

UCN_ABI_SIZE_ALIGN(ucn_target_t, 16U, 4U);
UCN_ABI_OFFSET(ucn_target_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_target_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_target_t, address, 4U);
UCN_ABI_OFFSET(ucn_target_t, binding_generation, 8U);
UCN_ABI_OFFSET(ucn_target_t, service_id, 12U);
UCN_ABI_OFFSET(ucn_target_t, reserved_zero, 14U);

UCN_ABI_SIZE_ALIGN(ucn_callback_scope_t, 16U, 8U);
UCN_ABI_OFFSET(ucn_callback_scope_t, runtime_instance, 0U);
UCN_ABI_OFFSET(ucn_callback_scope_t, owner_instance, 4U);
UCN_ABI_OFFSET(ucn_callback_scope_t, reserved_zero, 6U);
UCN_ABI_OFFSET(ucn_callback_scope_t, nonce, 8U);

UCN_ABI_SIZE_ALIGN(ucn_endpoint_message_t, UCN_ABI_ENDPOINT_MESSAGE_SIZE,
                   UCN_ABI_ENDPOINT_MESSAGE_ALIGN);
UCN_ABI_OFFSET(ucn_endpoint_message_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, source_address, 4U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, source_binding_generation, 8U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, service_id, 12U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, traffic_class, 14U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, hop_limit, 15U);
UCN_ABI_OFFSET(ucn_endpoint_message_t, payload, UCN_ABI_ENDPOINT_PAYLOAD);
UCN_ABI_OFFSET(ucn_endpoint_message_t, payload_bytes,
               UCN_ABI_ENDPOINT_PAYLOAD_BYTES);
UCN_ABI_OFFSET(ucn_endpoint_message_t, receive_timestamp_us,
               UCN_ABI_ENDPOINT_TIMESTAMP);
UCN_ABI_OFFSET(ucn_endpoint_message_t, callback_scope,
               UCN_ABI_ENDPOINT_CALLBACK_SCOPE);

UCN_ABI_SIZE_ALIGN(ucn_endpoint_config_t, UCN_ABI_ENDPOINT_CONFIG_SIZE,
                   UCN_ABI_ENDPOINT_CONFIG_ALIGN);
UCN_ABI_OFFSET(ucn_endpoint_config_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_endpoint_config_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_endpoint_config_t, service_id, 4U);
UCN_ABI_OFFSET(ucn_endpoint_config_t, reserved_zero, 6U);
UCN_ABI_OFFSET(ucn_endpoint_config_t, receive,
               UCN_ABI_ENDPOINT_CONFIG_RECEIVE);
UCN_ABI_OFFSET(ucn_endpoint_config_t, context,
               UCN_ABI_ENDPOINT_CONFIG_CONTEXT);

UCN_ABI_SIZE_ALIGN(ucn_static_path_t, 16U, 4U);
UCN_ABI_OFFSET(ucn_static_path_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_static_path_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_static_path_t, destination_address, 4U);
UCN_ABI_OFFSET(ucn_static_path_t, destination_binding_generation, 8U);
UCN_ABI_OFFSET(ucn_static_path_t, link_index, 12U);
UCN_ABI_OFFSET(ucn_static_path_t, path_frame_mtu, 14U);

UCN_ABI_SIZE_ALIGN(ucn_send_options_t, UCN_ABI_SEND_OPTIONS_SIZE, 8U);
UCN_ABI_OFFSET(ucn_send_options_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_send_options_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_send_options_t, absolute_deadline_us, 8U);
UCN_ABI_OFFSET(ucn_send_options_t, pinned_path, 16U);
UCN_ABI_OFFSET(ucn_send_options_t, completion_context,
               UCN_ABI_SEND_OPTIONS_CONTEXT);
UCN_ABI_OFFSET(ucn_send_options_t, completion,
               UCN_ABI_SEND_OPTIONS_COMPLETION);
UCN_ABI_OFFSET(ucn_send_options_t, traffic_class,
               UCN_ABI_SEND_OPTIONS_TRAFFIC);
UCN_ABI_OFFSET(ucn_send_options_t, delivery_guarantee,
               UCN_ABI_SEND_OPTIONS_TRAFFIC + 1U);
UCN_ABI_OFFSET(ucn_send_options_t, interaction_role,
               UCN_ABI_SEND_OPTIONS_TRAFFIC + 2U);
UCN_ABI_OFFSET(ucn_send_options_t, hop_limit,
               UCN_ABI_SEND_OPTIONS_TRAFFIC + 3U);
UCN_ABI_OFFSET(ucn_send_options_t, copy_payload,
               UCN_ABI_SEND_OPTIONS_TRAFFIC + 4U);
UCN_ABI_OFFSET(ucn_send_options_t, reserved_zero,
               UCN_ABI_SEND_OPTIONS_TRAFFIC + 5U);

UCN_ABI_SIZE_ALIGN(ucn_send_view_t, 16U, 4U);
UCN_ABI_OFFSET(ucn_send_view_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_send_view_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_send_view_t, terminal_result, 4U);
UCN_ABI_OFFSET(ucn_send_view_t, origin_sequence, 8U);
UCN_ABI_OFFSET(ucn_send_view_t, admission, 12U);
UCN_ABI_OFFSET(ucn_send_view_t, link_outcome, 13U);
UCN_ABI_OFFSET(ucn_send_view_t, buffer_released, 14U);
UCN_ABI_OFFSET(ucn_send_view_t, callback_delivered, 15U);

UCN_ABI_SIZE_ALIGN(ucn_step_budget_t, 8U, 2U);
UCN_ABI_OFFSET(ucn_step_budget_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_step_budget_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_step_budget_t, max_work, 4U);
UCN_ABI_OFFSET(ucn_step_budget_t, reserved_zero, 6U);

UCN_ABI_SIZE_ALIGN(ucn_step_result_t, 24U, 8U);
UCN_ABI_OFFSET(ucn_step_result_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_step_result_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_step_result_t, next_deadline_us, 8U);
UCN_ABI_OFFSET(ucn_step_result_t, work_done, 16U);
UCN_ABI_OFFSET(ucn_step_result_t, more_work, 18U);
UCN_ABI_OFFSET(ucn_step_result_t, lifecycle, 19U);

UCN_ABI_SIZE_ALIGN(ucn_stats_t, 36U, 4U);
UCN_ABI_OFFSET(ucn_stats_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_stats_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_stats_t, tx_admitted, 4U);
UCN_ABI_OFFSET(ucn_stats_t, tx_completed, 8U);
UCN_ABI_OFFSET(ucn_stats_t, tx_failed, 12U);
UCN_ABI_OFFSET(ucn_stats_t, rx_published, 16U);
UCN_ABI_OFFSET(ucn_stats_t, rx_delivered, 20U);
UCN_ABI_OFFSET(ucn_stats_t, rx_dropped, 24U);
UCN_ABI_OFFSET(ucn_stats_t, malformed, 28U);
UCN_ABI_OFFSET(ucn_stats_t, no_space, 32U);

UCN_ABI_SIZE_ALIGN(ucn_storage_t, UCN_STORAGE_BYTES,
                   UCN_STORAGE_ALIGNMENT);

#if UCN_FEATURE_PERSISTENCE_ENABLED
UCN_ABI_SIZE_ALIGN(ucn_persist_domain_key_t, 16U, 8U);
UCN_ABI_OFFSET(ucn_persist_domain_key_t, domain_id, 0U);
UCN_ABI_OFFSET(ucn_persist_domain_key_t, domain_kind, 8U);
UCN_ABI_OFFSET(ucn_persist_domain_key_t, reserved_zero, 10U);
UCN_ABI_OFFSET(ucn_persist_domain_key_t, reserved_zero2, 12U);

UCN_ABI_SIZE_ALIGN(ucn_persist_manifest_entry_t, 40U, 8U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, domain, 8U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, body_capacity_bytes, 24U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, slot_capacity_bytes, 28U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, schema_id, 32U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, schema_version, 34U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, digest_suite, 36U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, witness_policy, 38U);
UCN_ABI_OFFSET(ucn_persist_manifest_entry_t, provider_atomicity_class, 39U);

UCN_ABI_SIZE_ALIGN(ucn_persist_manifest_t, UCN_ABI_PERSIST_MANIFEST_SIZE, 8U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, protocol_manifest_version, 4U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, storage_layout_version, 8U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, composition_feature_bits, 16U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, entries,
               UCN_ABI_PERSIST_MANIFEST_ENTRIES);
UCN_ABI_OFFSET(ucn_persist_manifest_t, entry_count,
               UCN_ABI_PERSIST_MANIFEST_ENTRIES + sizeof(void *));
UCN_ABI_OFFSET(ucn_persist_manifest_t, profile_id,
               UCN_ABI_PERSIST_MANIFEST_ENTRIES + sizeof(void *) + 2U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, reserved_zero,
               UCN_ABI_PERSIST_MANIFEST_ENTRIES + sizeof(void *) + 3U);
UCN_ABI_OFFSET(ucn_persist_manifest_t, expected_digest,
               UCN_ABI_PERSIST_MANIFEST_ENTRIES + sizeof(void *) + 4U);

UCN_ABI_SIZE_ALIGN(ucn_persist_io_completion_t, 32U, 8U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, io_token, 8U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, result, 16U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, exact_bytes, 20U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, phase, 24U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, blob_state, 25U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, slot_index, 26U);
UCN_ABI_OFFSET(ucn_persist_io_completion_t, reserved_zero, 27U);

UCN_ABI_SIZE_ALIGN(ucn_persist_witness_view_t, 40U, 8U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t, domain, 8U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t,
               highest_maybe_published_generation, 24U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t, state, 32U);
UCN_ABI_OFFSET(ucn_persist_witness_view_t, reserved_zero, 33U);

UCN_ABI_SIZE_ALIGN(ucn_persistence_provider_vtable_t,
                   UCN_ABI_PERSIST_PROVIDER_VTABLE_SIZE,
                   UCN_ABI_PERSIST_PROVIDER_VTABLE_ALIGN);
UCN_ABI_OFFSET(ucn_persistence_provider_vtable_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persistence_provider_vtable_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persistence_provider_vtable_t, begin_load_slot,
               sizeof(void *));
UCN_ABI_OFFSET(ucn_persistence_provider_vtable_t, poll,
               7U * sizeof(void *));

UCN_ABI_SIZE_ALIGN(ucn_persistence_provider_t,
                   UCN_ABI_PERSIST_PROVIDER_SIZE,
                   UCN_ABI_PERSIST_PROVIDER_VTABLE_ALIGN);
UCN_ABI_OFFSET(ucn_persistence_provider_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persistence_provider_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persistence_provider_t, context,
               UCN_ABI_PERSIST_PROVIDER_CONTEXT);
UCN_ABI_OFFSET(ucn_persistence_provider_t, vtable,
               UCN_ABI_PERSIST_PROVIDER_VTABLE);
UCN_ABI_OFFSET(ucn_persistence_provider_t, minimum_write_alignment,
               UCN_ABI_PERSIST_PROVIDER_VTABLE + sizeof(void *));

UCN_ABI_SIZE_ALIGN(ucn_persist_domain_binding_t, 32U, 8U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, domain, 8U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, business_owner_instance, 24U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, domain_generation, 26U);
UCN_ABI_OFFSET(ucn_persist_domain_binding_t, reserved_zero, 28U);

UCN_ABI_SIZE_ALIGN(ucn_persistence_config_t, UCN_ABI_PERSIST_CONFIG_SIZE,
                   UCN_ABI_PERSIST_PROVIDER_VTABLE_ALIGN);
UCN_ABI_OFFSET(ucn_persistence_config_t, struct_size, 0U);
UCN_ABI_OFFSET(ucn_persistence_config_t, api_version, 2U);
UCN_ABI_OFFSET(ucn_persistence_config_t, runtime_instance, 4U);
UCN_ABI_OFFSET(ucn_persistence_config_t, owner_instance, 8U);
UCN_ABI_OFFSET(ucn_persistence_config_t, reserved_zero, 10U);
UCN_ABI_OFFSET(ucn_persistence_config_t, required_domain_mask, 12U);
UCN_ABI_OFFSET(ucn_persistence_config_t, manifest,
               UCN_ABI_PERSIST_CONFIG_MANIFEST);
UCN_ABI_OFFSET(ucn_persistence_config_t, domain_bindings,
               UCN_ABI_PERSIST_CONFIG_BINDINGS);
UCN_ABI_OFFSET(ucn_persistence_config_t, domain_binding_count,
               UCN_ABI_PERSIST_CONFIG_BINDING_COUNT);
UCN_ABI_OFFSET(ucn_persistence_config_t, reserved_zero2,
               UCN_ABI_PERSIST_CONFIG_BINDING_COUNT + 2U);
UCN_ABI_OFFSET(ucn_persistence_config_t, provider,
               UCN_ABI_PERSIST_CONFIG_PROVIDER);
UCN_ABI_OFFSET(ucn_persistence_config_t, state_lock,
               UCN_ABI_PERSIST_CONFIG_LOCK);
UCN_ABI_OFFSET(ucn_persistence_config_t, shared_callback_gate,
               UCN_ABI_PERSIST_CONFIG_GATE);
UCN_ABI_OFFSET(ucn_persistence_config_t, digest_workspace,
               UCN_ABI_PERSIST_CONFIG_DIGEST_WORKSPACE);

UCN_ABI_SIZE_ALIGN(ucn_persistence_storage_t, UCN_PERSIST_STORAGE_BYTES,
                   UCN_PERSIST_STORAGE_ALIGNMENT);
UCN_ABI_SIZE_ALIGN(ucn_persist_gate_storage_t,
                   UCN_PERSIST_GATE_STORAGE_BYTES,
                   UCN_PERSIST_GATE_STORAGE_ALIGNMENT);
UCN_ABI_SIZE_ALIGN(ucn_persistence_digest_workspace_t,
                   UCN_PERSIST_DIGEST_WORKSPACE_BYTES,
                   UCN_PERSIST_STORAGE_ALIGNMENT);
#endif

#endif
