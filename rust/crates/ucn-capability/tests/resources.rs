//! Capability Profile 固定对象资源门禁；Host 布局不替代目标 MCU RAM/栈实测。

use core::mem::{align_of, size_of};

use ucn_capability::{FullCapabilityOwner, LiteCapabilityOwner, NanoCapabilityOwner};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoCapabilityOwner>();
    let lite = size_of::<LiteCapabilityOwner>();
    let full = size_of::<FullCapabilityOwner>();
    assert!(nano <= 1_024);
    assert!(lite <= 4_096);
    assert!(full <= 8_192);
    assert!(align_of::<NanoCapabilityOwner>().is_power_of_two());
    assert_eq!(
        align_of::<NanoCapabilityOwner>(),
        align_of::<LiteCapabilityOwner>()
    );
    assert_eq!(
        align_of::<LiteCapabilityOwner>(),
        align_of::<FullCapabilityOwner>()
    );
    std::println!("capability_storage nano={nano} lite={lite} full={full}");
}
