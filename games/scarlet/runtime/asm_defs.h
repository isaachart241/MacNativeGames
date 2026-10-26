/* Assembly portability for the gates (gate.S, hle_stack.S, thread_enter.S): Mach-O on Apple, ELF on Android (Thor port).
   SYM: C symbol names (Mach-O prefixes '_'); PAGE / PAGEOFF: adrp page and low-12-bit offset of a symbol; LOAD_TP(r): the
   base the thread slots hang off (loader.c runtime_slot_offset) into r; CSTRING_SECTION: C string literals. */
#pragma once
#ifdef __APPLE__
#define SYM(x) _##x
#define PAGE(x) x@PAGE
#define PAGEOFF(x) x@PAGEOFF
#define LOAD_TP(r) mrs r, tpidrro_el0
#define CSTRING_SECTION .cstring
#else
#define SYM(x) x
#define PAGE(x) x
#define PAGEOFF(x) :lo12:x
/* Thor port: Bionic's thread pointer; the guest's thread slots are in a per-thread block whose address Bionic's
   TLS_SLOT_APP (slot 2, offered to apps since Android 10) holds, which works in an app's process as well. */
#define LOAD_TP(r) mrs r, tpidr_el0 ; ldr r, [r, #16]
#define CSTRING_SECTION .section .rodata.str1.1, "aMS", @progbits, 1
#endif
