#ifndef UCN_EXAMPLE_HEALTH_SCHEMA_H
#define UCN_EXAMPLE_HEALTH_SCHEMA_H
#include <stdint.h>
#include <stddef.h>

/* Business-only, never memcpy a C struct onto Wire.
 * 纯业务Schema，禁止把结构体内存直接当线格式。 */
typedef struct ucn_example_health {
    uint32_t voltage_mv;
    int32_t current_ma;
    int16_t temperature_centi_c;
    uint8_t mode, flags;
    uint32_t fault_bits;
} ucn_example_health_t;

static void ucn_health_put32(uint8_t *p, uint32_t x)
{ p[0] = (uint8_t)(x >> 24U); p[1] = (uint8_t)(x >> 16U); p[2] = (uint8_t)(x >> 8U); p[3] = (uint8_t)x; }
static uint32_t ucn_health_get32(const uint8_t *p)
{ return (uint32_t)p[0] << 24U | (uint32_t)p[1] << 16U | (uint32_t)p[2] << 8U | p[3]; }
static int ucn_health_encode(const ucn_example_health_t *v, uint8_t *out, size_t bytes)
{
    uint16_t t;
    if (v == NULL || out == NULL || bytes != 16U) { return 0; }
    t = (uint16_t)v->temperature_centi_c;
    ucn_health_put32(out, v->voltage_mv); ucn_health_put32(out + 4U, (uint32_t)v->current_ma);
    out[8] = (uint8_t)(t >> 8U); out[9] = (uint8_t)t;
    out[10] = v->mode; out[11] = v->flags; ucn_health_put32(out + 12U, v->fault_bits);
    return 1;
}
static int ucn_health_decode(const uint8_t *in, size_t bytes, ucn_example_health_t *out)
{
    uint32_t current;
    uint16_t t;
    if (in == NULL || out == NULL || bytes != 16U) { return 0; }
    current = ucn_health_get32(in + 4U); t = (uint16_t)((uint16_t)in[8] << 8U | in[9]);
    out->voltage_mv = ucn_health_get32(in);
    out->current_ma = current <= INT32_MAX ? (int32_t)current : -1 - (int32_t)(UINT32_MAX - current);
    out->temperature_centi_c = t <= INT16_MAX ? (int16_t)t : (int16_t)(-1 - (int32_t)(UINT16_MAX - t));
    out->mode = in[10]; out->flags = in[11]; out->fault_bits = ucn_health_get32(in + 12U);
    return 1;
}
#endif
