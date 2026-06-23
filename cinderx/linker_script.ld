/* linker_script.ld
 * Linker script for when Cinder is not compiled with BOLT. This places the
 * .cinder_jit section next to the text so that we can likely do short jumps
 * to the JIT and Python runtime.
 *
 * This is optional but can give improved performance on some architectures
 * where we get better performance through improved proximity.
 */
SECTIONS {
  .cinder_jit (NOLOAD) : ALIGN(2097152) {
    PROVIDE_HIDDEN(__cinder_jit_start = .);
    . = . + 75 * 1024 * 1024;
    PROVIDE_HIDDEN(__cinder_jit_end = .);
  }
} INSERT BEFORE .text;
