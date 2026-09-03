#ifndef UCN_ENDPOINT_H
#define UCN_ENDPOINT_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 静态 Endpoint 是 MCU 默认业务 ABI：不增加任何帧字段，直接复用
 * ucn_frame_t.message_type。每个产品必须在自己的 Endpoint 表中冻结具体
 * 编号、Payload、单位、字节序、版本和 Q0～Q3 语义。 */
typedef uint8_t ucn_endpoint_t;

#define UCN_STATIC_ENDPOINT_FIRST ((ucn_endpoint_t)0x40U)
#define UCN_STATIC_ENDPOINT_LAST ((ucn_endpoint_t)0xBFU)

bool ucn_endpoint_is_static(ucn_endpoint_t endpoint);
bool ucn_message_type_is_control(uint8_t message_type);

#ifdef __cplusplus
}
#endif

#endif
