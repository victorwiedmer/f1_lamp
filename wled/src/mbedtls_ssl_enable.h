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

/* ---- Crypto modules compiled out by SDK, re-enabled + compiled from source ---- */

/* AES block cipher (needed by GCM, CBC cipher suites) */
#undef  MBEDTLS_AES_C
#define MBEDTLS_AES_C

/* GCM authenticated-encryption mode (AEAD) */
#undef  MBEDTLS_GCM_C
#define MBEDTLS_GCM_C

/* Elliptic-Curve Diffie-Hellman (key exchange) */
#undef  MBEDTLS_ECDH_C
#define MBEDTLS_ECDH_C

/* Elliptic-curve point arithmetic (required by ECDH) */
#undef  MBEDTLS_ECP_C
#define MBEDTLS_ECP_C

/* SECP256R1 is the most common curve for TLS 1.2 */
#undef  MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* Also enable secp384r1 as a fallback */
#undef  MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* NIST curve optimisations */
#undef  MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_NIST_OPTIM

/* ECDSA signatures (needed for ECDHE-ECDSA cipher suites, e.g. Cloudflare/GitHub) */
#undef  MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_C

/* ASN1 write (ECDSA signature encoding – already in SDK lib, just ensure macro) */
#undef  MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_ASN1_WRITE_C

/* ---- Key-exchange algorithms ---- */

/* ECDHE-RSA: server cert=RSA, ephemeral ECDH key-exchange */
#undef  MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* ECDHE-ECDSA: server cert=ECDSA, ephemeral ECDH (Cloudflare, GitHub, etc.) */
#undef  MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* Plain RSA key exchange as fallback */
#undef  MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* Cipher-suite table: let mbedtls negotiate from all compiled suites.
   Do NOT define MBEDTLS_SSL_CIPHERSUITES – use the full default list. */

/* Content length (use whatever the SDK configured; do not redefine) */
#undef  MBEDTLS_SSL_MAX_CONTENT_LEN
#define MBEDTLS_SSL_MAX_CONTENT_LEN  16384
