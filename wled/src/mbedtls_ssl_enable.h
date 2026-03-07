/*
 * mbedtls_ssl_enable.h  –  Re-enable SSL/TLS features for the F1 Lamp project.
 *
 * The Arduino ESP32-C3 framework ships with CONFIG_MBEDTLS_TLS_DISABLED=1,
 * which causes esp_config.h to #undef all SSL macros.  However the pre-compiled
 * libesp-tls.a still references mbedtls_ssl_* functions.  We compile the
 * mbedtls SSL source files ourselves and need the feature macros active.
 *
 * This file is included via  MBEDTLS_USER_CONFIG_FILE.  esp_config.h includes
 * it TWICE: once from the inner standard config.h (before the #undefs) and
 * once at the very end (after the #undefs).  The second inclusion is the one
 * that matters.  We intentionally have NO include guard so both inclusions
 * execute and the final defines survive.
 */

/* Core SSL/TLS module */
#undef  MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_TLS_C
#undef  MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_CLI_C

/* Protocol version – TLS 1.2 only */
#undef  MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_2

/* Mandatory extensions for modern servers */
#undef  MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#undef  MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#undef  MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#undef  MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_ALPN
#undef  MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_SESSION_TICKETS
#undef  MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* Cipher-suite table */
#undef  MBEDTLS_SSL_CIPHERSUITES
#define MBEDTLS_SSL_CIPHERSUITES   MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256

/* Content length (use whatever the SDK configured; do not redefine) */
#undef  MBEDTLS_SSL_MAX_CONTENT_LEN
#define MBEDTLS_SSL_MAX_CONTENT_LEN  16384
