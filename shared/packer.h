#ifndef STUPID_ELF_PACKER_SHARED_H
#define STUPID_ELF_PACKER_SHARED_H

#include <stdint.h>

/* Metadata block the tool appends after the stub; shared layout, oep at 0. */
struct packer_meta {
    uint64_t oep;
};

#endif
