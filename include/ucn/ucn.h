#ifndef UCN_H
#define UCN_H

/*
 * UCN v6 public umbrella header.
 * UCN v3/v4/v5 APIs are intentionally absent from the v6 release surface.
 */
#include "ucn/v6/ucn_v6_config.h"
#include "ucn/v6/ucn_v6_types.h"
#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_capability.h"
#include "ucn/v6/ucn_v6_identity.h"
#include "ucn/v6/ucn_v6_message.h"
#include "ucn/v6/ucn_v6_owner.h"
#include "ucn/v6/ucn_v6_qos.h"
#include "ucn/v6/ucn_v6_route.h"
#include "ucn/v6/ucn_v6_security.h"
#include "ucn/v6/ucn_v6_transfer.h"
#include "ucn/v6/ucn_v6_wire.h"

#if UCN_V6_FEATURE_REALTIME_ENABLED
#include "ucn/v6/ucn_v6_realtime.h"
#endif

#if UCN_V6_FEATURE_CLUSTER_ENABLED
#include "ucn/v6/ucn_v6_cluster.h"
#endif

#if UCN_V6_FEATURE_ADAPTER_ENABLED
#include "ucn/v6/ucn_v6_adapter.h"
#include "ucn/v6/ucn_v6_runtime.h"
#include "ucn/v6/adapters/ucn_v6_can.h"
#include "ucn/v6/adapters/ucn_v6_uart.h"
#include "ucn/v6/adapters/ucn_v6_usb.h"
#include "ucn/v6/adapters/ucn_v6_wifi.h"
#include "ucn/v6/ports/ucn_v6_freertos.h"
#include "ucn/v6/reference/esp32s3/ucn_v6_esp32s3.h"
#endif

#endif
