#pragma once

// libdatachannel negotiates SRTP protection profiles during the DTLS
// handshake. Mbed TLS keeps RFC 5764 support disabled in its default profile.
#define MBEDTLS_SSL_DTLS_SRTP
