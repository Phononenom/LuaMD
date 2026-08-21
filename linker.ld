OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* The payload executes from a PS5 JIT shared-memory alias mapped
 * PROT_READ|PROT_EXECUTE. That mapping is not writable, so unlike a normal
 * flat binary the image cannot simply fold .data and .bss in with .text --
 * the first write to any global would fault.
 *
 * Instead the writable data gets its own region, placed immediately after the
 * code in virtual address space. _start maps anonymous RW memory over that
 * range before touching a single global, which keeps every reference from the
 * code RIP-relative and in range. The initial contents of .data ride along
 * inside the read-only code image at __data_load and are copied across during
 * that same bootstrap; .bss is NOLOAD and never occupies a byte of the blob.
 */

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R-X: the JIT mapping                        */
    data PT_LOAD FLAGS(6);   /* RW-: mapped by _start, not present in file  */
}

SECTIONS
{
    . = 0;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(16);
    } :text

    /* Load address of .data: straight after the code, so `objcopy -O binary`
       emits one contiguous blob of code + initial data and nothing else. */
    __data_load = .;

    /* Virtual address of the RW region, page aligned so it can be mmap'd. */
    . = ALIGN(0x4000);

    .data : AT(__data_load) {
        __data_start = .;
        *(.data)
        *(.data.*)

        /* Even -static-pie leaves a couple of real GOT slots that it could not
           relax into `lea`, and each holds a link-time address that the
           discarded .rela.dyn was supposed to fix up. _start adds the load base
           to every non-zero entry between these two symbols; without that, the
           first call through one jumps into unmapped memory. */
        . = ALIGN(8);          /* loose bytes in .data would misalign the walk */
        __got_start = .;
        *(.got)
        *(.got.plt)
        . = ALIGN(8);
        __got_end = .;

        . = ALIGN(16);
        __data_end = .;
    } :data

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /* Total span the Lua launcher must reserve: code + data + bss. */
    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.rela.*)
        *(.rel.*)
        *(.plt)
        *(.plt.*)
    }
}
