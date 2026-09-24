/*
 * stupid-elf-packer :: stub (identity)
 *
 * The dumbest possible stub: it receives control (the tool repoints the ELF
 * entry point here) and immediately jumps to the target's original entry
 * point (OEP). It encrypts nothing. Its only job is to prove that diverting
 * control and handing it back runs the target cleanly.
 *
 * It touches neither the stack nor any register the target's _start relies on,
 * so _start sees the pristine argc/argv/envp/auxv the kernel set up, exactly as
 * if it had received control directly. Using an absolute jump target also makes
 * these bytes independent of where the stub is loaded.
 *
 * The OEP is hardcoded for now; the tool will later patch it from e_entry.
 */
.text
.global _start
_start:
    movabs $0x402e40, %rax   /* OEP of target/target */
    jmp    *%rax
