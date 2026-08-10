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
    ucn_result_t (*step_fn)(ucn_node_t *, uint32_t) = ucn_node_step;
    const ucn_node_stats_t *(*stats_fn)(const ucn_node_t *) =
        ucn_node_get_stats;

    return node == NULL && step_fn != NULL && stats_fn != NULL ? 0 : 1;
}
