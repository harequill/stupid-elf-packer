/*
 * stupid-elf-packer :: stub (dynamic)
 *
 * Same job as the identity stub, but instead of a hardcoded OEP it reads the
 * OEP at runtime from a metadata block (struct packer_meta) that the tool
 * appends right after this code.
 *
 * How it finds that block without knowing where it was loaded: `meta` is the
 * very first byte after the code, so `lea meta(%rip), %rax` computes its
 * runtime address relative to the instruction pointer. The tool appends the
 * struct exactly at that spot, so the label and the data line up.
 *
 * It touches neither the stack nor the registers the target's _start relies on,
 * so _start sees the pristine argc/argv/envp/auxv the kernel set up.
 */
.text
.global _start
_start:
    lea    meta(%rip), %rax   /* rax = address of the metadata block */
    mov    (%rax), %rax        /* rax = meta.oep (offset 0 in the struct) */
    jmp    *%rax               /* hand control to the original entry point */
meta:
    /* struct packer_meta is appended here by the tool */
