#include "ucn/ucn_endpoint.h"

/*
 * EN: Checks whether an Endpoint belongs to the statically assigned application range.
 * 中文：检查 Endpoint 是否属于静态分配的应用区间。
 */
bool ucn_endpoint_is_static(ucn_endpoint_t endpoint)
{
    return endpoint >= UCN_STATIC_ENDPOINT_FIRST && endpoint <= UCN_STATIC_ENDPOINT_LAST;
}

/*
 * EN: Checks whether a message type belongs to the UCN control plane.
 * 中文：检查消息类型是否属于 UCN 控制面。
 */
bool ucn_message_type_is_control(uint8_t message_type)
{
    return message_type == UCN_MSG_HELLO || message_type == UCN_MSG_JOIN_REQ ||
           message_type == UCN_MSG_JOIN_CHALLENGE ||
           message_type == UCN_MSG_JOIN_ACCEPT ||
           (message_type >= UCN_MSG_ROUTE_REQ &&
            message_type <= UCN_MSG_POLICY_DIAGNOSTIC_REPLY);
}
