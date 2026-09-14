//! Identity Profile 固定对象资源门禁；Host 布局不替代目标 MCU RAM/栈实测。

use core::mem::{align_of, size_of};

use ucn_identity::{FullIdentityOwner, LiteIdentityOwner, NanoIdentityOwner};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoIdentityOwner<'static>>();
    let lite = size_of::<LiteIdentityOwner<'static>>();
    let full = size_of::<FullIdentityOwner<'static>>();
    assert!(nano <= 4_096);
    assert!(lite <= 12_288);
    assert!(full <= 24_576);
    assert!(align_of::<NanoIdentityOwner<'static>>().is_power_of_two());
    assert_eq!(
        align_of::<NanoIdentityOwner<'static>>(),
        align_of::<LiteIdentityOwner<'static>>()
    );
    assert_eq!(
        align_of::<LiteIdentityOwner<'static>>(),
        align_of::<FullIdentityOwner<'static>>()
    );
    std::println!("identity_storage nano={nano} lite={lite} full={full}");
}
