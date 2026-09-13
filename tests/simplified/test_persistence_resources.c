#include "internal/ucn_persistence.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            abort();                                                        \
        }                                                                   \
    } while (0)

UCN_STATIC_ASSERT(sizeof(struct ucn_persistence_owner) <=
                      UCN_PERSIST_STORAGE_BYTES,
                  persistence_owner_fits_declared_storage);
UCN_STATIC_ASSERT(sizeof(struct ucn_persist_callback_gate) <=
                      UCN_PERSIST_GATE_STORAGE_BYTES,
                  persistence_gate_fits_declared_storage);
UCN_STATIC_ASSERT(UCN_PERSIST_SLOT_BYTES <= UINT32_MAX,
                  persistence_slot_size_fits_provider_completion);

int main(void)
{
    const size_t owner_bytes = sizeof(struct ucn_persistence_owner);
    const size_t gate_bytes = sizeof(struct ucn_persist_callback_gate);

    CHECK(ucn_persistence_storage_required() ==
           UCN_PERSIST_STORAGE_BYTES);
    CHECK(ucn_persist_gate_storage_required() ==
           UCN_PERSIST_GATE_STORAGE_BYTES);
    CHECK(owner_bytes <= UCN_PERSIST_STORAGE_BYTES);
    CHECK(gate_bytes <= UCN_PERSIST_GATE_STORAGE_BYTES);
    printf("persistence profile=%u domains=%u body=%u slot=%u "
           "owner=%zu/%u gate=%zu/%u\n",
           (unsigned)UCN_PROFILE,
           (unsigned)UCN_PERSIST_DOMAIN_COUNT,
           (unsigned)UCN_PERSIST_BODY_BYTES,
           (unsigned)UCN_PERSIST_SLOT_BYTES,
           owner_bytes,
           (unsigned)UCN_PERSIST_STORAGE_BYTES,
           gate_bytes,
           (unsigned)UCN_PERSIST_GATE_STORAGE_BYTES);
    return 0;
}
