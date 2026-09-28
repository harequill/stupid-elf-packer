/*
 * stupid-elf-packer :: stub (crypt)
 *
 * Reads the metadata block, decrypts the code region in memory, then jumps to
 * the OEP. Freestanding: no libc, raw syscalls. The XOR is the shared function,
 * so decrypt is byte-for-byte the inverse of the tool's encrypt.
 *
 * _start is inline asm placed first in the blob (e_entry points here). It finds
 * `meta` (the struct the tool appends right after the code) via a RIP-relative
 * lea, calls stub_main, and jumps to the OEP it returns. The `call`/`ret` pair
 * leaves rsp exactly as the kernel set it, so the target's _start is clean.
 */
#include <stdint.h>

#include "../shared/packer.h"
#include "../shared/xor.h"

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4
#define SYS_mprotect 10
#define PAGE         0x1000UL

/* raw mprotect: number in rax, args in rdi/rsi/rdx, `syscall` instruction */
static inline long sys_mprotect(unsigned long addr, unsigned long len, long prot) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_mprotect), "D"(addr), "S"(len), "d"(prot)
                     : "rcx", "r11", "memory");
    return ret;
}

__attribute__((used)) uint64_t stub_main(struct packer_meta *m) {
    if (m->code_size) {
        /* mprotect works on whole pages, so align down and extend the length. */
        unsigned long addr = (unsigned long)m->code_addr;
        unsigned long aligned = addr & ~(PAGE - 1);
        unsigned long len = (unsigned long)m->code_size + (addr - aligned);
        len = (len + PAGE - 1) & ~(PAGE - 1);

        sys_mprotect(aligned, len, PROT_READ | PROT_WRITE | PROT_EXEC);
        xor_apply((uint8_t *)addr, m->code_size, m->key);
        sys_mprotect(aligned, len, PROT_READ | PROT_EXEC);
    }
    return m->oep;
}

__asm__(
    ".section .text.start,\"ax\",@progbits\n"
    ".global _start\n"
    "_start:\n"
    "    mov %rdx, %rbx\n"         /* save the kernel's rdx (rtld_fini) across the C call */
    "    lea meta(%rip), %rdi\n"   /* rdi = &meta (appended right after the code) */
    "    call stub_main\n"          /* rax = OEP; rbx survives (callee-saved) */
    "    mov %rbx, %rdx\n"         /* restore rdx so the target's _start sees kernel state */
    "    jmp *%rax\n"              /* hand control to the original entry point */
);
