#include "internal/ucn_composition.h"

#include <stdio.h>
#include <string.h>

static int failures;

static ucn_i_composition_module_mask_t compiled_module_mask(void)
{
    return UCN_I_COMPOSITION_COMPILED_MODULE_MASK;
}

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

typedef struct capability_case {
    ucn_i_composition_capability_mask_t capability;
    ucn_i_composition_module_mask_t modules;
    ucn_i_composition_module_mask_t durable;
} capability_case_t;

static ucn_i_composition_request_t request_make(
    ucn_i_composition_module_mask_t modules,
    ucn_i_composition_capability_mask_t capabilities)
{
    ucn_i_composition_request_t request;

    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask = modules;
    request.capability_mask = capabilities;
    return request;
}

static void test_minimal_and_zero_write(void)
{
    ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_plan_t before;

    memset(&plan, 0xA5, sizeof(plan));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(plan.selected_module_mask == UCN_I_COMPOSE_FOUNDATION);
    CHECK(plan.durable_module_mask == 0U);
    CHECK(plan.init_count == 0U);
    CHECK(plan.private_owner_bytes > 0U);
    CHECK(plan.composition_digest != 0U);
    CHECK(ucn_i_composition_plan_valid(&plan));

    before = plan;
    request.available_module_mask = 0U;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);

    request = request_make(UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    request.reserved_zero = 1U;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);

    request = request_make(UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    CHECK(ucn_i_composition_plan_build(
              &request, (ucn_i_composition_plan_t *)(void *)&request) ==
          UCN_ERR_ARGUMENT);
}

static void test_capability_closure(void)
{
    static const capability_case_t cases[] = {
        {UCN_I_CAP_PROTECTED_C1,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY,
         UCN_I_COMPOSE_SECURITY},
        {UCN_I_CAP_DYNAMIC_ADMISSION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_ADMISSION |
             UCN_I_COMPOSE_CAPABILITY,
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY},
        {UCN_I_CAP_AUTO_ROUTE, UCN_I_COMPOSE_ROUTE, 0U},
        {UCN_I_CAP_ADVANCED_FLOW,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
             UCN_I_COMPOSE_FLOW,
         UCN_I_COMPOSE_SECURITY},
        {UCN_I_CAP_RELIABLE, UCN_I_COMPOSE_TRANSPORT, 0U},
        {UCN_I_CAP_TRANSFER,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_TRANSPORT,
         UCN_I_COMPOSE_TRANSPORT},
        {UCN_I_CAP_SERVICE, UCN_I_COMPOSE_SERVICE, 0U},
        {UCN_I_CAP_DURABLE_OPERATION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SERVICE,
         UCN_I_COMPOSE_SERVICE},
        {UCN_I_CAP_LOCAL_STAMP, UCN_I_COMPOSE_REALTIME, 0U},
        {UCN_I_CAP_NETWORK_TIME,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
             UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_REALTIME,
         UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_REALTIME},
        {UCN_I_CAP_STATIC_GROUP, UCN_I_COMPOSE_GROUP, 0U},
        {UCN_I_CAP_DYNAMIC_GROUP,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_GROUP,
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_GROUP},
        {UCN_I_CAP_CLUSTER,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
             UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
             UCN_I_COMPOSE_CLUSTER,
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CLUSTER},
        {UCN_I_CAP_CLUSTER_GROUP_ACCELERATION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
             UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
             UCN_I_COMPOSE_GROUP | UCN_I_COMPOSE_CLUSTER,
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CLUSTER}
    };
    size_t case_index;

    for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]);
         ++case_index) {
        const ucn_i_composition_module_mask_t exact_modules =
            UCN_I_COMPOSE_FOUNDATION | cases[case_index].modules;
        ucn_i_composition_request_t request = request_make(
            exact_modules, UCN_I_CAP_STATIC_C1 | cases[case_index].capability);
        ucn_i_composition_plan_t plan;
        uint8_t bit;

        memset(&plan, 0x5A, sizeof(plan));
        if ((exact_modules & ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) != 0U) {
            ucn_i_composition_plan_t before = plan;
            CHECK(ucn_i_composition_plan_build(&request, &plan) ==
                  UCN_ERR_UNSUPPORTED);
            CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
            continue;
        }
        CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
        CHECK(plan.selected_module_mask == exact_modules);
        CHECK(plan.durable_module_mask == cases[case_index].durable);
        CHECK(ucn_i_composition_plan_valid(&plan));

        for (bit = 1U; bit < 13U; ++bit) {
            const uint32_t module = UINT32_C(1) << bit;
            if ((cases[case_index].modules & module) != 0U) {
                ucn_i_composition_plan_t sentinel;
                ucn_i_composition_plan_t before;
                request.available_module_mask = exact_modules & ~module;
                memset(&sentinel, 0xC3, sizeof(sentinel));
                before = sentinel;
                CHECK(ucn_i_composition_plan_build(&request, &sentinel) ==
                      UCN_ERR_CONFIG);
                CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
            }
        }
    }
}

