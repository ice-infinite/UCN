#include "ucn/ucn_node_storage.h"

/* The Protocol Task owner still gets compile-time static allocation with no
 * heap and no generated size table. */
int test_node_storage_header(void)
{
    ucn_node_t node;

    return sizeof(node) > 0U && UCN_NODE_STORAGE_LAYOUT_VERSION == 8U ? 0 : 1;
}
