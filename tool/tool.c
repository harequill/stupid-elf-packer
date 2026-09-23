/*
 * stupid-elf-packer :: tool
 */
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Human-readable name for a program header type (matches `readelf -l`). */
static const char *pt_name(uint32_t type) {
    switch (type) {
        case PT_NULL:         return "NULL";
        case PT_LOAD:         return "LOAD";
        case PT_DYNAMIC:      return "DYNAMIC";
        case PT_INTERP:       return "INTERP";
        case PT_NOTE:         return "NOTE";
        case PT_PHDR:         return "PHDR";
        case PT_TLS:          return "TLS";
        case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
        case PT_GNU_STACK:    return "GNU_STACK";
        case PT_GNU_RELRO:    return "GNU_RELRO";
        default:              return "OTHER";
    }
}

/* Render p_flags as the R/W/E triplet, like readelf does. */
static void flags_str(uint32_t f, char out[4]) {
    out[0] = (f & PF_R) ? 'R' : ' ';
    out[1] = (f & PF_W) ? 'W' : ' ';
    out[2] = (f & PF_X) ? 'E' : ' ';
    out[3] = '\0';
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "how to use: %s <elf-target>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }

    /* 
     * It reads the ELF header. Starts at offset 0 and loads the magic number in the first byts of e_ident.
     */
    Elf64_Ehdr ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) {
        fprintf(stderr, "error: '%s' is too small to have an ELF header\n", path);
        fclose(f);
        return 1;
    }

    /* Step 1: Search for the magic number (0x7F 'E' 'L' 'F'). 
     * Without it, we can't be sure if the file makes sense as an ELF. */
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "error: '%s' not an ELF (invalid magic number)\n", path);
        fclose(f);
        return 1;
    }

    printf("ok: '%s' is a valid ELF\n", path);

    fclose(f);

    /* Map the whole file read-only and treat it as a byte array; pointers are
     * cast straight onto the ELF structs. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        close(fd);
        return 1;
    }
    size_t size = (size_t)st.st_size;

    uint8_t *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping stays valid after closing the fd */
    if (base == MAP_FAILED) {
        fprintf(stderr, "error: cannot mmap '%s'\n", path);
        return 1;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;

    /* v1 only supports 64-bit non-PIE executables. PIE (ET_DYN) would need
     * runtime relocation the stub does not do, so fail early and honestly. */
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "error: not a 64-bit ELF (v1 only supports ELFCLASS64)\n");
        munmap(base, size);
        return 1;
    }
    if (eh->e_type == ET_DYN) {
        fprintf(stderr, "error: '%s' is PIE (ET_DYN); v1 only supports non-PIE ET_EXEC\n", path);
        munmap(base, size);
        return 1;
    }
    if (eh->e_type != ET_EXEC) {
        fprintf(stderr, "error: '%s' is not an executable (e_type != ET_EXEC)\n", path);
        munmap(base, size);
        return 1;
    }

    /* The program header table location comes from the header. Stride by
     * e_phentsize rather than sizeof(Elf64_Phdr) to trust what it declares. */
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        fprintf(stderr, "error: '%s' has no program headers\n", path);
        munmap(base, size);
        return 1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        fprintf(stderr, "error: program header table is out of the file bounds\n");
        munmap(base, size);
        return 1;
    }

    /* Print every program header (to match `readelf -l`) and remember the
     * executable code segment (PT_LOAD + PF_X, the future encryption target)
     * and the PT_NOTE to later cannibalize into the stub's PT_LOAD. */
    int code_idx = -1;
    int note_idx = -1;

    printf("\nprogram headers (%u entries):\n", eh->e_phnum);
    printf("idx type         offset             vaddr              filesz             memsz              flg\n");

    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        char flg[4];
        flags_str(ph->p_flags, flg);

        printf("%3u %-12s 0x%016" PRIx64 " 0x%016" PRIx64 " 0x%016" PRIx64 " 0x%016" PRIx64 " %s\n",
               i, pt_name(ph->p_type), ph->p_offset, ph->p_vaddr,
               ph->p_filesz, ph->p_memsz, flg);

        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && code_idx < 0)
            code_idx = (int)i;
        if (ph->p_type == PT_NOTE && note_idx < 0)
            note_idx = (int)i;
    }

    /* Summary: OEP, the code segment, and whether a PT_NOTE exists. */
    printf("\n=== summary ===\n");
    printf("OEP (e_entry):   0x%" PRIx64 "\n", eh->e_entry);

    if (code_idx >= 0) {
        const Elf64_Phdr *code =
            (const Elf64_Phdr *)(base + eh->e_phoff + (uint64_t)code_idx * eh->e_phentsize);
        printf("code segment:    index %d, offset 0x%" PRIx64 ", size 0x%" PRIx64 " (filesz)\n",
               code_idx, code->p_offset, code->p_filesz);
    } else {
        printf("code segment:    NOT FOUND (no PT_LOAD with PF_X)\n");
    }

    if (note_idx >= 0)
        printf("PT_NOTE:         found at index %d\n", note_idx);
    else
        printf("PT_NOTE:         not found\n");

    munmap(base, size);
    return 0;
}
