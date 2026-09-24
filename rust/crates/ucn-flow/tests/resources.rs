//! Flow Profile 固定对象资源门禁；Host 布局不能替代目标 MCU RAM/栈实测。

use core::mem::{align_of, size_of};

use ucn_flow::{FullFlowOwner, LiteFlowOwner, NanoFlowOwner};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoFlowOwner>();
    let lite = size_of::<LiteFlowOwner>();
    let full = size_of::<FullFlowOwner>();
    std::println!("flow_storage nano={nano} lite={lite} full={full}");
    assert!(nano <= 4_096);
    assert!(lite <= 16_384);
    assert!(full <= 49_152);
    assert!(align_of::<NanoFlowOwner>().is_power_of_two());
    assert_eq!(align_of::<NanoFlowOwner>(), align_of::<LiteFlowOwner>());
    assert_eq!(align_of::<LiteFlowOwner>(), align_of::<FullFlowOwner>());
}
