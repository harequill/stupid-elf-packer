#ifndef STUPID_ELF_PACKER_XOR_H
#define STUPID_ELF_PACKER_XOR_H

#include <stdint.h>

/* In-place XOR with a repeating 8-byte key; its own inverse. Shared so the tool
   (encrypt) and the stub (decrypt) run the exact same operation. */
static inline void xor_apply(uint8_t *data, uint64_t size, uint64_t key) {
    const uint8_t *k = (const uint8_t *)&key;
    for (uint64_t i = 0; i < size; i++)
        data[i] ^= k[i & 7];
}

#endif
