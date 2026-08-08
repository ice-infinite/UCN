#ifndef UCN_H
#define UCN_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *ucn_version(void);
ucn_result_t ucn_validate_config(const ucn_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
