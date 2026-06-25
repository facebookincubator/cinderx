/* linker_script_bolt.ld
 * Reserves a small .cinder_jit NOLOAD region immediately AFTER .bss.
 * This is used when we are in a BOLT build and will ensure that the
 * JIT code can live next to where BOLT places the .text.
 */
SECTIONS {
  .cinder_jit (NOLOAD) : ALIGN(2097152) {
    PROVIDE_HIDDEN(__cinder_jit_start = .);
    . = . + 85 * 1024 * 1024;
    PROVIDE_HIDDEN(__cinder_jit_end = .);
  }
} INSERT AFTER .bss;
