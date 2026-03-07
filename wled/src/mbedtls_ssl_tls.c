/*
 * Wrapper to compile mbedtls ssl_tls.c from the workspace mbedtls source.
 * The core mbedtls SSL functions (mbedtls_ssl_init, mbedtls_ssl_setup, etc.)
 * are missing from the pre-built Arduino ESP32 framework SDK libraries.
 * We compile them here so esp_tls can link against them.
 */
#include "ssl_tls.c"
