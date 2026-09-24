//! Routing Profile 固定对象资源门禁；Host 布局不能替代目标 MCU RAM/栈实测。

use core::mem::{align_of, size_of};

use ucn_routing::{FullRouteOwner, LiteRouteOwner, NanoRouteOwner};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoRouteOwner>();
    let lite = size_of::<LiteRouteOwner>();
    let full = size_of::<FullRouteOwner>();
    std::println!("routing_storage nano={nano} lite={lite} full={full}");
    assert!(nano <= 2_048);
    assert!(lite <= 6_144);
    assert!(full <= 16_384);
    assert!(align_of::<NanoRouteOwner>().is_power_of_two());
    assert_eq!(align_of::<NanoRouteOwner>(), align_of::<LiteRouteOwner>());
    assert_eq!(align_of::<LiteRouteOwner>(), align_of::<FullRouteOwner>());
}
