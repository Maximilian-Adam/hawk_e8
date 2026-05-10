#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>

#define E8_MAXN   1024

typedef __int128 e8_i128;

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

static int
e8_param_verify_bound(unsigned logn, int64_t *bound)
{
	switch (logn) {
	case 8:
		/* floor(8*n*1.06^2) */
		*bound = 2301;
		return 1;
	case 9:
		/* floor(8*n*1.42^2) */
		*bound = 8259;
		return 1;
	case 10:
		/* floor(8*n*1.57^2) */
		*bound = 20192;
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

static void
poly_mul_add_i128(e8_i128 *d,
	const int32_t *a, const int32_t *b, size_t n)
{
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if (a[u] == 0) {
				continue;
			}
			size_t w = u + v;
			e8_i128 x = (e8_i128)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += x;
		}
	}
}

static e8_i128
inner_i32_i128(const int32_t *a, const e8_i128 *b, size_t n)
{
	e8_i128 s = 0;
	for (size_t u = 0; u < n; u ++) {
		s += (e8_i128)a[u] * b[u];
	}
	return s;
}

static unsigned
get_bit(const uint8_t *buf, size_t u)
{
	return (buf[u >> 3] >> (u & 7)) & 1u;
}

static void
hash_to_h(unsigned logn, uint8_t *h,
	const shake_context *sc_data, const void *hpub,
	const uint8_t *salt, size_t salt_len)
{
	uint8_t hm[64];
	size_t hpub_len = (size_t)1 << (logn - 4);
	shake_context scd;

	scd = *sc_data;
	shake_inject(&scd, hpub, hpub_len);
	shake_flip(&scd);
	shake_extract(&scd, hm, sizeof hm);

	shake_init(&scd, 256);
	shake_inject(&scd, hm, sizeof hm);
	shake_inject(&scd, salt, salt_len);
	shake_flip(&scd);
	shake_extract(&scd, h, (size_t)1 << (logn - 2));
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

/* see hawk_e8_inner.h */
int
e8_qnorm_direct(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const int32_t *w0, const int32_t *w1, unsigned logn)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}

	size_t n = (size_t)1 << logn;
	e8_i128 qw0[E8_MAXN], qw1[E8_MAXN];
	memset(qw0, 0, n * sizeof *qw0);
	memset(qw1, 0, n * sizeof *qw1);
	poly_mul_add_i128(qw0, q00, w0, n);
	poly_mul_add_i128(qw0, q01, w1, n);
	poly_mul_add_i128(qw1, q10, w0, n);
	poly_mul_add_i128(qw1, q11, w1, n);

	e8_i128 acc = inner_i32_i128(w0, qw0, n)
		+ inner_i32_i128(w1, qw1, n);
	if (acc < 0 || acc > (e8_i128)INT64_MAX) {
		return 0;
	}
	*norm = (int64_t)acc;
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_verify_uncompressed(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11)
{
	size_t salt_len;
	int64_t bound, norm;
	uint8_t salt[40], h[256];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int32_t w0[E8_MAXN], w1[E8_MAXN];

	if (!e8_param_salt_len(logn, &salt_len)
		|| !e8_param_verify_bound(logn, &bound)
		|| hpub_len != ((size_t)1 << (logn - 4)))
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	if (!e8_decode_sig_uncompressed(logn,
		salt, s0, s1, sig, sig_len))
	{
		return 0;
	}

	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	for (size_t u = 0; u < n; u ++) {
		w0[u] = (int32_t)get_bit(h, u) - 2 * (int32_t)s0[u];
		w1[u] = (int32_t)get_bit(h + (n >> 3), u)
			- 2 * (int32_t)s1[u];
	}

	if (!e8_qnorm_direct(&norm, q00, q01, q10, q11, w0, w1, logn)) {
		return 0;
	}
	return norm <= bound;
}

#endif
