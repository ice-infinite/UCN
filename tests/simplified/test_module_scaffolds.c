#include "internal/ucn_module_scaffold.h"

#include <stdio.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef const ucn_i_module_scaffold_descriptor_t *(*descriptor_fn_t)(void);

static int test_descriptor_registry(void)
{
    static const descriptor_fn_t descriptor_functions[UCN_I_MODULE_COUNT] = {
        ucn_i_security_scaffold,
        ucn_i_admission_scaffold,
        ucn_i_routing_scaffold,
        ucn_i_transport_scaffold,
        ucn_i_service_scaffold,
        ucn_i_realtime_scaffold,
        ucn_i_group_scaffold,
        ucn_i_cluster_scaffold
    };
    uint16_t seen = 0U;
    uint8_t index;

    for (index = 0U; index < UCN_I_MODULE_COUNT; ++index) {
        const ucn_i_module_scaffold_descriptor_t *descriptor =
            descriptor_functions[index]();
        uint32_t sentinel = UINT32_C(0xA5A55A5A);

        CHECK(descriptor != NULL);
        CHECK(ucn_i_module_scaffold_validate(descriptor) == UCN_OK);
        CHECK(descriptor->module_id == index);
        CHECK((seen & (UINT16_C(1) << descriptor->module_id)) == 0U);
        seen |= (UINT16_C(1) << descriptor->module_id);
        CHECK(ucn_i_module_scaffold_business_probe(
                  descriptor, &sentinel) == UCN_ERR_UNSUPPORTED);
        CHECK(sentinel == UINT32_C(0xA5A55A5A));
    }
    CHECK(seen == ((UINT16_C(1) << UCN_I_MODULE_COUNT) - 1U));
    return 0;
}

static int test_optional_branch_isolation(void)
{
    const ucn_i_module_scaffold_descriptor_t *realtime =
        ucn_i_realtime_scaffold();
    const ucn_i_module_scaffold_descriptor_t *group =
        ucn_i_group_scaffold();
    const ucn_i_module_scaffold_descriptor_t *cluster =
        ucn_i_cluster_scaffold();

    CHECK((realtime->coordinator_visible_mask &
           (UCN_I_DEP_GROUP | UCN_I_DEP_CLUSTER)) == 0U);
    CHECK((group->coordinator_visible_mask &
           (UCN_I_DEP_REALTIME | UCN_I_DEP_CLUSTER)) == 0U);
    CHECK((cluster->coordinator_visible_mask & UCN_I_DEP_REALTIME) == 0U);
    return 0;
}

static int test_malformed_descriptor_fails_closed(void)
{
    ucn_i_module_scaffold_descriptor_t malformed =
        *ucn_i_security_scaffold();
    uint32_t sentinel = UINT32_C(0x11223344);

    malformed.direct_dependency_mask |= UCN_I_DEP_SECURITY;
    CHECK(ucn_i_module_scaffold_validate(&malformed) == UCN_ERR_CONFIG);
    CHECK(ucn_i_module_scaffold_business_probe(
              &malformed, &sentinel) == UCN_ERR_CONFIG);
    CHECK(sentinel == UINT32_C(0x11223344));
    malformed = *ucn_i_security_scaffold();
    malformed.coordinator_visible_mask |= UCN_I_DEP_COORDINATOR;
    malformed.forbidden_direct_owner_mask =
        malformed.coordinator_visible_mask;
    CHECK(ucn_i_module_scaffold_validate(&malformed) == UCN_ERR_CONFIG);
    CHECK(ucn_i_module_scaffold_business_probe(
              ucn_i_security_scaffold(), NULL) == UCN_ERR_ARGUMENT);
    return 0;
}

int main(void)
{
    CHECK(test_descriptor_registry() == 0);
    CHECK(test_optional_branch_isolation() == 0);
    CHECK(test_malformed_descriptor_fails_closed() == 0);
    puts("v6 simplified C module scaffolds passed");
    return 0;
}
