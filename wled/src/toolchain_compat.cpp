/*
 * toolchain_compat.cpp  –  C++ link/runtime shims for the ESP32-C3 toolchain gap.
 *
 * See toolchain_compat.c for the background: the PlatformIO espressif32 SDK
 * libs were built with GCC 8.4, which we link with the ARM64 ESP-IDF 5.x
 * toolchain (GCC 14.2.0).  GCC 14's libstdc++ targets that assume the 'A'
 * (atomic) extension, which the ESP32-C3 (rv32imc) does not have.  When the
 * wrong libstdc++ variant is pulled in, `__atomic_add` / `__exchange_and_add`
 * are compiled as `amoadd.w` and fault with "Illegal instruction" at boot.
 *
 * We override those two exported libsupc++ symbols with single-core-safe
 * software implementations.  A RISC-V critical section (mask mstatus.MIE) is
 * the correct primitive on the single-core C3: these run on the FreeRTOS task
 * that owns the refcount/allocator state, and interrupt masking serialises
 * them against preemption.
 */
#include <stdint.h>

static inline uint32_t irq_save_and_disable(void)
{
    uint32_t mstatus;
    /* read mstatus, then atomically clear bit 3 (MIE) */
    asm volatile("csrrci %0, mstatus, 8" : "=r"(mstatus) : : "memory");
    return mstatus;
}

static inline void irq_restore(uint32_t mstatus)
{
    asm volatile("csrw mstatus, %0" : : "r"(mstatus) : "memory");
}

namespace __gnu_cxx {

/* void __atomic_add(int volatile *mem, int val)   ->  *mem += val */
void __atomic_add(int volatile *mem, int val)
{
    uint32_t s = irq_save_and_disable();
    *mem += val;
    irq_restore(s);
}

/* int __exchange_and_add(int volatile *mem, int val)  ->  old = *mem; *mem += val; return old */
int __exchange_and_add(int volatile *mem, int val)
{
    uint32_t s = irq_save_and_disable();
    int old = *mem;
    *mem = old + val;
    irq_restore(s);
    return old;
}

} /* namespace __gnu_cxx */