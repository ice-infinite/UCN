//! 编译期 Profile Storage 上限门禁；数值只证明 Host 类型布局，不冒充目标 MCU 实测。

use core::mem::{align_of, size_of};

use ucn_persistence::{CodecWorkspace, PersistenceOwner, ProviderGate};

type NanoOwner = PersistenceOwner<'static, 2, 256, 368>;
type LiteOwner = PersistenceOwner<'static, 4, 512, 624>;
type FullOwner = PersistenceOwner<'static, 8, 1024, 1136>;

#[test]
fn profile_storage_is_fixed_and_bounded() {
    let nano = size_of::<NanoOwner>();
    let lite = size_of::<LiteOwner>();
    let full = size_of::<FullOwner>();
    assert!(nano <= 4_096);
    assert!(lite <= 12_288);
    assert!(full <= 32_768);
    assert!(align_of::<NanoOwner>().is_power_of_two());
    assert_eq!(align_of::<NanoOwner>(), align_of::<LiteOwner>());
    assert_eq!(align_of::<LiteOwner>(), align_of::<FullOwner>());
    std::println!(
        "persistence_storage nano={nano} lite={lite} full={full} workspace={} gate={}",
        size_of::<CodecWorkspace>(),
        size_of::<ProviderGate>()
    );
}