static void test_intrinsic_closure_and_independence(void)
{
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t plan;
    ucn_i_composition_plan_t with_realtime;
    const ucn_i_composition_module_mask_t cluster_modules =
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
        UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
        UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
        UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_CLUSTER;

    request = request_make(UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_SECURITY,
                           UCN_I_CAP_STATIC_C1 | UCN_I_CAP_PROTECTED_C1);
    memset(&plan, 0, sizeof(plan));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_ERR_CONFIG);

    request = request_make(UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_GROUP,
                           UCN_I_CAP_STATIC_C1 | UCN_I_CAP_STATIC_GROUP);
    if ((compiled_module_mask() & UCN_I_COMPOSE_GROUP) != 0U) {
        CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
        CHECK((plan.selected_module_mask & UCN_I_COMPOSE_CLUSTER) == 0U);
        CHECK((plan.selected_module_mask & UCN_I_COMPOSE_REALTIME) == 0U);
    } else {
        CHECK(ucn_i_composition_plan_build(&request, &plan) ==
              UCN_ERR_UNSUPPORTED);
    }

    request = request_make(cluster_modules,
                           UCN_I_CAP_STATIC_C1 | UCN_I_CAP_CLUSTER);
    if ((cluster_modules & ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) != 0U) {
        CHECK(ucn_i_composition_plan_build(&request, &plan) ==
              UCN_ERR_UNSUPPORTED);
        return;
    }
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK((plan.selected_module_mask & UCN_I_COMPOSE_GROUP) == 0U);
    CHECK((plan.selected_module_mask & UCN_I_COMPOSE_REALTIME) == 0U);

    request.available_module_mask |= UCN_I_COMPOSE_REALTIME;
    request.capability_mask |= UCN_I_CAP_LOCAL_STAMP;
    if ((compiled_module_mask() & UCN_I_COMPOSE_REALTIME) != 0U) {
        CHECK(ucn_i_composition_plan_build(&request, &with_realtime) == UCN_OK);
        CHECK(plan.composition_digest != with_realtime.composition_digest);
        CHECK(with_realtime.selected_module_mask ==
              (plan.selected_module_mask | UCN_I_COMPOSE_REALTIME));
    } else {
        CHECK(ucn_i_composition_plan_build(&request, &with_realtime) ==
              UCN_ERR_UNSUPPORTED);
    }
}

