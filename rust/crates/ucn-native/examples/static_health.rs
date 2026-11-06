//! Host software loopback only; never opens physical links or COM ports.
#[allow(dead_code)]
#[path = "../tests/static_pubsub.rs"]
mod software_fixture;
fn main() {
    software_fixture::run_health_demo();
}
