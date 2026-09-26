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

#include "../shared/packer.h"

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

/*
 * Produce a packed copy of the input: append the stub followed by a
 * packer_meta block (carrying the OEP), turn the PT_NOTE at note_idx into a
 * PT_LOAD that maps both, and point e_entry at the stub.
 */
static int pack_elf(const char *out_path,
                    const uint8_t *in, size_t in_size,
                    const uint8_t *stub, size_t stub_size,
                    unsigned note_idx) {
    const uint64_t page = 0x1000;
    const Elf64_Ehdr *ieh = (const Elf64_Ehdr *)in;

    /* The stub lands right after the original bytes. */
    uint64_t stub_off = in_size;

    /* Pick a page-aligned virtual address above every existing segment, then
     * add stub_off % page so that p_vaddr and p_offset are congruent modulo the
     * page size; the kernel refuses the mapping otherwise. */
    uint64_t max_end = 0;
    for (unsigned i = 0; i < ieh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(in + ieh->e_phoff + (uint64_t)i * ieh->e_phentsize);
        if (ph->p_type == PT_LOAD) {
            uint64_t end = ph->p_vaddr + ph->p_memsz;
            if (end > max_end) max_end = end;
        }
    }
    uint64_t vaddr_base = ((max_end + page - 1) & ~(page - 1)) + page; /* aligned + gap */
    uint64_t stub_vaddr = vaddr_base + (stub_off % page);

    size_t blob_size = stub_size + sizeof(struct packer_meta);
    size_t out_size = in_size + blob_size;

    int fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        fprintf(stderr, "error: cannot create '%s'\n", out_path);
        return 1;
    }
    if (ftruncate(fd, (off_t)out_size) != 0) {
        fprintf(stderr, "error: cannot size '%s'\n", out_path);
        close(fd);
        return 1;
    }

    uint8_t *out = mmap(NULL, out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (out == MAP_FAILED) {
        fprintf(stderr, "error: cannot mmap '%s'\n", out_path);
        return 1;
    }

    /* Copy the original file, then append the stub and the metadata block. */
    memcpy(out, in, in_size);
    memcpy(out + stub_off, stub, stub_size);
    struct packer_meta meta = { .oep = ieh->e_entry };
    memcpy(out + stub_off + stub_size, &meta, sizeof(meta));

    /* Cannibalize the PT_NOTE into a PT_LOAD that maps the stub. */
    Elf64_Phdr *ph =
        (Elf64_Phdr *)(out + ieh->e_phoff + (uint64_t)note_idx * ieh->e_phentsize);
    ph->p_type   = PT_LOAD;
    ph->p_flags  = PF_R | PF_X;
    ph->p_offset = stub_off;
    ph->p_vaddr  = stub_vaddr;
    ph->p_paddr  = stub_vaddr;
    ph->p_filesz = blob_size;
    ph->p_memsz  = blob_size;
    ph->p_align  = page;

    /* Redirect the entry point to the stub. */
    ((Elf64_Ehdr *)out)->e_entry = stub_vaddr;

    msync(out, out_size, MS_SYNC);
    munmap(out, out_size);

    printf("\n=== packed ===\n");
    printf("output:          %s\n", out_path);
    printf("blob:            %zu bytes (stub %zu + meta %zu) at offset 0x%" PRIx64 "\n",
           blob_size, stub_size, sizeof(struct packer_meta), stub_off);
    printf("stub vaddr:      0x%" PRIx64 "\n", stub_vaddr);
    printf("meta.oep:        0x%" PRIx64 "\n", meta.oep);
    printf("new e_entry:     0x%" PRIx64 " (was 0x%" PRIx64 ")\n", stub_vaddr, ieh->e_entry);
    printf("cannibalized ph: index %u (PT_NOTE -> PT_LOAD)\n", note_idx);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "how to use: %s <elf-target>                  (inspect)\n", argv[0]);
        fprintf(stderr, "            %s <elf-target> <stub.bin> <out>  (pack)\n", argv[0]);
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

    /* Inspect-only mode stops here. */
    if (argc != 4) {
        munmap(base, size);
        return 0;
    }

    /* Pack mode: we need a PT_NOTE to cannibalize and a stub blob to inject. */
    if (note_idx < 0) {
        fprintf(stderr, "error: no PT_NOTE to cannibalize\n");
        munmap(base, size);
        return 1;
    }

    FILE *sf = fopen(argv[2], "rb");
    if (!sf) {
        fprintf(stderr, "error: cannot open stub '%s'\n", argv[2]);
        munmap(base, size);
        return 1;
    }
    fseek(sf, 0, SEEK_END);
    long ssize = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    if (ssize <= 0) {
        fprintf(stderr, "error: stub '%s' is empty\n", argv[2]);
        fclose(sf);
        munmap(base, size);
        return 1;
    }
    uint8_t *stub = malloc((size_t)ssize);
    if (!stub || fread(stub, 1, (size_t)ssize, sf) != (size_t)ssize) {
        fprintf(stderr, "error: cannot read stub '%s'\n", argv[2]);
        free(stub);
        fclose(sf);
        munmap(base, size);
        return 1;
    }
    fclose(sf);

    int rc = pack_elf(argv[3], base, size, stub, (size_t)ssize, (unsigned)note_idx);

    free(stub);
    munmap(base, size);
    return rc;
}