static void test_deterministic_order_digest_and_tamper(void)
{
    ucn_i_composition_capability_mask_t compiled_capabilities =
        UCN_I_CAP_STATIC_C1;
    ucn_i_composition_module_mask_t composed_modules =
        UCN_I_COMPOSE_FOUNDATION;
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t first;
    ucn_i_composition_plan_t second;
    size_t case_index;
    uint8_t index;

    static const capability_case_t optional_cases[] = {
        {UCN_I_CAP_PROTECTED_C1,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY, 0U},
        {UCN_I_CAP_DYNAMIC_ADMISSION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_ADMISSION |
             UCN_I_COMPOSE_CAPABILITY,
         0U},
        {UCN_I_CAP_AUTO_ROUTE, UCN_I_COMPOSE_ROUTE, 0U},
        {UCN_I_CAP_ADVANCED_FLOW,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
             UCN_I_COMPOSE_FLOW,
         0U},
        {UCN_I_CAP_RELIABLE, UCN_I_COMPOSE_TRANSPORT, 0U},
        {UCN_I_CAP_TRANSFER,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_TRANSPORT, 0U},
        {UCN_I_CAP_SERVICE, UCN_I_COMPOSE_SERVICE, 0U},
        {UCN_I_CAP_DURABLE_OPERATION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SERVICE, 0U},
        {UCN_I_CAP_LOCAL_STAMP, UCN_I_COMPOSE_REALTIME, 0U},
        {UCN_I_CAP_NETWORK_TIME,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
             UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
             UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_REALTIME,
         0U},
        {UCN_I_CAP_STATIC_GROUP, UCN_I_COMPOSE_GROUP, 0U},
        {UCN_I_CAP_DYNAMIC_GROUP,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_GROUP,
         0U},
        {UCN_I_CAP_CLUSTER,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
             UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
             UCN_I_COMPOSE_CLUSTER,
         0U},
        {UCN_I_CAP_CLUSTER_GROUP_ACCELERATION,
         UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
             UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
             UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
             UCN_I_COMPOSE_GROUP | UCN_I_COMPOSE_CLUSTER,
         0U}
    };

    for (case_index = 0U;
         case_index < sizeof(optional_cases) / sizeof(optional_cases[0]);
         ++case_index) {
        if ((optional_cases[case_index].modules &
             ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) == 0U) {
            compiled_capabilities |= optional_cases[case_index].capability;
            composed_modules |= optional_cases[case_index].modules;
        }
    }
    request = request_make(composed_modules, compiled_capabilities);

    CHECK(ucn_i_composition_plan_build(&request, &first) == UCN_OK);
    CHECK(ucn_i_composition_plan_build(&request, &second) == UCN_OK);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    CHECK(first.compiled_module_mask ==
          UCN_I_COMPOSITION_COMPILED_MODULE_MASK);
    for (index = 0U; index < first.init_count; ++index) {
        if (index > 0U) {
            CHECK(first.init_order[index] > first.init_order[index - 1U]);
        }
    }
    CHECK(first.private_owner_bytes > 0U);
    CHECK(ucn_i_composition_plan_valid(&first));

    second.composition_digest ^= UINT64_C(1);
    CHECK(!ucn_i_composition_plan_valid(&second));
    second = first;
    second.init_order[2] = second.init_order[1];
    CHECK(!ucn_i_composition_plan_valid(&second));
    second = first;
    second.private_owner_bytes += 1U;
    CHECK(!ucn_i_composition_plan_valid(&second));
}

int main(void)
{
    ucn_i_composition_request_t full_request;
    ucn_i_composition_plan_t full_plan;

    if ((compiled_module_mask() & UCN_I_COMPOSE_FOUNDATION) == 0U) {
        const ucn_i_composition_request_t request = request_make(
            UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
        ucn_i_composition_plan_t plan;
        ucn_i_composition_plan_t before;

        memset(&plan, 0xA5, sizeof(plan));
        before = plan;
        CHECK(ucn_i_composition_plan_build(&request, &plan) ==
              UCN_ERR_UNSUPPORTED);
        CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    } else {
        test_minimal_and_zero_write();
        test_capability_closure();
        test_intrinsic_closure_and_independence();
        test_deterministic_order_digest_and_tamper();
    }
    if (failures != 0) {
        fprintf(stderr, "composition failures=%d\n", failures);
        return 1;
    }
    full_request = request_make(UCN_I_COMPOSITION_COMPILED_MODULE_MASK |
                                    UCN_I_COMPOSE_FOUNDATION,
                                UCN_I_COMPOSE_KNOWN_CAPABILITIES);
    if (ucn_i_composition_plan_build(&full_request, &full_plan) == UCN_OK) {
        printf("composition tests passed profile=%u owner_bytes=%lu "
               "selected=0x%08lX digest=%016llX\n",
               (unsigned)UCN_PROFILE,
               (unsigned long)full_plan.private_owner_bytes,
               (unsigned long)full_plan.selected_module_mask,
               (unsigned long long)full_plan.composition_digest);
    } else {
        printf("composition tests passed profile=%u feature_off_plan=bounded\n",
               (unsigned)UCN_PROFILE);
    }
    return 0;
}
