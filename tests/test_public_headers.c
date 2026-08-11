#include <stddef.h>

#include "ucn/ucn_adapter.h"
#include "ucn/ucn_node.h"
#if UCN_FEATURE_SERVICE
#include "ucn/ucn_service_bridge.h"
#endif

/* This translation unit deliberately does not include ucn_node_storage.h.
 * It proves that pointer-only application/Adapter/Bridge declarations can use
 * the public API without seeing the Node implementation layout. */
int test_public_headers(void)
{
    ucn_node_t *node = NULL;
    const ucn_path_capability_t capability = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        64U
    };
    ucn_result_t (*step_fn)(ucn_node_t *, uint32_t) = ucn_node_step;
    const ucn_node_stats_t *(*stats_fn)(const ucn_node_t *) =
        ucn_node_get_stats;

    return node == NULL && step_fn != NULL && stats_fn != NULL &&
           ucn_node_install_local_path_capable(
               node, 1U, 2U, 2U, 1U, 1000U, &capability) == UCN_ERR_ARGUMENT &&
           ucn_node_send_path_install_capable(
               node, 2U, 1U, 2U, 0U, 0U, 1000U,
               &capability) == UCN_ERR_ARGUMENT ? 0 : 1;
}
