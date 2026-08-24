#include "ucn/ucn_cluster_storage.h"

int test_cluster_storage_header(void)
{
    ucn_cluster_t cluster;

    return sizeof(cluster) > 0U && UCN_CLUSTER_API_VERSION == 2U &&
                   UCN_CLUSTER_STORAGE_LAYOUT_VERSION == 2U ?
               0 :
               1;
}
