#include "internal/ucn_module_scaffold.h"

static const ucn_i_module_scaffold_descriptor_t cluster_scaffold = {
    sizeof(ucn_i_module_scaffold_descriptor_t),
    UCN_I_MODULE_SCAFFOLD_SCHEMA,
    UCN_I_MODULE_CLUSTER,
    8U,
    UCN_I_DEP_COMMON | UCN_I_DEP_COORDINATOR,
    UCN_I_DEP_SECURITY | UCN_I_DEP_ROUTING | UCN_I_DEP_TRANSPORT |
        UCN_I_DEP_PERSISTENCE | UCN_I_DEP_GROUP,
    UCN_I_DEP_SECURITY | UCN_I_DEP_ROUTING | UCN_I_DEP_TRANSPORT |
        UCN_I_DEP_PERSISTENCE | UCN_I_DEP_GROUP,
    UCN_I_MODULE_SCAFFOLD_ONLY | UCN_I_MODULE_FAILS_CLOSED |
        UCN_I_MODULE_PRIVATE,
    0U
};

const ucn_i_module_scaffold_descriptor_t *ucn_i_cluster_scaffold(void)
{
    return &cluster_scaffold;
}
