//! Security Profile 固定对象资源门禁；Host 布局不能替代目标 MCU 栈与 RAM 实测。

use core::mem::{align_of, size_of};

use ucn_security::{
    FullSecurityOwner, LiteSecurityOwner, NanoSecurityOwner, SecurityPacketWorkspace,
};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoSecurityOwner<'static>>();
    let lite = size_of::<LiteSecurityOwner<'static>>();
    let full = size_of::<FullSecurityOwner<'static>>();
    assert!(nano <= 4_096);
    assert!(lite <= 16_384);
    assert!(full <= 32_768);
    assert!(align_of::<NanoSecurityOwner<'static>>().is_power_of_two());
    assert_eq!(
        align_of::<NanoSecurityOwner<'static>>(),
        align_of::<LiteSecurityOwner<'static>>()
    );
    assert_eq!(
        align_of::<LiteSecurityOwner<'static>>(),
        align_of::<FullSecurityOwner<'static>>()
    );
    std::println!(
        "security_storage nano={nano} lite={lite} full={full} work256={} work512={}",
        size_of::<SecurityPacketWorkspace<256>>(),
        size_of::<SecurityPacketWorkspace<512>>()
    );
}
