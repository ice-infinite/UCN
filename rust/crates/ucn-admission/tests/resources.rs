//! Admission Profile 固定对象资源门禁；Host 布局不替代目标 MCU RAM/栈实测。

use core::mem::{align_of, size_of};

use ucn_admission::{FullAdmissionOwner, LiteAdmissionOwner, NanoAdmissionOwner, Reassembly};

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoAdmissionOwner<'static>>();
    let lite = size_of::<LiteAdmissionOwner<'static>>();
    let full = size_of::<FullAdmissionOwner<'static>>();
    assert!(nano <= 4_096);
    assert!(lite <= 16_384);
    assert!(full <= 32_768);
    assert!(size_of::<Reassembly>() <= 1_024);
    assert!(align_of::<NanoAdmissionOwner<'static>>().is_power_of_two());
    assert_eq!(
        align_of::<NanoAdmissionOwner<'static>>(),
        align_of::<LiteAdmissionOwner<'static>>()
    );
    assert_eq!(
        align_of::<LiteAdmissionOwner<'static>>(),
        align_of::<FullAdmissionOwner<'static>>()
    );
    std::println!(
        "admission_storage nano={nano} lite={lite} full={full} reassembly={}",
        size_of::<Reassembly>()
    );
}
