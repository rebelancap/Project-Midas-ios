/**
 * ASM_SYM("name") — spell a C-visible symbol name inside a top-level
 * __asm__() string so it links against C code on EVERY object format.
 *
 * i386 PE/COFF (MinGW) prepends `_` to C symbols; ELF does not. Inline-asm
 * strings bypass that decoration, so an asm-defined `foo` is invisible to C
 * callers of foo() on Windows (they look for `_foo`). GCC/Clang expose the
 * active decoration as __USER_LABEL_PREFIX__ (`_` on i386 PE, empty on ELF);
 * stringifying it and gluing it to the literal gives the right name on both
 * routes with no #ifdef per site.
 *
 * Usage (string-literal concatenation, names only — never sections/sizes):
 *   "    .globl " ASM_SYM("unknown2") "\n"
 *   ASM_SYM("unknown2") ":\n"
 *   "    .long " ASM_SYM("portPcmBank") "\n"
 */
#ifndef PORT_ASMSYM_H
#define PORT_ASMSYM_H

#define PORT_ASMSYM_STR_(x) #x
#define PORT_ASMSYM_STR(x) PORT_ASMSYM_STR_(x)

#ifdef __USER_LABEL_PREFIX__
#define ASM_SYM(name) PORT_ASMSYM_STR(__USER_LABEL_PREFIX__) name
#else
#define ASM_SYM(name) name
#endif

/* Section switches for those asm blocks. Mach-O (Apple) names sections
 * segment,section; `.data`/`.bss` are its shorthand directives. */
#ifdef __APPLE__
#define ASM_SECTION_DATA "    .data\n"
#define ASM_SECTION_BSS "    .bss\n"
#else
#define ASM_SECTION_DATA "    .section .data\n"
#define ASM_SECTION_BSS "    .section .bss\n"
#endif

#endif /* PORT_ASMSYM_H */
