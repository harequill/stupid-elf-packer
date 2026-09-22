/*
 * stupid-elf-packer :: tool
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return 0;
}
