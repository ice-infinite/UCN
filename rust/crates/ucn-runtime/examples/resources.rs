//! Host sizeof, not MCU RAM/stack proof. Host sizeof，不是MCU RAM/栈证明。
use core::mem::{align_of, size_of};
fn report<T>(name: &str) {
    println!(
        "{name} bytes={} alignment={}",
        size_of::<T>(),
        align_of::<T>()
    );
}
fn report_routes() {
    #[cfg(feature = "route-data-runtime")]
    {
        report::<ucn_runtime::RoutedNode<'static, 2, 4, 2, 1, 1, 128, 1, 2, 4, 1>>(
            "small_routed_data_node",
        );
        report::<ucn_runtime::RoutedNode<'static, 3, 12, 6, 2, 2, 256, 2, 4, 8, 2>>(
            "mesh_routed_data_test_node",
        );
    }
    #[cfg(feature = "route-runtime")]
    {
        report::<ucn_runtime::RouteRuntime<'static, 2, 4, 128>>("small_multi_peer_route_root");
        report::<ucn_runtime::RouteRuntime<'static, 3, 12, 256>>("mesh_route_test_root");
    }
}
fn main() {
    report_routes();
    #[cfg(feature = "bootstrap-runtime")]
    {
        report::<ucn_runtime::BootstrapClient<'static, 128>>("bootstrap_client");
        report::<ucn_runtime::BootstrapListener<'static, 128>>("bootstrap_listener");
        report::<ucn_admission::Cookie>("small_cookie");
        report::<ucn_runtime::BootstrapAuthExchange<'static, 'static, 128>>(
            "bootstrap_auth_exchange",
        );
    }
    #[cfg(feature = "capability-control")]
    report::<ucn_runtime::CapabilityRuntime<'static, 128>>("capability_runtime");
    #[cfg(feature = "peer-reauth")]
    report::<ucn_runtime::PeerReauthRuntime<'static, 'static, 128>>("peer_reauth_runtime");
    #[cfg(feature = "bootstrap-join")]
    {
        report::<ucn_runtime::JoinFreshness<'static>>("join_freshness");
        report::<ucn_runtime::BootstrapJoin<'static, 'static, 128>>("bootstrap_durable_join");
    }
    #[cfg(feature = "time-runtime")]
    report::<ucn_runtime::TimeRuntime<'static, 128>>("direct_time_runtime");
    #[cfg(feature = "parent-control")]
    report::<ucn_runtime::ParentCoordinator>("parent_coordinator");
    #[cfg(feature = "transfer-sender")]
    {
        report::<ucn_runtime::TransferSender<64, 4, 128>>("small_transfer_sender");
        report::<ucn_runtime::TransferSender<512, 16, 128>>("bounded_transfer_sender");
    }
    report::<ucn_transport::NanoTransferOwner>("nano_transfer");
    report::<ucn_transport::LiteTransferOwner>("lite_transfer");
    report::<ucn_transport::FullTransferOwner>("full_transfer");
    report::<ucn_service::NanoServiceOwner>("nano_service");
    report::<ucn_service::LiteServiceOwner>("lite_service");
    report::<ucn_service::FullServiceOwner>("full_service");
    report::<ucn_runtime::RuntimeBridge<2, 128>>("runtime_bridge");
    report::<ucn_runtime::ControlCoordinator<2, 128>>("control_coordinator");
    #[cfg(feature = "durable-rpc")]
    {
        report::<ucn_runtime::DurableRpcCoordinator<16, 16>>("nano_durable_rpc");
        report::<ucn_runtime::DurableRpcCoordinator<64, 64>>("lite_durable_rpc");
        report::<ucn_runtime::DurableRpcCoordinator<128, 128>>("full_durable_rpc");
    }
    #[cfg(feature = "service-rpc")]
    {
        report::<ucn_runtime::RpcBridge<1, 128>>("nano_rpc_bridge");
        report::<ucn_runtime::RpcBridge<2, 256>>("lite_rpc_bridge");
        report::<ucn_runtime::RpcBridge<4, 512>>("full_rpc_bridge");
    }
    report::<ucn_runtime::ControlReplies<1, 128>>("nano_control_replies");
    report::<ucn_runtime::ControlReplies<2, 256>>("lite_control_replies");
    report::<ucn_runtime::ControlReplies<4, 512>>("full_control_replies");
    #[cfg(feature = "operation-control")]
    {
        report::<ucn_runtime::OperationCoordinator<1>>("operation_coordinator");
        report::<ucn_runtime::OperationWatch>("operation_watch");
        report::<ucn_runtime::OperationRecovery>("operation_recovery");
        report::<ucn_runtime::OperationGc>("operation_gc");
        report::<ucn_runtime::ControlCommand>("control_command");
    }
    #[cfg(feature = "journal-runtime")]
    {
        report::<ucn_runtime::JournalRuntime<2, 128, 16, 16>>("nano_journal_runtime");
        report::<ucn_runtime::JournalRuntime<4, 256, 64, 64>>("lite_journal_runtime");
        report::<ucn_runtime::JournalRuntime<8, 512, 128, 128>>("full_journal_runtime");
    }
    #[cfg(feature = "source-runtime")]
    {
        report::<ucn_runtime::SourceRuntime<128, 16>>("nano_source_runtime");
        report::<ucn_runtime::SourceRuntime<256, 64>>("lite_source_runtime");
        report::<ucn_runtime::SourceRuntime<512, 128>>("full_source_runtime");
    }
    #[cfg(feature = "duplex-runtime")]
    {
        report::<ucn_runtime::DuplexRuntime<2, 128, 16, 16, 16>>("nano_duplex_runtime");
        report::<ucn_runtime::DuplexRuntime<4, 256, 64, 64, 64>>("lite_duplex_runtime");
        report::<ucn_runtime::DuplexRuntime<8, 512, 128, 128, 128>>("full_duplex_runtime");
    }
    #[cfg(feature = "transfer")]
    {
        report::<ucn_runtime::TransferBridge<1, 128>>("nano_transfer_bridge");
        report::<ucn_runtime::TransferBridge<2, 256>>("lite_transfer_bridge");
        report::<ucn_runtime::TransferBridge<4, 512>>("full_transfer_bridge");
    }
    report::<ucn_group::GroupAllocator>("group_allocator");
    report::<ucn_group::DynamicGroupOwner<4, 256>>("nano_group_context");
    report::<ucn_group::DynamicGroupOwner<8, 512>>("lite_group_context");
    report::<ucn_group::DynamicGroupOwner<16, 1024>>("full_group_context");
    report::<ucn_realtime::SyncOwner<1, 2>>("nano_sync");
    report::<ucn_realtime::TimeDomain<3>>("nano_time_domain");
    report::<ucn_cluster::ClusterOwner<1, 4, 256>>("nano_cluster");
    report::<ucn_cluster::ClusterOwner<3, 16, 512>>("lite_cluster");
    report::<ucn_cluster::ClusterOwner<7, 32, 1024>>("full_cluster");
}
