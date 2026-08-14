#ifndef UCN_TEST_TRANSFER_CONCURRENCY_CONFIG_H
#define UCN_TEST_TRANSFER_CONCURRENCY_CONFIG_H

/* Host regression profile for the opt-in multi-message Transfer contract. */
#ifndef UCN_TRANSFER_TX_SLOTS
#define UCN_TRANSFER_TX_SLOTS ((size_t)4U)
#endif

#ifndef UCN_TRANSFER_RX_SLOTS
#define UCN_TRANSFER_RX_SLOTS ((size_t)4U)
#endif

#endif
