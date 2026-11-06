#include "ucn/ucn_native.h"
#include <pthread.h>
#include <sched.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define main unused_core_tests_main
#include "test_core_static.c"
#undef main

static pthread_mutex_t native_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool producer_done, destroy_done;
static ucn_native_storage_t native_storage;
static ucn_native_t *root;
static ucn_native_handle_t publisher_handle;
static ucn_result_t native_lock(void *p) { return pthread_mutex_lock((pthread_mutex_t *)p) == 0 ? UCN_OK : UCN_ERR_STATE; }
static void native_unlock(void *p) { (void)pthread_mutex_unlock((pthread_mutex_t *)p); }
static bool is_done(bool *flag) { bool v; assert(pthread_mutex_lock(&done_mutex) == 0); v = *flag; assert(pthread_mutex_unlock(&done_mutex) == 0); return v; }
static void set_done(bool *flag) { assert(pthread_mutex_lock(&done_mutex) == 0); *flag = true; assert(pthread_mutex_unlock(&done_mutex) == 0); }
static void *worker(void *unused)
{
    uint64_t now = 1U;
    ucn_native_status_t status;
    (void)unused;
    while (!is_done(&producer_done)) { assert(ucn_native_step(root, ++now, 8U) == UCN_OK); sched_yield(); }
    assert(ucn_native_stop(root) == UCN_OK);
    do {
        assert(ucn_native_step(root, ++now, 8U) == UCN_OK);
        assert(ucn_native_status(root, &status) == UCN_OK);
    } while (!status.core_quiescent || status.pending_commands);
    assert(ucn_native_deinit(root) == UCN_OK);
    set_done(&destroy_done);
    return NULL;
}
int main(void)
{
    fake_link_t fake;
    ucn_node_t *node = NULL;
    ucn_path_handle_t path;
    ucn_static_path_t pc = path_config();
    ucn_native_channel_t c;
    ucn_native_config_t cfg;
    pthread_t thread;
    unsigned i;
    uint8_t payload = 1U;
    memset(&fake, 0, sizeof(fake)); fake.submit_result = UCN_DRIVER_COMPLETE;
    assert(make_node(&storage_a, 700U, 1U, &fake, &node) == UCN_OK);
    assert(ucn_static_path_add(node, &pc, &path) == UCN_OK);
    memset(&c, 0, sizeof(c)); c.id = 1U; c.service_id = 0x1234U; c.schema_id = 1U;
    c.schema_revision = 1U; c.payload_bytes = 1U; c.traffic_class = UCN_TRAFFIC_Q1;
    c.queue_policy = UCN_NATIVE_FIFO; c.security_minimum = UCN_NATIVE_STATIC_TRUSTED;
    c.max_local_age_us = 10000000U; c.source_count = 1U; c.target_count = 1U;
    c.sources[0].address = 1U; c.sources[0].binding_generation = 10U; c.sources[0].principal_digest = 0x1111U;
    c.targets[0].address = 2U; c.targets[0].binding_generation = 20U;
    memset(&cfg, 0, sizeof(cfg)); cfg.api_version = UCN_NATIVE_API_VERSION; cfg.storage_layout = UCN_NATIVE_LAYOUT;
    cfg.instance = 800U; cfg.manifest_hash = UCN_NATIVE_MANIFEST_HASH; cfg.channels = &c; cfg.channel_count = 1U;
    cfg.lock.struct_size = sizeof(cfg.lock); cfg.lock.api_version = UCN_API_VERSION;
    cfg.lock.context = &native_mutex; cfg.lock.enter = native_lock; cfg.lock.leave = native_unlock;
    assert(ucn_native_init(&native_storage, sizeof(native_storage), node, &cfg, &root) == UCN_OK);
    assert(ucn_publisher_create(root, 1U, &publisher_handle) == UCN_OK);
    assert(ucn_native_start(root, 1U) == UCN_OK);
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
    for (i = 0U; i < 20000U; ++i) {
        ucn_result_t r = ucn_publish(root, publisher_handle, &payload, 1U);
        assert(r == UCN_OK || r == UCN_ERR_STATE || r == UCN_ERR_NO_SPACE);
    }
    /* close remains a fixed latch even during the protocol worker's step. */
    assert(ucn_publisher_close(root, publisher_handle) == UCN_OK);
    set_done(&producer_done);
    while (!is_done(&destroy_done)) {
        ucn_native_status_t status;
        ucn_result_t r = ucn_native_status(root, &status);
        assert(r == UCN_OK || r == UCN_ERR_STATE);
        sched_yield();
    }
    assert(pthread_join(thread, NULL) == 0);
    puts("Native concurrent publish/close/status/retirement passed");
    return 0;
}
