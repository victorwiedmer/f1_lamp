/*
 * toolchain_compat.c  –  Link-time shims for the ESP32-C3 SDK toolchain gap.
 *
 * The PlatformIO espressif32 platform ships precompiled ESP-IDF libraries
 * (libesp_system.a, libnewlib.a, ...) that were built with the ESP-IDF 4.4
 * toolchain (GCC 8.4.0 + classic newlib).  That toolchain only has an
 * x86_64 macOS build, so on Apple Silicon we link against the ESP-IDF
 * 5.x ARM64 toolchain (GCC 14.2.0, picolibc).  Two libgcc/newlib symbols
 * referenced by the SDK were removed in the newer toolchain; we provide
 * them here as safe no-ops so the precompiled SDK libs can link.
 */

/* GCC 8.x libgcc exported this to let the runtime opt out of sorting
 * unwind FDEs by address.  GCC 13+ removed it.  A no-op is harmless for
 * a statically-linked embedded image (no dynamic loading). */
void _Unwind_SetEnableExceptionFdeSorting(int enable)
{
    (void)enable;
}

/* Classic newlib exported _cleanup_r() (flush stdio at exit).  picolibc
 * dropped it.  The ESP-IDF reent init calls it once at startup; firmware
 * never runs an atexit stdio flush, so a no-op is safe. */
struct _reent;
void _cleanup_r(struct _reent *r)
{
    (void)r;
}
