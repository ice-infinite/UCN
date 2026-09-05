#ifndef UCN_V6_BOOTSTRAP_PRIVATE_H
#define UCN_V6_BOOTSTRAP_PRIVATE_H

#include "ucn/v6/ucn_v6_bootstrap.h"

#define UCN_V6_BOOTSTRAP_OWNER_MAGIC UINT32_C(0x56425336)
#define UCN_V6_BOOTSTRAP_OWNER_CANARY UINT64_C(0x4D9E17A2C8036BF5)

struct ucn_v6_bootstrap_owner {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_bootstrap_config_t config;
    ucn_v6_bootstrap_pending_t join_pending[UCN_V6_BOOTSTRAP_MAX_PENDING];
    ucn_v6_bootstrap_pending_t reauth_pending[UCN_V6_BOOTSTRAP_MAX_PENDING];
    ucn_v6_bootstrap_link_budget_t budgets[UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS];
    ucn_v6_bootstrap_verifier_ops_t verifier;
    ucn_v6_callback_gate_t *callback_gate;
    bool initialized;
    uint64_t canary;
};

#endif
