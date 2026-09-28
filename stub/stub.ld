/* Lays out the crypt stub blob: _start first, then the rest of the code, with
   `meta` marking the first byte after it (where the tool appends the struct). */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : {
        *(.text.start)
        *(.text .text.*)
    }
    meta = .;
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.gcc_except_table*) }
}
