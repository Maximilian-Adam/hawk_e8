#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * Uncompressed signature format (salt || s0 || s1, int16 little-endian),
 * shared by the signer and the verifier. Integer-only: it is part of the
 * signing path (see make check-integer-only).
 */

static int
e8_param_salt_len(unsigned logn, size_t *salt_len)
{
	switch (logn) {
	case 8:
		*salt_len = 14;
		return 1;
	case 9:
		*salt_len = 24;
		return 1;
	case 10:
		*salt_len = 40;
		return 1;
	default:
		return 0;
	}
}

static int16_t
dec_i16le(const uint8_t *buf)
{
	uint32_t x = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
	return (int16_t)(x < 0x8000u ? (int32_t)x : (int32_t)x - 0x10000);
}

static void
enc_i16le(uint8_t *buf, int16_t x)
{
	uint32_t y = (uint16_t)x;
	buf[0] = (uint8_t)y;
	buf[1] = (uint8_t)(y >> 8);
}

/* see hawk_e8_inner.h */
size_t
e8_salt_len(unsigned logn)
{
	size_t salt_len;

	if (!e8_param_salt_len(logn, &salt_len)) {
		return 0;
	}
	return salt_len;
}

/* see hawk_e8_inner.h */
size_t
e8_sig_uncompressed_size(unsigned logn)
{
	size_t salt_len;

	if (!e8_param_salt_len(logn, &salt_len)) {
		return 0;
	}
	return salt_len + ((size_t)4 << logn);
}

/* see hawk_e8_inner.h */
int
e8_encode_sig_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const uint8_t *salt,
	const int16_t *s0, const int16_t *s1)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t *buf = sig;

	if (salt_len == 0 || sig_len != e8_sig_uncompressed_size(logn)) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	memcpy(buf, salt, salt_len);
	buf += salt_len;
	for (size_t u = 0; u < n; u ++, buf += 2) {
		enc_i16le(buf, s0[u]);
	}
	for (size_t u = 0; u < n; u ++, buf += 2) {
		enc_i16le(buf, s1[u]);
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_decode_sig_uncompressed(unsigned logn,
	uint8_t *salt, int16_t *s0, int16_t *s1,
	const void *sig, size_t sig_len)
{
	size_t salt_len = e8_salt_len(logn);
	const uint8_t *buf = sig;

	if (salt_len == 0 || sig_len != e8_sig_uncompressed_size(logn)) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	if (salt != NULL) {
		memcpy(salt, buf, salt_len);
	}
	buf += salt_len;
	if (s0 != NULL) {
		for (size_t u = 0; u < n; u ++, buf += 2) {
			s0[u] = dec_i16le(buf);
		}
	} else {
		buf += n << 1;
	}
	if (s1 != NULL) {
		for (size_t u = 0; u < n; u ++, buf += 2) {
			s1[u] = dec_i16le(buf);
		}
	}
	return 1;
}

#endif
