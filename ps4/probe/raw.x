/* A raw payload: the image as it runs, from address 0, for a loader that
   copies the file and jumps to its first byte (GoldHEN's BinLoader on
   port 9020). _start comes first; .bss is in the file, zeroed, as the
   loader allocates only what it is sent. No dynamic section, nothing to
   relocate: the code is position-independent and reaches its data
   relative to itself. */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) *(.got .got.*) }
    .bss : ALIGN(16) { *(.bss .bss.*) *(COMMON) . = ALIGN(16); }
    /* Relocations are kept, for the build to find and refuse: an absolute
       address in data is only right at the address it was linked for. */
    .rela.dyn : { *(.rela*) }
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.dynamic) *(.dynsym) *(.dynstr)
                  *(.hash) *(.gnu.hash) *(.interp) }
}
