#ifndef STUPID_ELF_PACKER_SHARED_H
#define STUPID_ELF_PACKER_SHARED_H

#include <stdint.h>

/* Metadata block the tool appends after the stub; shared layout, oep at 0.
   code_addr/code_size/key are zero when the tool packs without encryption. */
struct packer_meta {
    uint64_t oep;
    uint64_t code_addr;
    uint64_t code_size;
    uint64_t key;
};

#endif
