/* Signal-context registers by name, for the fault handlers (loader.c, gfx_bridge.c, nvn_dirty.c): Darwin's
   uc_mcontext->__ss on Apple, Linux's uc_mcontext on Android (Thor port). UC_ESR is the exception syndrome; bit 6 (WnR)
   tells a write fault from a read. */
#pragma once
#include <signal.h>
#include <stdint.h>
#include <sys/ucontext.h>
#ifdef __APPLE__
#define UC_PC(u) ((u)->uc_mcontext->__ss.__pc)
#define UC_SP(u) ((u)->uc_mcontext->__ss.__sp)
#define UC_FP(u) ((u)->uc_mcontext->__ss.__fp)
#define UC_LR(u) ((u)->uc_mcontext->__ss.__lr)
#define UC_X(u, i) ((u)->uc_mcontext->__ss.__x[i])
#define UC_ESR(u) ((uint64_t)(u)->uc_mcontext->__es.__esr)
#else
#include <asm/sigcontext.h>
#define UC_PC(u) ((u)->uc_mcontext.pc)
#define UC_SP(u) ((u)->uc_mcontext.sp)
#define UC_FP(u) ((u)->uc_mcontext.regs[29])
#define UC_LR(u) ((u)->uc_mcontext.regs[30])
#define UC_X(u, i) ((u)->uc_mcontext.regs[i])
/* The kernel appends records to the signal frame; the ESR record carries the syndrome of a data abort. */
static inline uint64_t uc_esr(const ucontext_t *u) {
    const uint8_t *p = (const uint8_t *)u->uc_mcontext.__reserved, *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const void *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == ESR_MAGIC) return ((const struct esr_context *)p)->esr;
        p += h->size;
    }
    return 0;
}
#define UC_ESR(u) uc_esr(u)
#endif
