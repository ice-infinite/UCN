#include "ucn/ucn_endpoint.h"

bool ucn_endpoint_is_static(ucn_endpoint_t endpoint)
{
    return endpoint >= UCN_STATIC_ENDPOINT_FIRST && endpoint <= UCN_STATIC_ENDPOINT_LAST;
}

bool ucn_message_type_is_control(uint8_t message_type)
{
    return message_type == UCN_MSG_HELLO || message_type == UCN_MSG_JOIN_REQ ||
           message_type == UCN_MSG_JOIN_CHALLENGE ||
           message_type == UCN_MSG_JOIN_ACCEPT ||
           (message_type >= UCN_MSG_ROUTE_REQ &&
            message_type <= UCN_MSG_POLICY_DIAGNOSTIC_REPLY);
}
