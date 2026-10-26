/* Assembly portability for the gates (gate.S, hle_stack.S, thread_enter.S): Mach-O on Apple, ELF on Android (Thor port).
   SYM: C symbol names (Mach-O prefixes '_'); PAGE / PAGEOFF: adrp page and low-12-bit offset of a symbol; TP_REG: the
   thread pointer the thread slots hang off (loader.c runtime_slot_offset); CSTRING_SECTION: C string literals. */
#pragma once
#ifdef __APPLE__
#define SYM(x) _##x
#define PAGE(x) x@PAGE
#define PAGEOFF(x) x@PAGEOFF
#define TP_REG tpidrro_el0
#define CSTRING_SECTION .cstring
#else
#define SYM(x) x
#define PAGE(x) x
#define PAGEOFF(x) :lo12:x
#define TP_REG tpidr_el0
#define CSTRING_SECTION .section .rodata.str1.1, "aMS", @progbits, 1
#endif
