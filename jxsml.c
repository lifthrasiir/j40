// JXSML: An extra small JPEG XL decoder
// Kang Seonghoon, 2022-06, Public Domain (CC0)

#ifndef JXSML__RECURSING

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include <stdio.h>

#define JXSML_DEBUG

enum {
	JXSML_CHROMA_WHITE = 0, JXSML_CHROMA_RED = 1,
	JXSML_CHROMA_GREEN = 2, JXSML_CHROMA_BLUE = 3,
};

typedef struct {
	int32_t width, height;
	enum {
		JXSML_ORIENT_TL = 1, JXSML_ORIENT_TR = 2, JXSML_ORIENT_BR = 3, JXSML_ORIENT_BL = 4,
		JXSML_ORIENT_LT = 5, JXSML_ORIENT_RT = 6, JXSML_ORIENT_RB = 7, JXSML_ORIENT_LB = 8,
	} orientation;
	int32_t intr_width, intr_height; // 0 if not specified
	int bpp, exp_bits;

	int32_t anim_tps_num, anim_tps_denom; // num=denom=0 if not animated
	int32_t anim_nloops; // 0 if infinity
	int anim_have_timecodes;

	char *icc;
	size_t iccsize;
	enum { JXSML_CS_CHROMA = 'c', JXSML_CS_GREY = 'g', JXSML_CS_XYB = 'x' } cspace;
	float cpoints[4 /*JXSML_CHROMA_xxx*/][2 /*x=0, y=1*/]; // only for JXSML_CS_CHROMA
	enum {
		JXSML_TF_709 = -1, JXSML_TF_UNKNOWN = -2, JXSML_TF_LINEAR = -8, JXSML_TF_SRGB = -13,
		JXSML_TF_PQ = -16, JXSML_TF_DCI = -17, JXSML_TF_HLG = -18,
		JXSML_GAMMA_MAX = 10000000,
	} gamma_or_tf; // gamma if > 0, transfer function if <= 0
	enum { JXSML_INTENT_PERC = 0, JXSML_INTENT_REL = 1, JXSML_INTENT_SAT = 2, JXSML_INTENT_ABS = 3 } render_intent;
	float intensity_target, min_nits; // 0 < min_nits <= intensity_target
	float linear_below; // absolute (nits) if >= 0; a negated ratio of max display brightness if [-1,0]
} jxsml;

typedef int jxsml_err;

typedef struct {
	enum {
		JXSML_EC_ALPHA = 0, JXSML_EC_DEPTH = 1, JXSML_EC_SPOT_COLOUR = 2,
		JXSML_EC_SELECTION_MASK = 3, JXSML_EC_BLACK = 4, JXSML_EC_CFA = 5,
		JXSML_EC_THERMAL = 6, JXSML_EC_NON_OPTIONAL = 15, JXSML_EC_OPTIONAL = 16,
	} type;
	int32_t bpp, exp_bits, dim_shift, name_len;
	char *name;
	union {
		int alpha_associated;
		struct { float red, green, blue, solidity; } spot;
		int32_t cfa_channel;
	} data;
} jxsml__ec_info;

typedef struct {
	jxsml *out;
	jxsml_err err;

	const uint8_t *buf, *container_buf;
	size_t bufsize, container_bufsize;
	int container_continued;
	uint32_t bits;
	int nbits;
	size_t bits_read;

	int modular_16bit_buffers;
	int num_extra_channels;
	jxsml__ec_info *ec_info;
	int xyb_encoded;
	float opsin_inv_mat[3][3], opsin_bias[3], quant_bias[3], quant_bias_num;
	int want_icc;
} jxsml__st;

////////////////////////////////////////////////////////////////////////////////
// utilities

#define JXSML__CONCAT_(a,b) a##b
#define JXSML__CONCAT(a,b) JXSML__CONCAT_(a,b)
#define JXSML__CONCAT3(a,b,c) JXSML__CONCAT(a,JXSML__CONCAT(b,c))

#define JXSML__PARAMETRIC_NAME_(x, JXSML__V) JXSML__CONCAT3(jxsml__, x, JXSML__V)
#define jxsml__(x, V) JXSML__PARAMETRIC_NAME_(x, JXSML__CONCAT(JXSML__, V))

#ifdef __has_attribute
	#define JXSML__HAS_ATTR(a) __has_attribute(a)
#else
	#define JXSML__HAS_ATTR(a) 0
#endif

#ifndef JXSML_STATIC
	#define JXSML_STATIC static
#endif

#ifndef JXSML_INLINE
	#define JXSML_INLINE JXSML_STATIC inline
#endif

#ifndef JXSML_ALWAYS_INLINE
	#if JXSML__HAS_ATTR(always_inline) || defined(__GNUC__) || defined(__clang__)
		#define JXSML_ALWAYS_INLINE __attribute__((always_inline)) JXSML_INLINE
	#elif defined(_MSC_VER)
		#define JXSML_ALWAYS_INLINE __forceinline JXSML_INLINE
	#else
		#define JXSML_ALWAYS_INLINE JXSML_INLINE
	#endif
#endif // !defined JXSML_ALWAYS_INLINE

JXSML_ALWAYS_INLINE int32_t jxsml__to_signed(int32_t x) {
	return (int32_t) (x & 1 ? -(x + 1) / 2 : x / 2);
}

// ----------------------------------------
// recursion for bit-dependent math functions
#define JXSML__RECURSING 100
#define JXSML__N 16
#include __FILE__
#define JXSML__N 32
#include __FILE__
#define JXSML__N 64
#include __FILE__
#undef JXSML__RECURSING

#endif // !JXSML__RECURSING
#if JXSML__RECURSING+0 == 100
	#define jxsml__intN_t JXSML__CONCAT3(int, JXSML__N, _t)
	#define jxsml__uintN_t JXSML__CONCAT3(uint, JXSML__N, _t)
// ----------------------------------------

// same to `(a + b) >> 1` but doesn't overflow, useful for tight loops with autovectorization
// https://devblogs.microsoft.com/oldnewthing/20220207-00/?p=106223
JXSML_ALWAYS_INLINE jxsml__intN_t jxsml__(floor_avg,N)(jxsml__intN_t x, jxsml__intN_t y) {
	return (jxsml__intN_t) (x / 2 + y / 2 + (x & y & 1));
}

JXSML_ALWAYS_INLINE jxsml__intN_t jxsml__(abs,N)(jxsml__intN_t x) {
	return (jxsml__intN_t) (x < 0 ? -x : x);
}
JXSML_ALWAYS_INLINE jxsml__intN_t jxsml__(min,N)(jxsml__intN_t x, jxsml__intN_t y) {
	return (jxsml__intN_t) (x < y ? x : y);
}
JXSML_ALWAYS_INLINE jxsml__intN_t jxsml__(max,N)(jxsml__intN_t x, jxsml__intN_t y) {
	return (jxsml__intN_t) (x > y ? x : y);
}

#define JXSML__UINTN_MAX JXSML__CONCAT3(UINT, JXSML__N, _MAX)
#if UINT_MAX == JXSML__UINTN_MAX
	#define JXSML__CLZN __builtin_clz
#elif ULONG_MAX == JXSML__UINTN_MAX
	#define JXSML__CLZN __builtin_clzl
#elif ULLONG_MAX == JXSML__UINTN_MAX
	#define JXSML__CLZN __builtin_clzll
#endif
#undef JXSML__UINTN_MAX
#ifdef JXSML__CLZN
	// both requires x to be > 0
	JXSML_ALWAYS_INLINE int jxsml__(floor_lg,N)(jxsml__uintN_t x) {
		return JXSML__N - 1 - JXSML__CLZN(x);
	}
	JXSML_ALWAYS_INLINE int jxsml__(ceil_lg,N)(jxsml__uintN_t x) {
		return x > 2 ? JXSML__N - JXSML__CLZN(x - 1) : 0;
	}
	#undef JXSML__CLZN
#endif

// ----------------------------------------
// end of recursion
	#undef jxsml__intN_t
	#undef jxsml__uintN_t
	#undef JXSML__N
#endif // JXSML__RECURSING == 100
#ifndef JXSML__RECURSING
// ----------------------------------------

////////////////////////////////////////////////////////////////////////////////
// error handling macros

#define JXSML__4(s) (jxsml_err) (((uint32_t) s[0] << 24) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 8) | (uint32_t) s[3])
#define JXSML__ERR(s) jxsml__set_error(st, JXSML__4(s))
#define JXSML__SHOULD(cond, s) do { if (st->err) goto error; else if (!(cond)) { jxsml__set_error(st, JXSML__4(s)); goto error; } } while (0)
#define JXSML__RAISE(s) do { jxsml__set_error(st, JXSML__4(s)); goto error; } while (0)
#define JXSML__RAISE_DELAYED() do { if (st->err) goto error; } while (0)
#define JXSML__TRY(expr) do { if (expr) goto error; } while (0)
#ifdef JXSML_DEBUG
#define JXSML__ASSERT(cond) JXSML__SHOULD(cond, "!exp")
#define JXSML__UNREACHABLE() JXSML__ASSERT(0)
#else
#define JXSML__ASSERT(cond) (void) 0
#define JXSML__UNREACHABLE() (void) 0
#endif

JXSML_STATIC jxsml_err jxsml__set_error(jxsml__st *st, jxsml_err err) {
	if (!st->err) {
		st->err = err;
#ifdef JXSML_DEBUG
		if (err == JXSML__4("!exp")) abort();
#endif
	}
	return err;
}

JXSML_STATIC void *jxsml__realloc(jxsml__st *st, void *ptr, size_t itemsize, int32_t len, int32_t *cap) {
	void *newptr;
	uint32_t newcap;
	JXSML__ASSERT(len >= 0);
	if (len <= *cap) return ptr;
	newcap = (uint32_t) *cap * 2;
	if (newcap > (uint32_t) INT32_MAX) newcap = (uint32_t) INT32_MAX;
	if (newcap < (uint32_t) len) newcap = (uint32_t) len;
	JXSML__SHOULD(newcap <= SIZE_MAX / itemsize, "!mem");
	JXSML__SHOULD(newptr = realloc(ptr, itemsize * newcap), "!mem");
	*cap = (int32_t) newcap;
	return newptr;
error:
	return NULL;
}

#define JXSML__TRY_REALLOC(ptr, itemsize, len, cap) \
	do { \
		void *newptr = jxsml__realloc(st, *(ptr), itemsize, len, cap); \
		if (newptr) *(ptr) = newptr; else goto error; \
	} while (0) \

////////////////////////////////////////////////////////////////////////////////
// container

JXSML_STATIC jxsml_err jxsml__container_u32(jxsml__st *st, uint32_t *v) {
	JXSML__SHOULD(st->container_bufsize >= 4, "shrt");
	*v = ((uint32_t) st->container_buf[0] << 24) | ((uint32_t) st->container_buf[1] << 16) |
		((uint32_t) st->container_buf[2] << 8) | (uint32_t) st->container_buf[3];
	st->container_buf += 4;
	st->container_bufsize -= 4;
error:
	return st->err;
}

typedef struct { uint32_t type; const uint8_t *start; size_t size; int brotli; } jxsml__box_t;
JXSML_STATIC jxsml_err jxsml__box(jxsml__st *st, jxsml__box_t *out) {
	uint32_t size32;
	JXSML__TRY(jxsml__container_u32(st, &size32));
	JXSML__TRY(jxsml__container_u32(st, &out->type));
	if (size32 == 0) {
		out->size = st->container_bufsize;
	} else if (size32 == 1) {
		uint32_t sizehi, sizelo;
		uint64_t size64;
		JXSML__TRY(jxsml__container_u32(st, &sizehi));
		JXSML__TRY(jxsml__container_u32(st, &sizelo));
		size64 = ((uint64_t) sizehi << 32) | (uint64_t) sizelo;
		JXSML__SHOULD(size64 >= 16, "boxx");
		size64 -= 16;
		JXSML__SHOULD(st->container_bufsize >= size64, "shrt");
		out->size = (size_t) size64;
	} else {
		JXSML__SHOULD(size32 >= 8, "boxx");
		size32 -= 8;
		JXSML__SHOULD(st->container_bufsize >= size32, "shrt");
		out->size = (size_t) size32;
	}
	out->brotli = (out->type == 0x62726f62 /*brob*/);
	if (out->brotli) {
		JXSML__SHOULD(out->size > (size_t) 4, "brot"); // Brotli stream is never empty so 4 is also out
		JXSML__TRY(jxsml__container_u32(st, &out->type));
		JXSML__SHOULD(out->type != 0x62726f62 /*brob*/ && (out->type >> 8) != 0x6a786c /*jxl*/, "brot");
		out->size -= 4;
	}
	out->start = st->container_buf;
	st->container_buf += out->size;
	st->container_bufsize -= out->size;
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__init_container(jxsml__st *st, const void *buf, size_t bufsize) {
	static const uint8_t JXL_BOX[12] = { // type `JXL `, value 0D 0A 87 0A
		0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
	}, FTYP_BOX[20] = { // type `ftyp`, brand `jxl `, version 0, only compatible w/ brand `jxl `
		0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x6a, 0x78, 0x6c, 0x20,
		0x00, 0x00, 0x00, 0x00, 0x6a, 0x78, 0x6c, 0x20,
	};

	st->buf = NULL;
	st->bufsize = 0;
	if (bufsize >= sizeof(JXL_BOX) && memcmp(buf, JXL_BOX, sizeof(JXL_BOX)) == 0) {
		st->container_buf = (const uint8_t *) buf + sizeof(JXL_BOX);
		st->container_bufsize = bufsize - sizeof(JXL_BOX);
		JXSML__SHOULD(st->container_bufsize >= sizeof(FTYP_BOX), "shrt");
		JXSML__SHOULD(memcmp(st->container_buf, FTYP_BOX, sizeof(FTYP_BOX)) == 0, "ftyp");
		st->container_buf += sizeof(FTYP_BOX);
		st->container_bufsize -= sizeof(FTYP_BOX);
		while (st->container_bufsize > 0) {
			jxsml__box_t box;
			JXSML__TRY(jxsml__box(st, &box));
			if (box.type == 0x6a786c6c /*jxll*/) {
				// TODO do something with this
			} else if (box.type == 0x6a786c63 /*jxlc*/) {
				JXSML__ASSERT(!box.brotli);
				st->container_continued = 0;
				st->buf = box.start;
				st->bufsize = box.size;
				break;
			} else if (box.type == 0x6a786c70 /*jxlp*/) {
				JXSML__ASSERT(!box.brotli);
				JXSML__SHOULD(box.size >= 4, "shrt");
				// TODO the partial codestream index is ignored right now
				st->container_continued = !(box.start[0] >> 7);
				st->buf = box.start + 4;
				st->bufsize = box.size - 4;
				break;
			}
		}
		JXSML__SHOULD(st->buf, "!box");
	} else {
		st->buf = (const uint8_t *) buf;
		st->bufsize = bufsize;
	}
	fprintf(stderr, "skipped %d bytes of container\n", (int32_t) (st->buf - (const uint8_t*) buf));
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__refill_container(jxsml__st *st) {
	jxsml__box_t box;
	JXSML__ASSERT(st->container_continued);
	do {
		JXSML__TRY(jxsml__box(st, &box));
		JXSML__SHOULD(
			box.type != 0x6a786c63 /*jxlc*/ &&
			box.type != 0x6a786c69 /*jxli*/,
			"box?"); // some box is disallowed after jxlp
	} while (box.type != 0x6a786c70 /*jxlp*/);
	JXSML__ASSERT(!box.brotli);
	JXSML__SHOULD(box.size >= 4, "shrt");
	// TODO the partial codestream index is ignored right now
	st->container_continued = !(box.start[0] >> 7);
	st->buf = box.start + 4;
	st->bufsize = box.size - 4;
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__finish_container(jxsml__st *st) {
	JXSML__ASSERT(!st->container_continued);
	while (st->container_bufsize > 0) {
		jxsml__box_t box;
		JXSML__TRY(jxsml__box(st, &box));
		JXSML__SHOULD(
			box.type != 0x6a786c63 /*jxlc*/ &&
			box.type != 0x6a786c70 /*jxlp*/ &&
			box.type != 0x6a786c69 /*jxli*/,
			"box?"); // some box is disallowed after the last jxlp/jxlc
	}
error:
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// bitstream

JXSML_STATIC jxsml_err jxsml__always_refill(jxsml__st *st, int32_t n) {
#ifdef JXSML_DEBUG
	if (n < 0 || n > 31) return JXSML__ERR("!exp");
#endif
	do {
		size_t consumed = (size_t) ((32 - st->nbits) >> 3);
		if (st->bufsize < consumed) {
			while (st->bufsize > 0) {
				st->bits |= (uint32_t) *st->buf++ << st->nbits;
				st->nbits += 8;
				--st->bufsize;
			}
			if (st->nbits < n) {
				JXSML__SHOULD(st->container_continued, "shrt");
				JXSML__TRY(jxsml__refill_container(st));
				continue; // now we have possibly more bits to refill, try again
			}
		} else {
			st->bufsize -= consumed;
#ifdef JXSML_DEBUG
			if (st->nbits > 24) return JXSML__ERR("!exp");
#endif
			do {
				st->bits |= (uint32_t) *st->buf++ << st->nbits;
				st->nbits += 8;
			} while (st->nbits <= 24);
		}
	} while (0);
error:
	return st->err;
}

// ensure st->nbits is at least n; otherwise pull as many bytes as possible into st->bits
#define jxsml__refill(st, n) (st->nbits < (n) ? jxsml__always_refill(st, n) : st->err)

JXSML_INLINE jxsml_err jxsml__zero_pad_to_byte(jxsml__st *st) {
	int32_t n = st->nbits & 7;
	if (st->bits & ((1u << n) - 1)) return JXSML__ERR("pad0");
	st->bits >>= n;
	st->nbits -= n;
	st->bits_read += (size_t) n;
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__skip(jxsml__st *st, uint64_t n) {
	uint64_t bytes;
	if ((uint64_t) st->nbits >= n) {
		st->bits >>= (int32_t) n;
		st->nbits -= (int32_t) n;
	} else {
		n -= (uint64_t) st->nbits;
		st->bits = 0;
		st->nbits = 0;
	}
	bytes = (uint64_t) (n >> 3);
	// TODO honor containers
	if (st->bufsize < bytes) return JXSML__ERR("shrt");
	st->bufsize -= bytes;
	st->buf += bytes;
	n &= 7;
	if (jxsml__refill(st, (int32_t) n)) return st->err;
	st->bits >>= (int32_t) n;
	st->nbits -= (int32_t) n;
	return st->err;
}

JXSML_INLINE int32_t jxsml__u(jxsml__st *st, int32_t n) {
	int32_t ret;
#ifdef JXSML_DEBUG
	if (n < 0 || n > 31) return JXSML__ERR("!exp"), 0;
#endif
	if (jxsml__refill(st, n)) return 0;
	ret = (int32_t) (st->bits & ((1u << n) - 1));
	st->bits >>= n;
	st->nbits -= n;
	st->bits_read += (size_t) n;
	return ret;
}

// the maximum value U32() actually reads is 2^30 + 4211711, so int32_t should be enough
JXSML_INLINE int32_t jxsml__u32(
	jxsml__st *st,
	int32_t o0, int32_t n0, int32_t o1, int32_t n1,
	int32_t o2, int32_t n2, int32_t o3, int32_t n3
) {
	const int32_t o[4] = { o0, o1, o2, o3 };
	const int32_t n[4] = { n0, n1, n2, n3 };
	int32_t sel;
#ifdef JXSML_DEBUG
	if (n0 < 0 || n0 > 30 || o0 > 0x7fffffff - (1 << n0)) return JXSML__ERR("!exp"), 0;
	if (n1 < 0 || n1 > 30 || o1 > 0x7fffffff - (1 << n1)) return JXSML__ERR("!exp"), 0;
	if (n2 < 0 || n2 > 30 || o2 > 0x7fffffff - (1 << n2)) return JXSML__ERR("!exp"), 0;
	if (n3 < 0 || n3 > 30 || o3 > 0x7fffffff - (1 << n3)) return JXSML__ERR("!exp"), 0;
#endif
	sel = jxsml__u(st, 2);
	return jxsml__u(st, n[sel]) + o[sel];
}

uint64_t jxsml__u64(jxsml__st *st) {
	int32_t sel = jxsml__u(st, 2), shift;
	uint64_t ret = (uint64_t) jxsml__u(st, sel * 4);
	if (sel < 3) {
		ret += 17u >> (8 - sel * 4);
	} else {
		for (shift = 12; shift < 64 && jxsml__u(st, 1); shift += 8) {
			ret |= (uint64_t) jxsml__u(st, shift < 56 ? 8 : 64 - shift) << shift;
		}
	}
	return ret;
}

JXSML_INLINE int32_t jxsml__enum(jxsml__st *st) {
	int32_t ret = jxsml__u32(st, 0, 0, 1, 0, 2, 4, 18, 6);
	// the spec says it should be 64, but the largest enum value in use is 18 (kHLG);
	// we have to reject unknown enum values anyway so we use a smaller limit to avoid overflow
	if (ret >= 31) return JXSML__ERR("enum"), 0;
	return ret;
}

JXSML_INLINE float jxsml__f16(jxsml__st *st) {
	int32_t bits = jxsml__u(st, 16);
	int32_t biased_exp = (bits >> 10) & 0x1f;
	if (biased_exp == 31) return JXSML__ERR("!fin"), 0.0f;
	return (bits >> 15 ? -1 : 1) * ldexpf((float) ((bits & 0x3ff) | (biased_exp > 0 ? 0x400 : 0)), biased_exp - 25);
}

JXSML_STATIC uint64_t jxsml__varint(jxsml__st *st) { // ICC only
	uint64_t value = 0;
	int32_t shift = 0;
	do {
		if (st->bufsize == 0) return JXSML__ERR("shrt"), (uint64_t) 0;
		int32_t b = jxsml__u(st, 8);
		value |= (uint64_t) (b & 0x7f) << shift;
		if (b < 128) return value;
		shift += 7;
	} while (shift < 63);
	return JXSML__ERR("vint"), (uint64_t) 0;
}

JXSML_INLINE int32_t jxsml__u8(jxsml__st *st) { // ANS distribution decoding only
	if (jxsml__u(st, 1)) {
		int32_t n = jxsml__u(st, 3);
		return jxsml__u(st, n) + (1 << n);
	} else {
		return 0;
	}
}

// equivalent to u(ceil(log2(max + 1))), decodes [0, max] with the minimal number of bits
JXSML_INLINE int32_t jxsml__at_most(jxsml__st *st, int32_t max) {
	int32_t v = max > 0 ? jxsml__u(st, jxsml__ceil_lg32((uint32_t) max + 1)) : 0;
	if (v > max) return JXSML__ERR("rnge"), 0;
	return v;
}

////////////////////////////////////////////////////////////////////////////////
// prefix code

static const uint8_t JXSML__REV5[32] = {
	0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
	1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
};

// a prefix code tree is represented by max_len (max code length), fast_len (explained below),
// and an int32_t table either statically or dynamically constructed.
// table[0] .. table[(1 << fast_len) - 1] are a lookup table for first fast_len bits.
// each entry is either a direct entry (positive),
// or an index to the first overflow entry (negative, the actual index is -table[i]).
//
// subsequent overflow entries are used for codes with the length > fast_len;
// the decoder reads overflow entries in the order, stopping at the first match.
// the last overflow entry is implicit so the table is constructed to ensure the match.
//
// a direct or overflow entry format:
// - bits 0..3: codeword length - fast_len
// - bits 4..15: codeword, skipping first fast_len bits, ordered like st->bits (overflow only)
// - bits 16..30: corresponding alphabet

enum { JXSML__MAX_TYPICAL_FAST_LEN = 7 }; // limit fast_len for typical cases
enum { JXSML__MAX_TABLE_GROWTH = 2 }; // we can afford 2x the table size if beneficial though

JXSML_INLINE int32_t jxsml__prefix_code(jxsml__st *, int32_t, int32_t, const int32_t *);

// read a prefix code tree, as specified in RFC 7932 section 3
JXSML_STATIC jxsml_err jxsml__init_prefix_code(
	jxsml__st *st, int32_t l2size, int32_t *out_fast_len, int32_t *out_max_len, int32_t **out_table
) {
	// for ordinary cases we have three different prefix codes:
	// layer 0 (fixed): up to 4 bits, decoding into 0..5, used L1SIZE = 18 times
	// layer 1: up to 5 bits, decoding into 0..17, used l2size times
	// layer 2: up to 15 bits, decoding into 0..l2size-1
	enum { L1SIZE = 18, L0MAXLEN = 4, L1MAXLEN = 5, L2MAXLEN = 15 };
	enum { L1CODESUM = 1 << L1MAXLEN, L2CODESUM = 1 << L2MAXLEN };
	static const int32_t L0TABLE[1 << L0MAXLEN] = {
		0x00002, 0x40002, 0x30002, 0x20003, 0x00002, 0x40002, 0x30002, 0x10004,
		0x00002, 0x40002, 0x30002, 0x20003, 0x00002, 0x40002, 0x30002, 0x50004,
	};
	static const uint8_t L1ZIGZAG[L1SIZE] = {1,2,3,4,0,5,17,6,16,7,8,9,10,11,12,13,14,15};

	int32_t l1lengths[L1SIZE] = {0}, *l2lengths = NULL;
	int32_t l1counts[L1MAXLEN + 1] = {0}, l2counts[L2MAXLEN + 1] = {0};
	int32_t l1starts[L1MAXLEN + 1], l2starts[L2MAXLEN + 1], l2overflows[L2MAXLEN + 1];
	int32_t l1table[1 << L1MAXLEN] = {0}, *l2table = NULL;
	int32_t total, code, hskip, fast_len, i, j;

	JXSML__ASSERT(l2size > 0 && l2size <= 0x8000);
	if (l2size == 1) {
		*out_fast_len = *out_max_len = 0;
		JXSML__SHOULD(*out_table = malloc(sizeof(int32_t)), "!mem");
		(*out_table)[0] = 0;
		return 0;
	}

	hskip = jxsml__u(st, 2);
	if (hskip == 1) { // simple prefix codes (section 3.4)
		static const struct { int8_t maxlen, sortfrom, sortto, len[8], symref[8]; } TEMPLATES[5] = {
			{ 3, 2, 4, {1,2,1,3,1,2,1,3}, {0,1,0,2,0,1,0,3} }, // NSYM=4 tree-select 1 (1233)
			{ 0, 0, 0, {0}, {0} },                             // NSYM=1 (0)
			{ 1, 0, 2, {1,1}, {0,1} },                         // NSYM=2 (11)
			{ 2, 1, 3, {1,2,1,2}, {0,1,0,2} },                 // NSYM=3 (122)
			{ 2, 0, 4, {2,2,2,2}, {0,1,2,3} },                 // NSYM=4 tree-select 0 (2222)
		};
		int32_t nsym = jxsml__u(st, 2) + 1, syms[4], tmp;
		for (i = 0; i < nsym; ++i) {
			syms[i] = jxsml__at_most(st, l2size - 1);
			for (j = 0; j < i; ++j) JXSML__SHOULD(syms[i] != syms[j], "hufd");
		}
		if (nsym == 4 && jxsml__u(st, 1)) nsym = 0; // tree-select
		JXSML__RAISE_DELAYED();

		// symbols of the equal length have to be sorted
		for (i = TEMPLATES[nsym].sortfrom + 1; i < TEMPLATES[nsym].sortto; ++i) {
			for (j = i; j > TEMPLATES[nsym].sortfrom && syms[j - 1] > syms[j]; --j) {
				tmp = syms[j - 1];
				syms[j - 1] = syms[j];
				syms[j] = tmp;
			}
		}

		*out_fast_len = *out_max_len = TEMPLATES[nsym].maxlen;
		JXSML__SHOULD(*out_table = malloc(sizeof(int32_t) << *out_max_len), "!mem");
		for (i = 0; i < (1 << *out_max_len); ++i) {
			(*out_table)[i] = (syms[TEMPLATES[nsym].symref[i]] << 16) | (int32_t) TEMPLATES[nsym].len[i];
		}
		return 0;
	}

	// complex prefix codes (section 3.5): read layer 1 code lengths using the layer 0 code
	total = 0;
	for (i = l1counts[0] = hskip; i < L1SIZE && total < L1CODESUM; ++i) {
		l1lengths[L1ZIGZAG[i]] = code = jxsml__prefix_code(st, L0MAXLEN, L0MAXLEN, L0TABLE);
		++l1counts[code];
		if (code) total += L1CODESUM >> code;
	}
	JXSML__SHOULD(total == L1CODESUM && l1counts[0] != i, "hufd");

	// construct the layer 1 tree
	if (l1counts[0] == i - 1) { // special case: a single code repeats as many as possible
		for (i = 0; l1lengths[i]; ++i); // this SHOULD terminate
		for (code = 0; code < L1CODESUM; ++code) l1table[code] = i;
		l1lengths[i] = 0;
	} else {
		l1starts[1] = 0;
		for (i = 2; i <= L1MAXLEN; ++i) {
			l1starts[i] = l1starts[i - 1] + (l1counts[i - 1] << (L1MAXLEN - (i - 1)));
		}
		for (i = 0; i < L1SIZE; ++i) {
			int32_t n = l1lengths[i], *start = &l1starts[n];
			if (n == 0) continue;
			for (code = (int32_t) JXSML__REV5[*start]; code < L1CODESUM; code += 1 << n) {
				l1table[code] = (i << 16) | n;
			}
			*start += L1CODESUM >> n;
		}
	}

	{ // read layer 2 code lengths using the layer 1 code
		int32_t prev = 8, rep, prev_rep = 0; // prev_rep: prev repeat count of 16(pos)/17(neg) so far
		JXSML__SHOULD(l2lengths = calloc((size_t) l2size, sizeof(int32_t)), "!mem");
		for (i = total = 0; i < l2size && total < L2CODESUM; ) {
			code = jxsml__prefix_code(st, L1MAXLEN, L1MAXLEN, l1table);
			if (code < 16) {
				l2lengths[i++] = code;
				++l2counts[code];
				if (code) {
					total += L2CODESUM >> code;
					prev = code;
				}
				prev_rep = 0;
			} else if (code == 16) { // repeat non-zero 3+u(2) times
				// instead of keeping the current repeat count, we calculate a difference
				// between the previous and current repeat count and directly apply the delta
				if (prev_rep < 0) prev_rep = 0;
				rep = (prev_rep > 0 ? 4 * prev_rep - 5 : 3) + jxsml__u(st, 2);
				total += (L2CODESUM * (rep - prev_rep)) >> prev;
				l2counts[prev] += rep - prev_rep;
				for (; prev_rep < rep; ++prev_rep) l2lengths[i++] = prev;
			} else { // code == 17: repeat zero 3+u(3) times
				if (prev_rep > 0) prev_rep = 0;
				rep = (prev_rep < 0 ? 8 * prev_rep + 13 : -3) - jxsml__u(st, 3);
				for (; prev_rep > rep; --prev_rep) l2lengths[i++] = 0;
			}
			JXSML__RAISE_DELAYED();
		}
		JXSML__SHOULD(total == L2CODESUM, "hufd");
	}

	// determine the layer 2 lookup table size
	l2starts[1] = 0;
	*out_max_len = 1;
	for (i = 2; i <= L2MAXLEN; ++i) {
		l2starts[i] = l2starts[i - 1] + (l2counts[i - 1] << (L2MAXLEN - (i - 1)));
		if (l2counts[i]) *out_max_len = i;
	}
	if (*out_max_len <= JXSML__MAX_TYPICAL_FAST_LEN) {
		fast_len = *out_max_len;
		JXSML__SHOULD(l2table = malloc(sizeof(int32_t) << fast_len), "!mem");
	} else {
		// if the distribution is flat enough the max fast_len might be slow
		// because most LUT entries will be overflow refs so we will hit slow paths for most cases.
		// we therefore calculate the table size with the max fast_len,
		// then find the largest fast_len within the specified table growth factor.
		int32_t size, size_limit, size_used;
		fast_len = JXSML__MAX_TYPICAL_FAST_LEN;
		size = 1 << fast_len;
		for (i = fast_len + 1; i <= *out_max_len; ++i) size += l2counts[i];
		size_used = size;
		size_limit = size * JXSML__MAX_TABLE_GROWTH;
		for (i = fast_len + 1; i <= *out_max_len; ++i) {
			size = size + (1 << i) - l2counts[i];
			if (size <= size_limit) {
				size_used = size;
				fast_len = i;
			}
		}
		l2overflows[fast_len + 1] = 1 << fast_len;
		for (i = fast_len + 2; i <= *out_max_len; ++i) l2overflows[i] = l2overflows[i - 1] + l2counts[i - 1];
		JXSML__SHOULD(l2table = malloc(sizeof(int32_t) * (size_t) (size_used + 1)), "!mem");
		// this entry should be unreachable, but should work as a stopper if there happens to be a logic bug
		l2table[size_used] = 0;
	}

	// fill the layer 2 table
	for (i = 0; i < l2size; ++i) {
		int32_t n = l2lengths[i], *start = &l2starts[n];
		if (n == 0) continue;
		code = (int32_t) JXSML__REV5[*start & 31] << 10 |
			(int32_t) JXSML__REV5[*start >> 5 & 31] << 5 |
			(int32_t) JXSML__REV5[*start >> 10];
		if (n <= fast_len) {
			for (; code < (1 << fast_len); code += 1 << n) l2table[code] = (i << 16) | n;
			*start += L2CODESUM >> n;
		} else {
			// there should be exactly one code which is a LUT-covered prefix plus all zeroes;
			// in the canonical Huffman tree that code would be in the first overflow entry
			if ((code >> fast_len) == 0) l2table[code] = -l2overflows[n];
			*start += L2CODESUM >> n;
			l2table[l2overflows[n]++] = (i << 16) | (code >> fast_len << 4) | (n - fast_len);
		}
	}

	*out_fast_len = fast_len;
	*out_table = l2table;
	return 0;

error:
	free(l2lengths);
	free(l2table);
	return st->err;
}

JXSML_STATIC int32_t jxsml__match_overflow(jxsml__st *st, int32_t fast_len, const int32_t *table) {
	int32_t entry, code, code_len;
	st->nbits -= fast_len;
	st->bits >>= fast_len;
	st->bits_read += (size_t) fast_len;
	do {
		entry = *table++;
		code = (entry >> 4) & 0xfff;
		code_len = entry & 15;
	} while (code != (int32_t) (st->bits & ((1u << code_len) - 1)));
	return entry;
}

JXSML_INLINE int32_t jxsml__prefix_code(jxsml__st *st, int32_t fast_len, int32_t max_len, const int32_t *table) {
	int32_t entry, code_len;
	if (st->nbits < max_len && jxsml__always_refill(st, 0)) return 0;
	entry = table[st->bits & ((1u << fast_len) - 1)];
	if (entry < 0 && fast_len < max_len) entry = jxsml__match_overflow(st, fast_len, table - entry);
	code_len = entry & 15;
	st->nbits -= code_len;
	st->bits >>= code_len;
	st->bits_read += (size_t) code_len;
	return entry >> 16;
}

////////////////////////////////////////////////////////////////////////////////
// hybrid integer encoding

// token < 2^split_exp is interpreted as is.
// otherwise (token - 2^split_exp) is split into NNHHHLLL where config determines H/L lengths.
// then MMMMM = u(NN + split_exp - H/L lengths) is read; the decoded value is 1HHHMMMMMLLL.
typedef struct { int8_t split_exp, msb_in_token, lsb_in_token; } jxsml__hybrid_int_config_t;

JXSML_STATIC jxsml_err jxsml__hybrid_int_config(jxsml__st *st, int32_t log_alpha_size, jxsml__hybrid_int_config_t *out) {
	out->split_exp = (int8_t) jxsml__at_most(st, log_alpha_size);
	if (out->split_exp != log_alpha_size) {
		out->msb_in_token = (int8_t) jxsml__at_most(st, out->split_exp);
		out->lsb_in_token = (int8_t) jxsml__at_most(st, out->split_exp - out->msb_in_token);
	} else {
		out->msb_in_token = out->lsb_in_token = 0;
	}
	return st->err;
}

JXSML_INLINE int32_t jxsml__hybrid_int(jxsml__st *st, int32_t token, jxsml__hybrid_int_config_t config) {
	int32_t midbits, lo, mid, hi, top, bits_in_token, split = 1 << config.split_exp;
	if (token < split) return token;
	bits_in_token = config.msb_in_token + config.lsb_in_token;
	midbits = config.split_exp - bits_in_token + ((token - split) >> bits_in_token);
	// TODO midbits can overflow!
	mid = jxsml__u(st, midbits);
	top = 1 << config.msb_in_token;
	lo = token & ((1 << config.lsb_in_token) - 1);
	hi = (token >> config.lsb_in_token) & (top - 1);
	return ((top | hi) << (midbits + config.lsb_in_token)) | ((mid << config.lsb_in_token) | lo);
}

////////////////////////////////////////////////////////////////////////////////
// rANS alias table

enum { JXSML__DIST_BITS = 12 };

// the alias table of size N is conceptually an array of N buckets with probability 1/N each,
// where each bucket corresponds to at most two symbols distinguished by the cutoff point.
// this is done by rearranging symbols so that every symbol boundary falls into distinct buckets.
// so it allows *any* distribution of N symbols to be decoded in a constant time after the setup.
// the table is not unique though, so the spec needs to specify the exact construction algorithm.
//
//   input range: 0         cutoff               bucket_size
//                +-----------|----------------------------+
// output symbol: |     i     |           symbol           | <- bucket i
//                +-----------|----------------------------+
//  output range: 0     cutoff|offset    offset+bucket_size
typedef struct { int16_t cutoff, offset_or_next, symbol; } jxsml__alias_bucket;

JXSML_STATIC jxsml_err jxsml__init_alias_map(
	jxsml__st *st, const int16_t *D, int32_t log_alpha_size, jxsml__alias_bucket **out
) {
	int16_t log_bucket_size = (int16_t) (JXSML__DIST_BITS - log_alpha_size);
	int16_t bucket_size = (int16_t) (1 << log_bucket_size);
	int16_t table_size = (int16_t) (1 << log_alpha_size);
	jxsml__alias_bucket *buckets = NULL;
	// the underfull and overfull stacks are implicit linked lists; u/o resp. is the top index,
	// buckets[u/o].next is the second-to-top index and so on. an index -1 indicates the bottom.
	int16_t u = -1, o = -1, i, j;

	JXSML__ASSERT(5 <= log_alpha_size && log_alpha_size <= 8);
	JXSML__SHOULD(buckets = malloc(sizeof(jxsml__alias_bucket) << log_alpha_size), "!mem");

	for (i = 0; i < table_size && !D[i]; ++i);
	for (j = (int16_t) (i + 1); j < table_size && !D[j]; ++j);
	if (i < table_size && j >= table_size) { // D[i] is the only non-zero probability
		for (j = 0; j < table_size; ++j) {
			buckets[j].symbol = i;
			buckets[j].offset_or_next /*offset*/ = (int16_t) (j << log_bucket_size);
			buckets[j].cutoff = 0;
		}
		*out = buckets;
		return 0;
	}

	// each bucket is either settled (fields fully set) or unsettled (only `cutoff` is set).
	// unsettled buckets are either in the underfull stack, in which case `cutoff < bucket_size`,
	// or in the overfull stack, in which case `cutoff > bucket_size`. other fields are left
	// unused, so `offset` in settled buckets is aliased to `next` in unsettled buckets.
	// when rearranging results in buckets with `cutoff == bucket_size`,
	// final fields are set and they become settled; eventually every bucket has to be settled.
	for (i = 0; i < table_size; ++i) {
		int16_t cutoff = D[i];
		buckets[i].cutoff = cutoff;
		if (cutoff > bucket_size) {
			buckets[i].offset_or_next /*next*/ = o;
			o = i;
		} else if (cutoff < bucket_size) {
			buckets[i].offset_or_next /*next*/ = u;
			u = i;
		} else { // immediately settled
			buckets[i].symbol = i;
			buckets[i].offset_or_next /*offset*/ = 0;
		}
	}

	while (o >= 0) {
		int16_t by, tmp;
		JXSML__ASSERT(u >= 0);
		by = (int16_t) (bucket_size - buckets[u].cutoff);
		// move the input range [cutoff[o] - by, cutoff[o]] of the bucket o into
		// the input range [cutoff[u], bucket_size] of the bucket u (which is settled after this)
		tmp = buckets[u].offset_or_next /*next*/;
		buckets[o].cutoff = (int16_t) (buckets[o].cutoff - by);
		buckets[u].symbol = o;
		buckets[u].offset_or_next /*offset*/ = (int16_t) (buckets[o].cutoff - buckets[u].cutoff);
		u = tmp;
		if (buckets[o].cutoff < bucket_size) { // o is now underfull, move to the underfull stack
			tmp = buckets[o].offset_or_next /*next*/;
			buckets[o].offset_or_next /*next*/ = u;
			u = o;
			o = tmp;
		} else if (buckets[o].cutoff == bucket_size) { // o is also settled
			tmp = buckets[o].offset_or_next /*next*/;
			buckets[o].offset_or_next /*offset*/ = 0;
			o = tmp;
		}
	}

	JXSML__ASSERT(u < 0);
	*out = buckets;
	return 0;

error:
	free(buckets);
	return st->err;
}

enum { JXSML__ANS_INIT_STATE = 0x130000 };

JXSML_STATIC int32_t jxsml__ans_code(
	jxsml__st *st, uint32_t *state, int32_t log_bucket_size,
	const int16_t *D, const jxsml__alias_bucket *aliases
) {
	if (*state == 0) {
		*state = (uint32_t) jxsml__u(st, 16);
		*state |= (uint32_t) jxsml__u(st, 16) << 16;
	}
	{
		int32_t index = (int32_t) (*state & 0xfff);
		int32_t i = index >> log_bucket_size;
		int32_t pos = index & ((1 << log_bucket_size) - 1);
		const jxsml__alias_bucket *bucket = &aliases[i];
		int32_t symbol = pos < bucket->cutoff ? i : bucket->symbol;
		int32_t offset = pos < bucket->cutoff ? 0 : bucket->offset_or_next /*offset*/;
#ifdef JXSML_DEBUG
		if (D[symbol] == 0) return JXSML__ERR("ans0"), 0;
#endif
		*state = (uint32_t) D[symbol] * (*state >> 12) + (uint32_t) offset + (uint32_t) pos;
		if (*state < (1u << 16)) *state = (*state << 16) | (uint32_t) jxsml__u(st, 16);
		return symbol;
	}
}

////////////////////////////////////////////////////////////////////////////////
// entropy code

typedef union {
	jxsml__hybrid_int_config_t config;
	struct {
		jxsml__hybrid_int_config_t config;
		int32_t count;
	} init; // only used during the initialization
	struct {
		jxsml__hybrid_int_config_t config;
		int16_t *D;
		jxsml__alias_bucket *aliases;
	} ans; // if parent use_prefix_code is false
	struct {
		jxsml__hybrid_int_config_t config;
		int16_t fast_len, max_len;
		int32_t *table;
	} prefix; // if parent use_prefix_code is true
} jxsml__code_cluster_t;

typedef struct {
	int32_t num_dist;
	int lz77_enabled, use_prefix_code;
	int32_t min_symbol, min_length;
	int32_t log_alpha_size; // only used when use_prefix_code is false
	int32_t num_clusters; // in [1, min(num_dist, 256)]
	uint8_t *cluster_map; // each in [0, num_clusters)
	jxsml__hybrid_int_config_t lz_len_config;
	jxsml__code_cluster_t *clusters;
} jxsml__code_spec_t;

typedef struct {
	const jxsml__code_spec_t *spec;
	// LZ77 states
	int32_t num_to_copy, copy_pos, num_decoded;
	int32_t window_cap, *window;
	// ANS state
	uint32_t ans_state; // 0 if uninitialized
} jxsml__code_t;

JXSML_STATIC jxsml_err jxsml__code_spec(jxsml__st *, int32_t, jxsml__code_spec_t *);
JXSML_STATIC jxsml_err jxsml__code(jxsml__st *, int32_t, int32_t, jxsml__code_t *);
JXSML_STATIC jxsml_err jxsml__finish_and_free_code(jxsml__st *, jxsml__code_t *);
JXSML_STATIC void jxsml__free_code(jxsml__code_t *);
JXSML_STATIC void jxsml__free_code_spec(jxsml__code_spec_t *);

JXSML_STATIC jxsml_err jxsml__cluster_map(
	jxsml__st *st, int32_t num_dist, int32_t max_allowed, int32_t *num_clusters, uint8_t *map
) {
	jxsml__code_spec_t codespec = {0}; // cluster map might be recursively coded
	jxsml__code_t code = { .spec = &codespec };
	uint32_t seen[8] = {0};
	int32_t i, j;

	JXSML__ASSERT(max_allowed >= 1 && max_allowed <= 256);
	if (max_allowed > num_dist) max_allowed = num_dist;

	if (num_dist == 1) {
		*num_clusters = 1;
		map[0] = 0;
		return 0;
	}

	if (jxsml__u(st, 1)) { // is_simple (# clusters < 8)
		int32_t nbits = jxsml__u(st, 2);
		for (i = 0; i < num_dist; ++i) {
			map[i] = (uint8_t) jxsml__u(st, nbits);
			JXSML__SHOULD((int32_t) map[i] < max_allowed, "clst");
		}
	} else {
		int use_mtf = jxsml__u(st, 1);

		// num_dist=1 prevents further recursion
		JXSML__TRY(jxsml__code_spec(st, 1, &codespec));
		for (i = 0; i < num_dist; ++i) {
			int32_t index = jxsml__code(st, 0, 0, &code);
			JXSML__SHOULD(index < max_allowed, "clst");
			map[i] = (uint8_t) index;
		}
		JXSML__TRY(jxsml__finish_and_free_code(st, &code));
		jxsml__free_code_spec(&codespec);

		if (use_mtf) {
			uint8_t mtf[256], moved;
			for (i = 0; i < 256; ++i) mtf[i] = (uint8_t) i;
			for (i = 0; i < num_dist; ++i) {
				j = map[i];
				map[i] = moved = mtf[j];
				for (; j > 0; --j) mtf[j] = mtf[j - 1];
				mtf[0] = moved;
			}
		}
	}

	// verify cluster_map and determine the implicit num_clusters
	for (i = 0; i < num_dist; ++i) seen[map[i] >> 5] |= (uint32_t) 1 << (map[i] & 31);
	for (i = 0; i < 256 && (seen[i >> 5] >> (i & 31) & 1); ++i);
	JXSML__ASSERT(i > 0);
	*num_clusters = i; // the first unset position or 256 if none
	for (; i < 256 && !(seen[i >> 5] >> (i & 31) & 1); ++i);
	JXSML__SHOULD(i == 256, "clst"); // no more set position beyond num_clusters

	return 0;

error:
	jxsml__free_code(&code);
	jxsml__free_code_spec(&codespec);
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__code_spec(jxsml__st *st, int32_t num_dist, jxsml__code_spec_t *spec) {
	int32_t i, j;

	spec->cluster_map = NULL;
	spec->clusters = NULL;

	// LZ77Params
	spec->lz77_enabled = jxsml__u(st, 1);
	if (spec->lz77_enabled) {
		spec->min_symbol = jxsml__u32(st, 224, 0, 512, 0, 4096, 0, 8, 15);
		spec->min_length = jxsml__u32(st, 3, 0, 4, 0, 5, 2, 9, 8);
		JXSML__TRY(jxsml__hybrid_int_config(st, 8, &spec->lz_len_config));
		++num_dist; // num_dist - 1 is a synthesized LZ77 length distribution
	} else {
		spec->min_symbol = spec->min_length = 0x7fffffff;
	}

	// cluster_map: a mapping from context IDs to actual distributions
	JXSML__SHOULD(spec->cluster_map = malloc(sizeof(uint8_t) * (size_t) num_dist), "!mem");
	JXSML__TRY(jxsml__cluster_map(st, num_dist, 256, &spec->num_clusters, spec->cluster_map));

	JXSML__SHOULD(spec->clusters = calloc((size_t) spec->num_clusters, sizeof(jxsml__code_cluster_t)), "!mem");

	spec->use_prefix_code = jxsml__u(st, 1);
	if (spec->use_prefix_code) {
		for (i = 0; i < spec->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, 15, &spec->clusters[i].config));
		}

		for (i = 0; i < spec->num_clusters; ++i) {
			if (jxsml__u(st, 1)) {
				int32_t n = jxsml__u(st, 4);
				spec->clusters[i].init.count = 1 + (1 << n) + jxsml__u(st, n);
				JXSML__SHOULD(spec->clusters[i].init.count <= (1 << 15), "hufd");
			} else {
				spec->clusters[i].init.count = 1;
			}
		}

		for (i = 0; i < spec->num_clusters; ++i) {
			jxsml__code_cluster_t *c = &spec->clusters[i];
			int32_t fast_len, max_len;
			JXSML__TRY(jxsml__init_prefix_code(st, c->init.count, &fast_len, &max_len, &c->prefix.table));
			c->prefix.fast_len = (int16_t) fast_len;
			c->prefix.max_len = (int16_t) max_len;
		}
	} else {
		enum { DISTBITS = JXSML__DIST_BITS, DISTSUM = 1 << DISTBITS };

		spec->log_alpha_size = 5 + jxsml__u(st, 2);
		for (i = 0; i < spec->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, spec->log_alpha_size, &spec->clusters[i].config));
		}

		for (i = 0; i < spec->num_clusters; ++i) {
			int32_t table_size = 1 << spec->log_alpha_size;
			int16_t *D;
			JXSML__SHOULD(D = malloc(sizeof(int16_t) * (size_t) table_size), "!mem");
			spec->clusters[i].ans.D = D;

			switch (jxsml__u(st, 2)) {
			case 1: // one entry
				memset(D, 0, sizeof(int16_t) * (size_t) table_size);
				D[jxsml__u8(st)] = DISTSUM;
				break;

			case 3: { // two entries
				int32_t v1 = jxsml__u8(st);
				int32_t v2 = jxsml__u8(st);
				JXSML__SHOULD(v1 != v2 && v1 < table_size && v2 < table_size, "ansd");
				memset(D, 0, sizeof(int16_t) * (size_t) table_size);
				D[v1] = (int16_t) jxsml__u(st, DISTBITS);
				D[v2] = (int16_t) (DISTSUM - D[v1]);
				break;
			}

			case 2: { // evenly distribute to first `alpha_size` entries (false -> true)
				int32_t alpha_size = jxsml__u8(st) + 1;
				int16_t d = (int16_t) (DISTSUM / alpha_size);
				int16_t bias_size = (int16_t) (DISTSUM - d * alpha_size);
				for (j = 0; j < bias_size; ++j) D[j] = (int16_t) (d + 1);
				for (; j < alpha_size; ++j) D[j] = d;
				for (; j < table_size; ++j) D[j] = 0;
				break;
			}

			case 0: { // bit counts + RLE (false -> false)
				int32_t len, shift, alpha_size, omit_log, omit_pos, code, total, n;
				int32_t ncodes, codes[259]; // exponents if >= 0, negated repeat count if < 0

				len = jxsml__u(st, 1) ? jxsml__u(st, 1) ? jxsml__u(st, 1) ? 3 : 2 : 1 : 0;
				shift = jxsml__u(st, len) + (1 << len) - 1;
				JXSML__SHOULD(shift <= 13, "ansd");
				alpha_size = jxsml__u8(st) + 3;

				omit_log = -1; // there should be at least one non-RLE code
				for (j = ncodes = 0; j < alpha_size; ) {
					static const int32_t TABLE[] = { // reinterpretation of kLogCountLut
						0xa0003,     -16, 0x70003, 0x30004, 0x60003, 0x80003, 0x90003, 0x50004,
						0xa0003, 0x40004, 0x70003, 0x10004, 0x60003, 0x80003, 0x90003, 0x20004,
						0x00011, 0xb0022, 0xc0003, 0xd0043, // overflow for ...0001
					};
					code = jxsml__prefix_code(st, 4, 7, TABLE);
					if (code < 13) {
						++j;
						codes[ncodes++] = code;
						if (omit_log < code) omit_log = code;
					} else {
						j += code = jxsml__u8(st) + 4;
						codes[ncodes++] = -code;
					}
				}
				JXSML__SHOULD(j == alpha_size && omit_log >= 0, "ansd");

				omit_pos = -1;
				for (j = n = total = 0; j < ncodes; ++j) {
					code = codes[j];
					if (code < 0) { // repeat
						int16_t prev = n > 0 ? D[n - 1] : 0;
						JXSML__SHOULD(prev >= 0, "ansd"); // implicit D[n] followed by RLE
						total += (int32_t) prev * (int32_t) -code;
						while (code++ < 0) D[n++] = prev;
					} else if (code == omit_log) { // the first longest D[n] is "omitted" (implicit)
						omit_pos = n;
						omit_log = -1; // this branch runs at most once
						D[n++] = -1;
					} else if (code < 2) {
						total += code;
						D[n++] = (int16_t) code;
					} else {
						int32_t bitcount;
						--code;
						bitcount = jxsml__min32(jxsml__max32(0, shift - ((DISTBITS - code) >> 1)), code);
						code = (1 << code) + (jxsml__u(st, bitcount) << (code - bitcount));
						total += code;
						D[n++] = (int16_t) code;
					}
				}
				for (; n < table_size; ++n) D[n] = 0;
				JXSML__ASSERT(omit_pos >= 0);
				JXSML__SHOULD(total <= DISTSUM, "ansd");
				D[omit_pos] = (int16_t) (DISTSUM - total);
				break;
			}

			default: JXSML__UNREACHABLE();
			}

			JXSML__TRY(jxsml__init_alias_map(st, D, spec->log_alpha_size, &spec->clusters[i].ans.aliases));
		}
	}

	spec->num_dist = num_dist;
	return 0;

error:
	jxsml__free_code_spec(spec);
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__entropy_code_cluster(
	jxsml__st *st, int use_prefix_code, int32_t log_alpha_size,
	jxsml__code_cluster_t *cluster, uint32_t *ans_state
) {
	if (use_prefix_code) {
		return jxsml__prefix_code(st, cluster->prefix.fast_len, cluster->prefix.max_len, cluster->prefix.table);
	} else {
		return jxsml__ans_code(st, ans_state, JXSML__DIST_BITS - log_alpha_size, cluster->ans.D, cluster->ans.aliases);
	}
}

// aka DecodeHybridVarLenUint
JXSML_STATIC jxsml_err jxsml__code(jxsml__st *st, int32_t ctx, int32_t dist_mult, jxsml__code_t *code) {
	const jxsml__code_spec_t *spec = code->spec;
	int32_t token, distance, log_alpha_size;
	jxsml__code_cluster_t *cluster;
	int use_prefix_code;

	if (code->num_to_copy > 0) {
continue_lz77:
		--code->num_to_copy;
		return code->window[code->num_decoded++ & 0xfffff] = code->window[code->copy_pos++ & 0xfffff];
	}

#ifdef JXSML_DEBUG
	if (ctx >= spec->num_dist) return JXSML__ERR("!exp"), 0;
#endif
	use_prefix_code = spec->use_prefix_code;
	log_alpha_size = spec->log_alpha_size;
	cluster = &spec->clusters[spec->cluster_map[ctx]];
	token = jxsml__entropy_code_cluster(st, use_prefix_code, log_alpha_size, cluster, &code->ans_state);
	if (token >= spec->min_symbol) { // this is large enough if lz77_enabled is false
		jxsml__code_cluster_t *lz_cluster = &spec->clusters[spec->cluster_map[spec->num_dist - 1]];
		code->num_to_copy = jxsml__hybrid_int(st, token - spec->min_symbol, spec->lz_len_config) + spec->min_length;
		token = jxsml__entropy_code_cluster(st, use_prefix_code, log_alpha_size, lz_cluster, &code->ans_state);
		distance = jxsml__hybrid_int(st, token, lz_cluster->config);
		if (st->err) return 0;
		if (!dist_mult) {
			++distance;
		} else if (distance >= 120) {
			distance -= 119;
		} else {
			static const uint8_t SPECIAL_DISTANCES[120] = { // {a,b} encoded as (a+7)*16+b
				0x71, 0x80, 0x81, 0x61, 0x72, 0x90, 0x82, 0x62, 0x91, 0x51, 0x92, 0x52,
				0x73, 0xa0, 0x83, 0x63, 0xa1, 0x41, 0x93, 0x53, 0xa2, 0x42, 0x74, 0xb0,
				0x84, 0x64, 0xb1, 0x31, 0xa3, 0x43, 0x94, 0x54, 0xb2, 0x32, 0x75, 0xa4,
				0x44, 0xb3, 0x33, 0xc0, 0x85, 0x65, 0xc1, 0x21, 0x95, 0x55, 0xc2, 0x22,
				0xb4, 0x34, 0xa5, 0x45, 0xc3, 0x23, 0x76, 0xd0, 0x86, 0x66, 0xd1, 0x11,
				0x96, 0x56, 0xd2, 0x12, 0xb5, 0x35, 0xc4, 0x24, 0xa6, 0x46, 0xd3, 0x13,
				0x77, 0xe0, 0x87, 0x67, 0xc5, 0x25, 0xe1, 0x01, 0xb6, 0x36, 0xd4, 0x14,
				0x97, 0x57, 0xe2, 0x02, 0xa7, 0x47, 0xe3, 0x03, 0xc6, 0x26, 0xd5, 0x15,
				0xf0, 0xb7, 0x37, 0xe4, 0x04, 0xf1, 0xf2, 0xd6, 0x16, 0xf3, 0xc7, 0x27,
				0xe5, 0x05, 0xf4, 0xd7, 0x17, 0xe6, 0x06, 0xf5, 0xe7, 0x07, 0xf6, 0xf7,
			};
			int32_t special = (int32_t) SPECIAL_DISTANCES[distance];
			distance = ((special >> 4) - 7) + dist_mult * (special & 7);
		}
		if (distance > code->num_decoded) distance = code->num_decoded;
		if (distance > (1 << 20)) distance = 1 << 20;
		code->copy_pos = code->num_decoded - distance;
		goto continue_lz77;
	}

	token = jxsml__hybrid_int(st, token, cluster->config);
	if (st->err) return 0;
	if (spec->lz77_enabled) {
		if (!code->window) { // XXX should be dynamically resized
			code->window = malloc(sizeof(int32_t) << 20);
			if (!code->window) return JXSML__ERR("!mem"), 0;
		}
		code->window[code->num_decoded++ & 0xfffff] = token;
	}
	return token;
}

JXSML_STATIC void jxsml__free_code(jxsml__code_t *code) {
	free(code->window);
	code->window = NULL;
	code->window_cap = 0;
}

JXSML_STATIC jxsml_err jxsml__finish_and_free_code(jxsml__st *st, jxsml__code_t *code) {
	if (!code->spec->use_prefix_code) {
		if (code->ans_state) {
			JXSML__SHOULD(code->ans_state == JXSML__ANS_INIT_STATE, "ans?");
		} else { // edge case: if no symbols have been read the state has to be read at this point
			JXSML__SHOULD(jxsml__u(st, 16) == (JXSML__ANS_INIT_STATE & 0xffff), "ans?");
			JXSML__SHOULD(jxsml__u(st, 16) == (JXSML__ANS_INIT_STATE >> 16), "ans?");
		}
	}
	if (code->spec->lz77_enabled) JXSML__SHOULD(code->num_to_copy == 0, "lz7?");
error:
	jxsml__free_code(code);
	return st->err;
}

JXSML_STATIC void jxsml__free_code_spec(jxsml__code_spec_t *spec) {
	int32_t i;
	if (spec->clusters) {
		for (i = 0; i < spec->num_clusters; ++i) {
			if (spec->use_prefix_code) {
				free(spec->clusters[i].prefix.table);
			} else {
				free(spec->clusters[i].ans.D);
				free(spec->clusters[i].ans.aliases);
			}
		}
		free(spec->clusters);
		spec->clusters = NULL;
	}
	free(spec->cluster_map);
	spec->cluster_map = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// image header & metadata

JXSML_STATIC jxsml_err jxsml__size_header(jxsml__st *st, int32_t *outw, int32_t *outh) {
	int32_t div8 = jxsml__u(st, 1);
	*outh = div8 ? (jxsml__u(st, 5) + 1) * 8 : jxsml__u32(st, 1, 9, 1, 13, 1, 18, 1, 30);
	switch (jxsml__u(st, 3)) { // ratio
	case 0: *outw = div8 ? (jxsml__u(st, 5) + 1) * 8 : jxsml__u32(st, 1, 9, 1, 13, 1, 18, 1, 30); break;
	case 1: *outw = *outh; break;
	case 2: *outw = (int32_t) ((uint64_t) *outh * 6 / 5); break;
	case 3: *outw = (int32_t) ((uint64_t) *outh * 4 / 3); break;
	case 4: *outw = (int32_t) ((uint64_t) *outh * 3 / 2); break;
	case 5: *outw = (int32_t) ((uint64_t) *outh * 16 / 9); break;
	case 6: *outw = (int32_t) ((uint64_t) *outh * 5 / 4); break;
	case 7:
		// height is at most 2^30, so width is at most 2^31 which requires uint32_t.
		// but in order to avoid bugs we rarely use unsigned integers, so we just reject it.
		// this should be not a problem as the Main profile Level 10 (the largest profile)
		// already limits height to at most 2^30.
		JXSML__SHOULD(*outh < 0x40000000, "bigg");
		*outw = *outh * 2;
		break;
	default: JXSML__UNREACHABLE();
	}
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__bit_depth(jxsml__st *st, int32_t *outbpp, int32_t *outexpbits) {
	if (jxsml__u(st, 1)) { // float_sample
		int32_t mantissa_bits;
		*outbpp = jxsml__u32(st, 32, 0, 16, 0, 24, 0, 1, 6);
		*outexpbits = jxsml__u(st, 4) + 1;
		mantissa_bits = *outbpp - *outexpbits - 1;
		JXSML__SHOULD(2 <= mantissa_bits && mantissa_bits <= 23, "bpp?");
		JXSML__SHOULD(2 <= *outexpbits && *outexpbits <= 8, "exp?"); // implies bpp in [5,32] when combined
	} else {
		*outbpp = jxsml__u32(st, 8, 0, 10, 0, 12, 0, 1, 6);
		*outexpbits = 0;
		JXSML__SHOULD(1 <= *outbpp && *outbpp <= 31, "bpp?");
	}
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__name(jxsml__st *st, int32_t *outlen, char **outbuf) {
	char *buf = NULL;
	int32_t i, c, cc, len;
	len = jxsml__u32(st, 0, 0, 0, 4, 16, 5, 48, 10);
	if (len > 0) {
		JXSML__SHOULD(buf = malloc((size_t) len + 1), "!mem");
		for (i = 0; i < len; ++i) {
			buf[i] = (char) jxsml__u(st, 8);
			JXSML__RAISE_DELAYED();
		}
		buf[len] = 0;
		for (i = 0; i < len; ) { // UTF-8 verification
			c = (uint8_t) buf[i++];
			cc = (uint8_t) buf[i]; // always accessible thanks to null-termination
			c = c < 0x80 ? 0 : c < 0xc2 ? -1 : c < 0xe0 ? 1 :
				c < 0xf0 ? (c == 0xe0 ? cc >= 0xa0 : c == 0xed ? cc < 0xa0 : 1) ? 2 : -1 :
				c < 0xf5 ? (c == 0xf0 ? cc >= 0x90 : c == 0xf4 ? cc < 0x90 : 1) ? 3 : -1 : -1;
			JXSML__SHOULD(c >= 0 && i + c < len, "name");
			while (c-- > 0) JXSML__SHOULD((buf[i++] & 0xc0) == 0x80, "name");
		}
		*outbuf = buf;
	} else {
		JXSML__RAISE_DELAYED();
		*outbuf = NULL;
	}
	*outlen = len;
	return 0;
error:
	free(buf);
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__customxy(jxsml__st *st, float xy[2]) {
	xy[0] = (float)jxsml__to_signed(jxsml__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21)) / 100000.0f;
	xy[1] = (float)jxsml__to_signed(jxsml__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21)) / 100000.0f;
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__extensions(jxsml__st *st) {
	uint64_t extensions = jxsml__u64(st);
	uint64_t nbits = 0;
	int32_t i;
	for (i = 0; i < 64; ++i) {
		if (extensions >> i & 1) {
			uint64_t n = jxsml__u64(st);
			JXSML__RAISE_DELAYED();
			if (nbits > UINT64_MAX - n) JXSML__RAISE("over");
			nbits += n;
		}
	}
	return jxsml__skip(st, nbits);
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__image_metadata(jxsml__st *st) {
	static const float SRGB_CHROMA[4][2] = { // default chromacity (kD65, kSRGB)
		{0.3127f, 0.3290f}, {0.639998686f, 0.330010138f},
		{0.300003784f, 0.600003357f}, {0.150002046f, 0.059997204f},
	};

	int32_t i, j;

	st->out->orientation = JXSML_ORIENT_TL;
	st->out->intr_width = 0;
	st->out->intr_height = 0;
	st->out->bpp = 8;
	st->out->exp_bits = 0;
	st->out->anim_tps_num = 0;
	st->out->anim_tps_denom = 0;
	st->out->anim_nloops = 0;
	st->out->anim_have_timecodes = 0;
	st->out->icc = NULL;
	st->out->iccsize = 0;
	st->out->cspace = JXSML_CS_CHROMA;
	memcpy(st->out->cpoints, SRGB_CHROMA, sizeof SRGB_CHROMA);
	st->out->gamma_or_tf = JXSML_TF_SRGB;
	st->out->render_intent = JXSML_INTENT_REL;
	st->out->intensity_target = 255.0f;
	st->out->min_nits = 0.0f;
	st->out->linear_below = 0.0f;

	if (!jxsml__u(st, 1)) { // !all_default
		int32_t extra_fields = jxsml__u(st, 1);
		if (extra_fields) {
			st->out->orientation = jxsml__u(st, 3) + 1;
			if (jxsml__u(st, 1)) { // have_intr_size
				JXSML__TRY(jxsml__size_header(st, &st->out->intr_width, &st->out->intr_height));
			}
			if (jxsml__u(st, 1)) { // have_preview
				JXSML__RAISE("TODO: preview");
			}
			if (jxsml__u(st, 1)) { // have_animation
				st->out->anim_tps_num = jxsml__u32(st, 100, 0, 1000, 0, 1, 10, 1, 30);
				st->out->anim_tps_denom = jxsml__u32(st, 1, 0, 1001, 0, 1, 8, 1, 10);
				st->out->anim_nloops = jxsml__u32(st, 0, 0, 0, 3, 0, 16, 0, 32);
				st->out->anim_have_timecodes = jxsml__u(st, 1);
			}
		}
		JXSML__TRY(jxsml__bit_depth(st, &st->out->bpp, &st->out->exp_bits));
		st->modular_16bit_buffers = jxsml__u(st, 1);
		st->num_extra_channels = jxsml__u32(st, 0, 0, 1, 0, 2, 4, 1, 12);
		JXSML__SHOULD(st->ec_info = calloc((size_t) st->num_extra_channels, sizeof(jxsml__ec_info)), "!mem");
		for (i = 0; i < st->num_extra_channels; ++i) {
			jxsml__ec_info *ec = &st->ec_info[i];
			if (jxsml__u(st, 1)) { // d_alpha
				ec->type = JXSML_EC_ALPHA;
				ec->bpp = 8;
				ec->exp_bits = ec->dim_shift = ec->name_len = 0;
				ec->name = NULL;
				ec->data.alpha_associated = 0;
			} else {
				ec->type = jxsml__enum(st);
				JXSML__TRY(jxsml__bit_depth(st, &ec->bpp, &ec->exp_bits));
				ec->dim_shift = jxsml__u32(st, 0, 0, 3, 0, 4, 0, 1, 3);
				JXSML__TRY(jxsml__name(st, &ec->name_len, &ec->name));
				switch (ec->type) {
				case JXSML_EC_ALPHA:
					ec->data.alpha_associated = jxsml__u(st, 1);
					break;
				case JXSML_EC_SPOT_COLOUR:
					ec->data.spot.red = jxsml__f16(st);
					ec->data.spot.green = jxsml__f16(st);
					ec->data.spot.blue = jxsml__f16(st);
					ec->data.spot.solidity = jxsml__f16(st);
					break;
				case JXSML_EC_CFA:
					ec->data.cfa_channel = jxsml__u32(st, 1, 0, 0, 2, 3, 4, 19, 8);
					break;
				case JXSML_EC_DEPTH: case JXSML_EC_SELECTION_MASK: case JXSML_EC_BLACK:
				case JXSML_EC_THERMAL: case JXSML_EC_NON_OPTIONAL: case JXSML_EC_OPTIONAL:
					break;
				default: JXSML__RAISE("ect?");
				}
			}
			JXSML__RAISE_DELAYED();
		}
		st->xyb_encoded = jxsml__u(st, 1);
		if (!jxsml__u(st, 1)) { // ColourEncoding.all_default
			enum { CS_RGB = 0, CS_GREY = 1, CS_XYB = 2, CS_UNKNOWN = 3 } cspace;
			enum { WP_D65 = 1, WP_CUSTOM = 2, WP_E = 10, WP_DCI = 11 };
			enum { PR_SRGB = 1, PR_CUSTOM = 2, PR_2100 = 9, PR_P3 = 11 };
			st->want_icc = jxsml__u(st, 1);
			cspace = jxsml__enum(st);
			switch (cspace) {
			case CS_RGB: case CS_UNKNOWN: st->out->cspace = JXSML_CS_CHROMA; break;
			case CS_GREY: st->out->cspace = JXSML_CS_GREY; break;
			case CS_XYB: st->out->cspace = JXSML_CS_XYB; break;
			default: JXSML__RAISE("csp?");
			}
			// TODO: should verify cspace grayness with ICC grayness
			if (!st->want_icc) {
				if (cspace != CS_XYB) {
					static const float E[2] = {1/3.f, 1/3.f}, DCI[2] = {0.314f, 0.351f},
						BT2100[3][2] = {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}},
						P3[3][2] = {{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}};
					switch (jxsml__enum(st)) {
					case WP_D65: break; // default
					case WP_CUSTOM: JXSML__TRY(jxsml__customxy(st, st->out->cpoints[JXSML_CHROMA_WHITE])); break;
					case WP_E: memcpy(st->out->cpoints + JXSML_CHROMA_WHITE, E, sizeof E); break;
					case WP_DCI: memcpy(st->out->cpoints + JXSML_CHROMA_WHITE, DCI, sizeof DCI); break;
					default: JXSML__RAISE("wpt?");
					}
					if (cspace != CS_GREY) {
						switch (jxsml__enum(st)) {
						case PR_SRGB: break; // default
						case PR_CUSTOM:
							JXSML__TRY(jxsml__customxy(st, st->out->cpoints[JXSML_CHROMA_RED]));
							JXSML__TRY(jxsml__customxy(st, st->out->cpoints[JXSML_CHROMA_GREEN]));
							JXSML__TRY(jxsml__customxy(st, st->out->cpoints[JXSML_CHROMA_BLUE]));
							break;
						case PR_2100: memcpy(st->out->cpoints + JXSML_CHROMA_RED, BT2100, sizeof BT2100); break;
						case PR_P3: memcpy(st->out->cpoints + JXSML_CHROMA_RED, P3, sizeof P3); break;
						default: JXSML__RAISE("prm?");
						}
					}
				}
				if (jxsml__u(st, 1)) { // have_gamma
					st->out->gamma_or_tf = jxsml__u(st, 24);
					JXSML__SHOULD(st->out->gamma_or_tf > 0 && st->out->gamma_or_tf <= JXSML_GAMMA_MAX, "gama");
					if (cspace == CS_XYB) JXSML__SHOULD(st->out->gamma_or_tf == 3333333, "gama");
				} else {
					st->out->gamma_or_tf = -jxsml__enum(st);
					JXSML__SHOULD((
						1 << -JXSML_TF_709 | 1 << -JXSML_TF_UNKNOWN | 1 << -JXSML_TF_LINEAR |
						1 << -JXSML_TF_SRGB | 1 << -JXSML_TF_PQ | 1 << -JXSML_TF_DCI |
						1 << -JXSML_TF_HLG
					) >> -st->out->gamma_or_tf & 1, "tfn?");
				}
				st->out->render_intent = jxsml__enum(st);
				JXSML__SHOULD((
					1 << JXSML_INTENT_PERC | 1 << JXSML_INTENT_REL |
					1 << JXSML_INTENT_SAT | 1 << JXSML_INTENT_ABS
				) >> st->out->render_intent & 1, "itt?");
			}
		}
		if (extra_fields) {
			if (!jxsml__u(st, 1)) { // ToneMapping.all_default
				int relative_to_max_display;
				st->out->intensity_target = jxsml__f16(st);
				JXSML__SHOULD(st->out->intensity_target > 0, "tone");
				st->out->min_nits = jxsml__f16(st);
				JXSML__SHOULD(0 < st->out->min_nits && st->out->min_nits <= st->out->intensity_target, "tone");
				relative_to_max_display = jxsml__u(st, 1);
				st->out->linear_below = jxsml__f16(st);
				if (relative_to_max_display) {
					JXSML__SHOULD(0 <= st->out->linear_below && st->out->linear_below <= 1, "tone");
					st->out->linear_below *= -1.0f;
				} else {
					JXSML__SHOULD(0 <= st->out->linear_below, "tone");
				}
			}
		}
		JXSML__TRY(jxsml__extensions(st));
	}
	if (!jxsml__u(st, 1)) { // !default_m
		int32_t cw_mask;
		if (st->xyb_encoded) {
			for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) st->opsin_inv_mat[i][j] = jxsml__f16(st);
			for (i = 0; i < 3; ++i) st->opsin_bias[i] = jxsml__f16(st);
			for (i = 0; i < 3; ++i) st->quant_bias[i] = jxsml__f16(st);
			st->quant_bias_num = jxsml__f16(st);
		}
		cw_mask = jxsml__u(st, 3);
		if (cw_mask & 1) {
			JXSML__RAISE("TODO: up2_weight");
		}
		if (cw_mask & 2) {
			JXSML__RAISE("TODO: up4_weight");
		}
		if (cw_mask & 4) {
			JXSML__RAISE("TODO: up8_weight");
		}
	}
	JXSML__RAISE_DELAYED();
	return 0;

error:
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// ICC

JXSML_STATIC jxsml_err jxsml__icc(jxsml__st *st) {
	size_t enc_size, index;
	jxsml__code_spec_t codespec = {0};
	jxsml__code_t code = { .spec = &codespec };
	int32_t byte = 0, prev = 0, pprev = 0, ctx;

	enc_size = jxsml__u64(st);
	JXSML__TRY(jxsml__code_spec(st, 41, &codespec));

	for (index = 0; index < enc_size; ++index) {
		pprev = prev;
		prev = byte;
		ctx = 0;
		if (index > 128) {
			if (prev < 16) ctx = prev < 2 ? prev + 3 : 5;
			else if (prev > 240) ctx = 6 + (prev == 255);
			else if (97 <= (prev | 32) && (prev | 32) <= 122) ctx = 1;
			else if (prev == 44 || prev == 46 || (48 <= prev && prev < 58)) ctx = 2;
			else ctx = 8;
			if (pprev < 16) ctx += 2 * 8;
			else if (pprev > 240) ctx += 3 * 8;
			else if (97 <= (pprev | 32) && (pprev | 32) <= 122) ctx += 0 * 8;
			else if (pprev == 44 || pprev == 46 || (48 <= pprev && pprev < 58)) ctx += 1 * 8;
			else ctx += 4 * 8;
		}
		byte = jxsml__code(st, ctx, 0, &code);
		//fprintf(stderr, "%zd/%zd: %zd ctx=%d byte=%#x %c\n", index, enc_size, st->bits_read, ctx, (int)byte, 0x20 <= byte && byte < 0x7f ? byte : ' ');
		JXSML__RAISE_DELAYED();
		// TODO actually interpret them
	}
	JXSML__TRY(jxsml__finish_and_free_code(st, &code));
	jxsml__free_code_spec(&codespec);

	//size_t output_size = jxsml__varint(st);
	//size_t commands_size = jxsml__varint(st);

	/*
	static const char PREDICTIONS[] = {
		'*', '*', '*', '*', 0, 0, 0, 0, 4, 0, 0, 0, 'm', 'n', 't', 'r',
		'R', 'G', 'B', ' ', 'X', 'Y', 'Z', ' ', 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 'a', 'c', 's', 'p', 0, '@', '@', '@', 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 246, 214, 0, 1, 0, 0, 0, 0, 211, 45,
		'#', '#', '#', '#',
	};
	char pred = i < sizeof(PREDICTIONS) ? PREDICTIONS[i] : 0;
	switch (pred) {
	case '*': pred = output_size[i]; break;
	case '#': pred = header[i - 76]; break;
	case '@':
		switch (header[40]) {
		case 'A': pred = "APPL"[i - 40]; break;
		case 'M': pred = "MSFT"[i - 40]; break;
		case 'S':
			switch (i < 41 ? 0 : header[41]) {
			case 'G': pred = "SGI "[i - 40]; break;
			case 'U': pred = "SUNW"[i - 40]; break;
			}
			break;
		}
		break;
	}
	*/

	return 0;

error:
	jxsml__free_code(&code);
	jxsml__free_code_spec(&codespec);
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// MA tree

enum { JXSML__NUM_PRED = 14 };

typedef union {
	struct {
		int32_t prop; // < 0, ~prop is the property index (e.g. -1 = channel index)
		int32_t value;
		int32_t leftoff, rightoff; // relative to the current node
	} branch;
	struct {
		int32_t ctx; // >= 0
		int32_t predictor;
		int32_t offset, multiplier;
	} leaf;
} jxsml__tree_t;

JXSML_STATIC jxsml_err jxsml__tree(jxsml__st *st, jxsml__tree_t **tree, jxsml__code_spec_t *codespec) {
	jxsml__code_t code = { .spec = codespec };
	jxsml__tree_t *t = NULL;
	int32_t tree_idx = 0, tree_cap = 8;
	int32_t ctx_id = 0, nodes_left = 1;

	JXSML__TRY(jxsml__code_spec(st, 6, codespec));
	JXSML__SHOULD(t = malloc(sizeof(jxsml__tree_t) * (size_t) tree_cap), "!mem");
	while (nodes_left-- > 0) { // depth-first, left-to-right ordering
		jxsml__tree_t *n;
		int32_t prop = jxsml__code(st, 1, 0, &code), val, shift;
		JXSML__TRY_REALLOC(&t, sizeof(jxsml__tree_t), tree_idx + 1, &tree_cap);
		n = &t[tree_idx++];
		if (prop > 0) {
			n->branch.prop = -prop;
			n->branch.value = jxsml__to_signed(jxsml__code(st, 0, 0, &code));
			n->branch.leftoff = ++nodes_left;
			n->branch.rightoff = ++nodes_left;
		} else {
			n->leaf.ctx = ctx_id++;
			n->leaf.predictor = jxsml__code(st, 2, 0, &code);
			n->leaf.offset = jxsml__to_signed(jxsml__code(st, 3, 0, &code));
			shift = jxsml__code(st, 4, 0, &code);
			JXSML__SHOULD(shift < 31, "tree");
			val = jxsml__code(st, 5, 0, &code);
			JXSML__SHOULD(val < (1 << (31 - shift)) - 1, "tree");
			n->leaf.multiplier = (val + 1) << shift;
		}
		JXSML__SHOULD(tree_idx + nodes_left <= (1 << 26), "tree");
	}
	JXSML__TRY(jxsml__finish_and_free_code(st, &code));
	jxsml__free_code_spec(codespec);
	memset(codespec, 0, sizeof(*codespec)); // XXX is it required?
	JXSML__TRY(jxsml__code_spec(st, ctx_id, codespec));
	*tree = t;
	return 0;

error:
	free(t);
	jxsml__free_code(&code);
	jxsml__free_code_spec(codespec);
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// modular

typedef union {
	enum jxsml__transform_id {
		JXSML__TR_RCT = 0, JXSML__TR_PALETTE = 1, JXSML__TR_SQUEEZE = 2
	} tr;
	struct {
		enum jxsml__transform_id tr; // = JXSML__TR_RCT
		int32_t begin_c, type;
	} rct;
	struct {
		enum jxsml__transform_id tr; // = JXSML__TR_PALETTE
		int32_t begin_c, num_c, nb_colours, nb_deltas, d_pred;
	} pal;
	// this is nested in the bitstream, but flattened here.
	// nb_transforms get updated accordingly, but should be enough (the maximum is 80808)
	struct {
		enum jxsml__transform_id tr; // = JXSML__TR_SQUEEZE
		int implicit; // if true, no explicit parameters given in the bitstream
		int horizontal, in_place;
		int32_t begin_c, num_c;
	} sq;
} jxsml__transform_t;

typedef struct {
	int32_t width, height;
	int32_t hshift, vshift; // -1 if not applicable
	void *pixels; // either int16_t* or int32_t* depending on modular_16bit_buffers
} jxsml__modular_channel_t;

typedef struct { int8_t p1, p2, p3[5], w[4]; } jxsml__wp_params;

typedef struct {
	int use_global_tree;
	jxsml__wp_params wp;
	int32_t nb_transforms;
	jxsml__transform_t *transform;
	jxsml__tree_t *tree; // owned only if use_global_tree is false
	jxsml__code_spec_t codespec;
	jxsml__code_t code;
	int32_t num_channels, nb_meta_channels;
	jxsml__modular_channel_t *channel;
	int32_t max_width; // aka dist_multiplier, excludes meta channels
} jxsml__modular_t;

JXSML_STATIC void jxsml__free_modular(jxsml__modular_t *);

JXSML_STATIC void jxsml__init_modular_common(jxsml__modular_t *m) {
	m->transform = NULL;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(jxsml__code_spec_t));
	memset(&m->code, 0, sizeof(jxsml__code_t));
	m->code.spec = &m->codespec;
	m->channel = NULL;
}

JXSML_STATIC jxsml_err jxsml__init_modular(
	jxsml__st *st, int32_t num_channels, const int32_t *w, const int32_t *h, jxsml__modular_t *m
) {
	int32_t i;

	jxsml__init_modular_common(m);
	m->num_channels = num_channels;
	JXSML__ASSERT(num_channels > 0);
	JXSML__SHOULD(m->channel = calloc((size_t) num_channels, sizeof(jxsml__modular_channel_t)), "!mem");
	for (i = 0; i < num_channels; ++i) {
		m->channel[i].width = w[i];
		m->channel[i].height = h[i];
		m->channel[i].hshift = m->channel[i].vshift = 0;
	}
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__init_modular_for_global(
	jxsml__st *st, int frame_is_modular, int frame_do_ycbcr,
	int32_t frame_log_upsampling, const int32_t *frame_ec_log_upsampling,
	int32_t frame_width, int32_t frame_height, jxsml__modular_t *m
) {
	int32_t i;

	jxsml__init_modular_common(m);
	m->num_channels = st->num_extra_channels;
	if (frame_is_modular) {
		m->num_channels += (!frame_do_ycbcr && !st->xyb_encoded && st->out->cspace == JXSML_CS_GREY ? 1 : 3);
	}
	if (m->num_channels == 0) return 0;

	JXSML__SHOULD(m->channel = calloc((size_t) m->num_channels, sizeof(jxsml__modular_channel_t)), "!mem");
	for (i = 0; i < st->num_extra_channels; ++i) {
		int32_t log_upsampling = (frame_ec_log_upsampling ? frame_ec_log_upsampling[i] : 0) + st->ec_info[i].dim_shift;
		JXSML__SHOULD(log_upsampling >= frame_log_upsampling, "usmp");
		JXSML__SHOULD(log_upsampling == 0, "TODO: upsampling is not yet supported");
		m->channel[i].width = frame_width;
		m->channel[i].height = frame_height;
		m->channel[i].hshift = m->channel[i].vshift = 0;
	}
	for (; i < m->num_channels; ++i) {
		m->channel[i].width = frame_width;
		m->channel[i].height = frame_height;
		m->channel[i].hshift = m->channel[i].vshift = 0;
	}
	return 0;

error:
	free(m->channel);
	m->channel = NULL;
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__init_modular_for_pass_group(
	jxsml__st *st, int32_t num_gm_channels, int32_t gw, int32_t gh,
	int32_t minshift, int32_t maxshift, const jxsml__modular_t *gm, jxsml__modular_t *m
) {
	int32_t i, max_channels;

	jxsml__init_modular_common(m);
	m->num_channels = 0;
	max_channels = gm->num_channels - num_gm_channels;
	JXSML__ASSERT(max_channels >= 0);
	JXSML__SHOULD(m->channel = calloc((size_t) max_channels, sizeof(jxsml__modular_channel_t)), "!mem");
	for (i = num_gm_channels; i < gm->num_channels; ++i) {
		jxsml__modular_channel_t *gc = &gm->channel[i], *c = &m->channel[m->num_channels];
		if (gc->hshift < 3 || gc->vshift < 3) {
			JXSML__ASSERT(gc->hshift >= 0 && gc->vshift >= 0);
			(void) minshift; (void) maxshift;
			// TODO check minshift/maxshift!!!
			c->hshift = gc->hshift;
			c->vshift = gc->vshift;
			c->width = gw >> gc->hshift; // TODO is this correct? should be ceil?
			c->height = gh >> gc->vshift;
			++m->num_channels;
		}
	}
	if (m->num_channels == 0) {
		free(m->channel);
		m->channel = NULL;
	}
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__combine_modular_from_pass_group(
	jxsml__st *st, int32_t num_gm_channels, int32_t gy, int32_t gx,
	int32_t minshift, int32_t maxshift, const jxsml__modular_t *gm, jxsml__modular_t *m
) {
	size_t pixel_size = st->modular_16bit_buffers ? sizeof(int16_t) : sizeof(int32_t);
	int32_t gcidx, cidx, y, gx0, gy0;
	for (gcidx = num_gm_channels, cidx = 0; gcidx < gm->num_channels; ++gcidx) {
		jxsml__modular_channel_t *gc = &gm->channel[gcidx], *c = &m->channel[cidx];
		if (gc->hshift < 3 || gc->vshift < 3) {
			(void) minshift; (void) maxshift;
			// TODO check minshift/maxshift!!!
			JXSML__ASSERT(gc->hshift == c->hshift && gc->vshift == c->vshift);
			gx0 = gx >> gc->hshift;
			gy0 = gy >> gc->vshift;
			JXSML__ASSERT(gx0 + c->width <= gc->width && gy0 + c->height <= gc->height);
			for (y = 0; y < c->height; ++y) {
				memcpy(
					(char*) gc->pixels + pixel_size * (size_t) ((gy0 + y) * gc->width + gx0),
					(char*) c->pixels + pixel_size * (size_t) (y * c->width),
					pixel_size * (size_t) c->width);
			}
			fprintf(stderr, "combined channel %d with w=%d h=%d to channel %d with w=%d h=%d gx0=%d gy0=%d\n", cidx, c->width, c->height, gcidx, gc->width, gc->height, gx0, gy0);
			++cidx;
		}
	}
	JXSML__ASSERT(cidx == m->num_channels);
error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__is_modular_channel_equal_sized(
	jxsml__st *st, const jxsml__modular_channel_t *begin, const jxsml__modular_channel_t *end
) {
	jxsml__modular_channel_t c;
	if (begin >= end) return 0; // do not allow edge cases
	c = *begin;
	while (++begin < end) {
		if (c.width != begin->width || c.height != begin->height) return 0;
#ifdef JXSML_DEBUG
		if (c.vshift >= 0 && c.hshift >= 0 && begin->vshift >= 0 && begin->hshift >= 0 &&
			(c.vshift != begin->vshift || c.hshift != begin->hshift)
		) {
			return JXSML__ERR("!exp"), 0;
		}
#endif
	}
	return 1;
}

JXSML_STATIC jxsml_err jxsml__modular_header(
	jxsml__st *st, jxsml__tree_t *global_tree, const jxsml__code_spec_t *global_codespec,
	jxsml__modular_t *m
) {
	jxsml__modular_channel_t *channel = m->channel;
	int32_t num_channels = m->num_channels, nb_meta_channels = 0;
	// note: channel_cap is the upper bound of # channels during inverse transform, and since
	// we don't shrink the channel list we don't ever need reallocation in jxsml__inverse_transform!
	int32_t channel_cap = m->num_channels, transform_cap;
	int32_t i, j;

	JXSML__ASSERT(num_channels > 0);

	m->use_global_tree = jxsml__u(st, 1);
	JXSML__SHOULD(!m->use_global_tree || global_tree, "mtre");

	{ // WPHeader
		int default_wp = jxsml__u(st, 1);
		m->wp.p1 = default_wp ? 16 : (int8_t) jxsml__u(st, 5);
		m->wp.p2 = default_wp ? 10 : (int8_t) jxsml__u(st, 5);
		for (i = 0; i < 5; ++i) m->wp.p3[i] = default_wp ? 7 * (i < 3) : (int8_t) jxsml__u(st, 5);
		for (i = 0; i < 4; ++i) m->wp.w[i] = default_wp ? 12 + (i < 1) : (int8_t) jxsml__u(st, 4);
	}

	transform_cap = m->nb_transforms = jxsml__u32(st, 0, 0, 1, 0, 2, 4, 18, 8);
	JXSML__SHOULD(m->transform = malloc(sizeof(jxsml__transform_t) * (size_t) transform_cap), "!mem");
	for (i = 0; i < m->nb_transforms; ++i) {
		jxsml__transform_t *tr = &m->transform[i];
		int32_t num_sq;

		tr->tr = (enum jxsml__transform_id) jxsml__u(st, 2);
		switch (tr->tr) {
		// RCT: [begin_c, begin_c+3) -> [begin_c, begin_c+3)
		case JXSML__TR_RCT: {
			int32_t begin_c = tr->rct.begin_c = jxsml__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
			int32_t type = tr->rct.type = jxsml__u32(st, 6, 0, 0, 2, 2, 4, 10, 6);
			JXSML__SHOULD(type < 42, "rctt");
			JXSML__SHOULD(begin_c + 3 <= num_channels, "rctc");
			JXSML__SHOULD(begin_c >= nb_meta_channels || begin_c + 3 <= nb_meta_channels, "rctc");
			JXSML__SHOULD(jxsml__is_modular_channel_equal_sized(st, channel + begin_c, channel + begin_c + 3), "rtcd");
			fprintf(stderr, "transform %d: rct type %d [%d,%d)\n", i, type, begin_c, begin_c + 3);
			break;
		}

		// Palette: [begin_c, end_c) -> palette 0 (meta, nb_colours by num_c) + index begin_c+1
		case JXSML__TR_PALETTE: {
			jxsml__modular_channel_t input;
			int32_t begin_c = tr->pal.begin_c = jxsml__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
			int32_t num_c = tr->pal.num_c = jxsml__u32(st, 1, 0, 3, 0, 4, 0, 1, 13);
			int32_t end_c = begin_c + num_c;
			int32_t nb_colours = tr->pal.nb_colours = jxsml__u32(st, 0, 8, 256, 10, 1280, 12, 5376, 16);
			tr->pal.nb_deltas = jxsml__u32(st, 0, 0, 1, 8, 257, 10, 1281, 16);
			tr->pal.d_pred = jxsml__u(st, 4);
			JXSML__SHOULD(tr->pal.d_pred < JXSML__NUM_PRED, "palp");
			JXSML__SHOULD(end_c <= num_channels, "palc");
			if (begin_c < nb_meta_channels) { // num_c meta channels -> 2 meta channels (palette + index)
				JXSML__SHOULD(end_c <= nb_meta_channels, "palc");
				nb_meta_channels += 2 - num_c;
			} else { // num_c color channels -> 1 meta channel (palette) + 1 color channel (index)
				nb_meta_channels += 1;
			}
			JXSML__SHOULD(jxsml__is_modular_channel_equal_sized(st, channel + begin_c, channel + end_c), "rtcd");
			// inverse palette transform always requires one more channel slot
			JXSML__TRY_REALLOC(&channel, sizeof(*channel), num_channels + 1, &channel_cap);
			input = channel[begin_c];
			memmove(channel + 1, channel, sizeof(*channel) * (size_t) begin_c);
			memmove(channel + begin_c + 2, channel + end_c, sizeof(*channel) * (size_t) (num_channels - end_c));
			channel[0].width = nb_colours;
			channel[0].height = num_c;
			channel[0].hshift = 0; // !!!
			channel[0].vshift = -1;
			channel[begin_c + 1] = input;
			num_channels += 2 - num_c;
			fprintf(stderr, "transform %d: palette [%d,%d) c%d d%d p%d\n", i, begin_c, end_c, nb_colours, tr->pal.nb_deltas, tr->pal.d_pred);
			break;
		}

		// Squeeze: 
		case JXSML__TR_SQUEEZE: {
			num_sq = jxsml__u32(st, 0, 0, 1, 4, 9, 6, 41, 8);
			if (num_sq == 0) {
				tr->sq.implicit = 1;
			} else {
				JXSML__TRY_REALLOC(&m->transform, sizeof(jxsml__transform_t),
					m->nb_transforms + num_sq - 1, &transform_cap);
				for (j = 0; j < num_sq; ++j) {
					tr = &m->transform[i + j];
					tr->sq.tr = JXSML__TR_SQUEEZE;
					tr->sq.implicit = 0;
					tr->sq.horizontal = jxsml__u(st, 1);
					tr->sq.in_place = jxsml__u(st, 1);
					tr->sq.begin_c = jxsml__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
					tr->sq.num_c = jxsml__u32(st, 1, 0, 2, 0, 3, 0, 4, 4);
				}
				i += num_sq - 1;
				m->nb_transforms += num_sq - 1;
			}
			JXSML__RAISE("TODO: squeeze channel effects");
			break;
		}

		default: JXSML__RAISE("xfm?");
		}
		JXSML__RAISE_DELAYED();
	}

	if (m->use_global_tree) {
		m->tree = global_tree;
		memcpy(&m->codespec, global_codespec, sizeof(jxsml__code_spec_t));
	} else {
		JXSML__TRY(jxsml__tree(st, &m->tree, &m->codespec));
	}

	m->channel = channel;
	m->num_channels = num_channels;
	m->nb_meta_channels = nb_meta_channels;
	m->max_width = 0;
	for (i = 0; i < num_channels; ++i) {
		channel[i].pixels = NULL;
		if (i >= nb_meta_channels) m->max_width = jxsml__max32(m->max_width, channel[i].width);
	}
	return 0;

error:
	free(channel);
	free(m->transform);
	if (!m->use_global_tree) {
		free(m->tree);
		jxsml__free_code_spec(&m->codespec);
	}
	m->num_channels = 0;
	m->channel = NULL;
	m->transform = NULL;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(jxsml__code_spec_t));
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__allocate_modular(jxsml__st *st, jxsml__modular_t *m) {
	size_t pixel_size = st->modular_16bit_buffers ? sizeof(int16_t) : sizeof(int32_t);
	int32_t i;
	for (i = 0; i < m->num_channels; ++i) {
		jxsml__modular_channel_t *c = &m->channel[i];
		JXSML__SHOULD(c->pixels = malloc(pixel_size * (size_t) (c->width * c->height)), "!mem");
	}
error:
	return st->err;
}

JXSML_STATIC void jxsml__free_modular(jxsml__modular_t *m) {
	int32_t i;
	jxsml__free_code(&m->code);
	if (!m->use_global_tree) {
		free(m->tree);
		jxsml__free_code_spec(&m->codespec);
	}
	for (i = 0; i < m->num_channels; ++i) {
		free(m->channel[i].pixels);
	}
	free(m->transform);
	free(m->channel);
	m->use_global_tree = 0;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(jxsml__code_spec_t));
	m->transform = NULL;
	m->num_channels = 0;
	m->channel = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// modular prediction

static const int32_t JXSML__24DIVP1[64] = { // [i] = floor(2^24 / (i+1))
	0x1000000, 0x800000, 0x555555, 0x400000, 0x333333, 0x2aaaaa, 0x249249, 0x200000,
	0x1c71c7, 0x199999, 0x1745d1, 0x155555, 0x13b13b, 0x124924, 0x111111, 0x100000,
	0xf0f0f, 0xe38e3, 0xd7943, 0xccccc, 0xc30c3, 0xba2e8, 0xb2164, 0xaaaaa,
	0xa3d70, 0x9d89d, 0x97b42, 0x92492, 0x8d3dc, 0x88888, 0x84210, 0x80000,
	0x7c1f0, 0x78787, 0x75075, 0x71c71, 0x6eb3e, 0x6bca1, 0x69069, 0x66666,
	0x63e70, 0x61861, 0x5f417, 0x5d174, 0x5b05b, 0x590b2, 0x57262, 0x55555,
	0x53978, 0x51eb8, 0x50505, 0x4ec4e, 0x4d487, 0x4bda1, 0x4a790, 0x49249,
	0x47dc1, 0x469ee, 0x456c7, 0x44444, 0x4325c, 0x42108, 0x41041, 0x40000,
};

// ----------------------------------------
// recursion for modular buffer sizes (16/32)
#define JXSML__RECURSING 200
#define JXSML__P 16
#define JXSML__2P 32
#include __FILE__
#define JXSML__P 32
#define JXSML__2P 64
#include __FILE__
#undef JXSML__RECURSING

#endif // !JXSML__RECURSING
#if JXSML__RECURSING+0 == 200
	#define jxsml__intP_t JXSML__CONCAT3(int, JXSML__P, _t)
	#define jxsml__int2P_t JXSML__CONCAT3(int, JXSML__2P, _t)
	#define jxsml__uint2P_t JXSML__CONCAT3(uint, JXSML__2P, _t)
// ----------------------------------------

typedef struct {
	int32_t width;
	jxsml__wp_params params;
	jxsml__int2P_t (*errors)[5], pred[5]; // [0..3] = sub-predictions, [4] = final prediction
	jxsml__int2P_t trueerrw, trueerrn, trueerrnw, trueerrne;
} jxsml__(wp,2P);

typedef struct { jxsml__intP_t w, n, nw, ne, nn, nee, ww, nww; } jxsml__(neighbors_t,P);
JXSML_ALWAYS_INLINE jxsml__(neighbors_t,P) jxsml__(neighbors,P)(
	const jxsml__intP_t *pixels, int32_t x, int32_t y, int32_t width
) {
	jxsml__(neighbors_t,P) p;
	pixels += width * y;

	/*            NN
	 *             |
	 *             v
	 * NWW  NW   _ N <- NE <- NEE
	 *  |    |   /|
	 *  v    v |/ 
	 * WW -> W  `  C
	 *
	 * A -> B means that if A doesn't exist B is used instead.
	 * if the pixel at the end of this chain doesn't exist as well, 0 is used.
	 */
	p.w = x > 0 ? pixels[x - 1] : y > 0 ? pixels[x - width] : 0;
	p.n = y > 0 ? pixels[x - width] : p.w;
	p.nw = x > 0 && y > 0 ? pixels[(x - 1) - width] : p.w;
	p.ne = x + 1 < width && y > 0 ? pixels[(x + 1) - width] : p.n;
	p.nn = y > 1 ? pixels[x - 2 * width] : p.n;
	p.nee = x + 2 < width && y > 0 ? pixels[(x + 2) - width] : p.ne;
	p.ww = x > 1 ? pixels[x - 2] : p.w;
	p.nww = x > 1 && y > 0 ? pixels[(x - 2) - width] : p.ww;
	return p;
}

JXSML_INLINE jxsml__int2P_t jxsml__(gradient,2P)(jxsml__int2P_t w, jxsml__int2P_t n, jxsml__int2P_t nw) {
	jxsml__int2P_t lo = jxsml__(min,2P)(w, n), hi = jxsml__(max,2P)(w, n);
	return jxsml__(min,2P)(jxsml__(max,2P)(lo, w + n - nw), hi);
}

JXSML_STATIC jxsml_err jxsml__(init_wp,2P)(jxsml__st *st, jxsml__wp_params params, int32_t width, jxsml__(wp,2P) *wp) {
	int32_t i;
	JXSML__ASSERT(width > 0);
	wp->width = width;
	wp->params = params;
	JXSML__SHOULD(wp->errors = calloc((size_t) width * 2, sizeof(jxsml__int2P_t[5])), "!mem");
	for (i = 0; i < 5; ++i) wp->pred[i] = 0;
	wp->trueerrw = wp->trueerrn = wp->trueerrnw = wp->trueerrne = 0;
error:
	return st->err;
}

// also works when wp is zero-initialized (in which case does nothing)
JXSML_INLINE void jxsml__(wp_before_predict,2P)(jxsml__(wp,2P) *wp, int32_t x, int32_t y, const jxsml__(neighbors_t,P) *p) {
	typedef jxsml__int2P_t int2P_t;
	typedef jxsml__uint2P_t uint2P_t;

	static const int2P_t ZERO[4] = {0, 0, 0, 0};

	int2P_t (*err)[5], (*nerr)[5];
	int2P_t w[4], wsum, sum;
	int32_t logw, i;
	const int2P_t *errw, *errn, *errnw, *errne, *errww, *errw2;

	if (!wp->errors) return;

	err = wp->errors + (y & 1 ? wp->width : 0);
	nerr = wp->errors + (y & 1 ? 0 : wp->width);

	errw = x > 0 ? err[x - 1] : ZERO;
	errn = y > 0 ? nerr[x] : ZERO;
	errnw = x > 0 && y > 0 ? nerr[x - 1] : errn;
	errne = x + 1 < wp->width && y > 0 ? nerr[x + 1] : errn;
	errww = x > 1 ? err[x - 2] : ZERO;
	errw2 = x + 1 < wp->width ? ZERO : errw; // what?

	wp->trueerrw = x > 0 ? err[x - 1][4] : 0;
	wp->trueerrn = y > 0 ? nerr[x][4] : 0;
	wp->trueerrnw = x > 0 && y > 0 ? nerr[x - 1][4] : wp->trueerrn;
	wp->trueerrne = x + 1 < wp->width && y > 0 ? nerr[x + 1][4] : wp->trueerrn;

	wp->pred[0] = (p->w + p->ne - p->n) << 3;
	wp->pred[1] = (p->n << 3) - (((wp->trueerrw + wp->trueerrn + wp->trueerrne) * wp->params.p1) >> 5);
	wp->pred[2] = (p->w << 3) - (((wp->trueerrw + wp->trueerrn + wp->trueerrnw) * wp->params.p2) >> 5);
	wp->pred[3] = (p->n << 3) -
		((wp->trueerrnw * wp->params.p3[0] + wp->trueerrn * wp->params.p3[1] +
		  wp->trueerrne * wp->params.p3[2] + ((p->nn - p->n) << 3) * wp->params.p3[3] +
		  ((p->nw - p->w) << 3) * wp->params.p3[4]) >> 5);
	for (i = 0; i < 4; ++i) {
		int2P_t errsum = errn[i] + errw[i] + errnw[i] + errww[i] + errne[i] + errw2[i];
		int32_t shift = jxsml__max32(jxsml__(floor_lg,2P)((uint2P_t) errsum + 1) - 5, 0);
		w[i] = (int2P_t) (4 + ((int64_t) wp->params.w[i] * JXSML__24DIVP1[errsum >> shift] >> shift));
	}
	logw = jxsml__(floor_lg,2P)((uint2P_t) (w[0] + w[1] + w[2] + w[3])) - 4;
	wsum = sum = 0;
	for (i = 0; i < 4; ++i) {
		wsum += w[i] >>= logw;
		sum += wp->pred[i] * w[i];
	}
	wp->pred[4] = (int2P_t) (((int64_t) sum + (wsum >> 1) - 1) * JXSML__24DIVP1[wsum - 1] >> 24);
	if (((wp->trueerrn ^ wp->trueerrw) | (wp->trueerrn ^ wp->trueerrnw)) <= 0) {
		int2P_t lo = jxsml__(min,2P)(p->w, jxsml__(min,2P)(p->n, p->ne)) << 3;
		int2P_t hi = jxsml__(max,2P)(p->w, jxsml__(max,2P)(p->n, p->ne)) << 3;
		wp->pred[4] = jxsml__(min,2P)(jxsml__(max,2P)(lo, wp->pred[4]), hi);
	}
}

JXSML_INLINE jxsml__int2P_t jxsml__(predict,2P)(
	jxsml__st *st, int32_t pred, const jxsml__(wp,2P) *wp, const jxsml__(neighbors_t,P) *p
) {
	switch (pred) {
	case 0: return 0;
	case 1: return p->w;
	case 2: return p->n;
	case 3: return (p->w + p->n) / 2;
	case 4: return jxsml__(abs,2P)(p->n - p->nw) < jxsml__(abs,2P)(p->w - p->nw) ? p->w : p->n;
	case 5: return jxsml__(gradient,2P)(p->w, p->n, p->nw);
	case 6: return (wp->pred[4] + 3) >> 3;
	case 7: return p->ne;
	case 8: return p->nw;
	case 9: return p->ww;
	case 10: return (p->w + p->nw) / 2;
	case 11: return (p->n + p->nw) / 2;
	case 12: return (p->n + p->ne) / 2;
	case 13: return (6 * p->n - 2 * p->nn + 7 * p->w + p->ww + p->nee + 3 * p->ne + 8) / 16;
	default: return JXSML__ERR("pred"), 0;
	}
}

// also works when wp is zero-initialized (in which case does nothing)
JXSML_STATIC void jxsml__(wp_after_predict,2P)(jxsml__(wp,2P) *wp, int32_t x, int32_t y, jxsml__int2P_t val) {
	if (wp->errors) {
		jxsml__int2P_t *err = wp->errors[(y & 1 ? wp->width : 0) + x];
		int32_t i;
		for (i = 0; i < 4; ++i) err[i] = (jxsml__(abs,2P)(wp->pred[i] - (val << 3)) + 3) >> 3;
		err[4] = wp->pred[4] - (val << 3);
	}
}

// also works when wp is zero-initialized (in which case does nothing)
JXSML_STATIC void jxsml__(reset_wp,2P)(jxsml__(wp,2P) *wp) {
	int32_t i;
	if (wp->errors) memset(wp->errors, 0, (size_t) wp->width * 2 * sizeof(jxsml__int2P_t[5]));
	for (i = 0; i < 5; ++i) wp->pred[i] = 0;
	wp->trueerrw = wp->trueerrn = wp->trueerrnw = wp->trueerrne = 0;
}

JXSML_STATIC void jxsml__(free_wp,2P)(jxsml__(wp,2P) *wp) {
	free(wp->errors);
	wp->errors = NULL;
	wp->width = 0;
}

JXSML_STATIC jxsml_err jxsml__(modular_channel,P)(
	jxsml__st *st, jxsml__modular_t *m, int32_t cidx, int32_t sidx
) {
	typedef jxsml__intP_t intP_t;
	typedef jxsml__int2P_t int2P_t;

	jxsml__modular_channel_t *c = &m->channel[cidx];
	int32_t width = c->width, height = c->height;
	int32_t y, x, i;
	int32_t nrefcmap, *refcmap = NULL; // refcmap[i] is a channel index for properties (16..19)+4*i
	jxsml__(wp,2P) wp = {0};

	JXSML__ASSERT(m->tree); // caller should set this to the global tree if not given
	JXSML__ASSERT(c->pixels);

	{ // determine whether to use weighted predictor (expensive)
		int32_t lasttree = 0, use_wp = 0;
		for (i = 0; i <= lasttree && !use_wp; ++i) {
			if (m->tree[i].branch.prop < 0) {
				use_wp |= ~m->tree[i].branch.prop == 15;
				lasttree = jxsml__max32(lasttree,
					i + jxsml__max32(m->tree[i].branch.leftoff, m->tree[i].branch.rightoff));
			} else {
				use_wp |= m->tree[i].leaf.predictor == 6;
			}
		}
		if (use_wp) JXSML__TRY(jxsml__(init_wp,2P)(st, m->wp, width, &wp));
	}

	JXSML__SHOULD(refcmap = malloc(sizeof(int32_t) * (size_t) cidx), "!mem");
	nrefcmap = 0;
	for (i = cidx - 1; i >= 0; --i) {
		jxsml__modular_channel_t *refc = &m->channel[i];
		if (c->width != refc->width || c->height != refc->height) continue;
		if (c->hshift != refc->hshift || c->vshift != refc->vshift) continue;
		refcmap[nrefcmap++] = i;
	}

	for (y = 0; y < height; ++y) {
		intP_t *outpixels = (intP_t*) c->pixels + y * c->width;
		for (x = 0; x < width; ++x) {
			jxsml__tree_t *n = m->tree;
			jxsml__(neighbors_t,P) p = jxsml__(neighbors,P)(c->pixels, x, y, c->width);
			int2P_t val;

			// wp should be calculated before any property testing due to max_error (property 15)
			jxsml__(wp_before_predict,2P)(&wp, x, y, &p);

			while (n->branch.prop < 0) {
				int32_t refcidx;
				jxsml__modular_channel_t *refc;
				intP_t *refpixels;

				switch (~n->branch.prop) {
				case 0: val = cidx; break;
				case 1: val = sidx; break;
				case 2: val = y; break;
				case 3: val = x; break;
				case 4: val = jxsml__(abs,2P)(p.n); break;
				case 5: val = jxsml__(abs,2P)(p.w); break;
				case 6: val = p.n; break;
				case 7: val = p.w; break;
				case 8: val = x > 0 ? p.w - (p.ww + p.nw - p.nww) : p.w; break;
				case 9: val = p.w + p.n - p.nw; break;
				case 10: val = p.w - p.nw; break;
				case 11: val = p.nw - p.n; break;
				case 12: val = p.n - p.ne; break;
				case 13: val = p.n - p.nn; break;
				case 14: val = p.w - p.ww; break;
				case 15: // requires use_wp; otherwise will be 0
					val = wp.trueerrw;
					if (jxsml__(abs,2P)(val) < jxsml__(abs,2P)(wp.trueerrn)) val = wp.trueerrn;
					if (jxsml__(abs,2P)(val) < jxsml__(abs,2P)(wp.trueerrnw)) val = wp.trueerrnw;
					if (jxsml__(abs,2P)(val) < jxsml__(abs,2P)(wp.trueerrne)) val = wp.trueerrne;
					break;
				default:
					refcidx = (~n->branch.prop - 16) / 4;
					JXSML__SHOULD(refcidx < nrefcmap, "trec");
					refc = &m->channel[refcmap[refcidx]];
					refpixels = refc->pixels;
					JXSML__ASSERT(c->width == refc->width && c->height == refc->height);
					val = refpixels[y * refc->width + x]; // rC
					if (~n->branch.prop & 2) {
						int2P_t rw = x > 0 ? refpixels[y * refc->width + (x - 1)] : 0;
						int2P_t rn = y > 0 ? refpixels[(y - 1) * refc->width + x] : rw;
						int2P_t rnw = x > 0 && y > 0 ? refpixels[(y - 1) * refc->width + (x - 1)] : rw;
						val -= jxsml__(gradient,2P)(rw, rn, rnw);
					}
					if (~n->branch.prop & 1) val = jxsml__(abs,2P)(val);
					break;
				}
				n += val > n->branch.value ? n->branch.leftoff : n->branch.rightoff;
			}

			val = jxsml__code(st, n->leaf.ctx, m->max_width, &m->code);
			//printf("%d ", val);
			val = jxsml__to_signed((int32_t) val) * n->leaf.multiplier + n->leaf.offset;
			val += jxsml__(predict,2P)(st, n->leaf.predictor, &wp, &p);
			JXSML__SHOULD(INT16_MIN <= val && val <= INT16_MAX, "povf");
			outpixels[x] = (intP_t) val;
			jxsml__(wp_after_predict,2P)(&wp, x, y, val);
		}
			//printf("\n");
	}
			//printf("--\n"); fflush(stdout);

	jxsml__(free_wp,2P)(&wp);
	free(refcmap);
	return 0;

error:
	jxsml__(free_wp,2P)(&wp);
	free(refcmap);
	free(c->pixels);
	c->pixels = NULL;
	return st->err;
}

// ----------------------------------------
// end of recursion
	#undef jxsml__intP_t
	#undef jxsml__int2P_t
	#undef jxsml__uint2P_t
	#undef JXSML__P
	#undef JXSML__2P
#endif // JXSML__RECURSING == 200
#ifndef JXSML__RECURSING
// ----------------------------------------

JXSML_STATIC jxsml_err jxsml__modular_channel(jxsml__st *st, jxsml__modular_t *m, int32_t cidx, int32_t sidx) {
	if (st->modular_16bit_buffers) {
		return jxsml__modular_channel16(st, m, cidx, sidx);
	} else {
		return jxsml__modular_channel32(st, m, cidx, sidx);
	}
}

////////////////////////////////////////////////////////////////////////////////
// modular (inverse) transform

#define JXSML__X(x,y,z) {x,y,z}, {-(x),-(y),-(z)}
#define JXSML__XX(a,b,c,d,e,f) JXSML__X a, JXSML__X b, JXSML__X c, JXSML__X d, JXSML__X e, JXSML__X f
static const int16_t JXSML__PALETTE_DELTAS[144][3] = { // the first entry is a duplicate and skipped
	JXSML__XX((0, 0, 0), (4, 4, 4), (11, 0, 0), (0, 0, -13), (0, -12, 0), (-10, -10, -10)),
	JXSML__XX((-18, -18, -18), (-27, -27, -27), (-18, -18, 0), (0, 0, -32), (-32, 0, 0), (-37, -37, -37)),
	JXSML__XX((0, -32, -32), (24, 24, 45), (50, 50, 50), (-45, -24, -24), (-24, -45, -45), (0, -24, -24)),
	JXSML__XX((-34, -34, 0), (-24, 0, -24), (-45, -45, -24), (64, 64, 64), (-32, 0, -32), (0, -32, 0)),
	JXSML__XX((-32, 0, 32), (-24, -45, -24), (45, 24, 45), (24, -24, -45), (-45, -24, 24), (80, 80, 80)),
	JXSML__XX((64, 0, 0), (0, 0, -64), (0, -64, -64), (-24, -24, 45), (96, 96, 96), (64, 64, 0)),
	JXSML__XX((45, -24, -24), (34, -34, 0), (112, 112, 112), (24, -45, -45), (45, 45, -24), (0, -32, 32)),
	JXSML__XX((24, -24, 45), (0, 96, 96), (45, -24, 24), (24, -45, -24), (-24, -45, 24), (0, -64, 0)),
	JXSML__XX((96, 0, 0), (128, 128, 128), (64, 0, 64), (144, 144, 144), (96, 96, 0), (-36, -36, 36)),
	JXSML__XX((45, -24, -45), (45, -45, -24), (0, 0, -96), (0, 128, 128), (0, 96, 0), (45, 24, -45)),
	JXSML__XX((-128, 0, 0), (24, -45, 24), (-45, 24, -45), (64, 0, -64), (64, -64, -64), (96, 0, 96)),
	JXSML__XX((45, -45, 24), (24, 45, -45), (64, 64, -64), (128, 128, 0), (0, 0, -128), (-24, 45, -45)),
};
#undef JXSML__X
#undef JXSML__XX

// ----------------------------------------
// recursion for modular inverse transform
#define JXSML__RECURSING 300
#define JXSML__P 16
#define JXSML__2P 32
#include __FILE__
#define JXSML__P 32
#define JXSML__2P 64
#include __FILE__
#undef JXSML__RECURSING

#endif // !JXSML__RECURSING
#if JXSML__RECURSING+0 == 300
	#define jxsml__intP_t JXSML__CONCAT3(int, JXSML__P, _t)
	#define jxsml__int2P_t JXSML__CONCAT3(int, JXSML__2P, _t)
// ----------------------------------------

JXSML_STATIC jxsml_err jxsml__(inverse_rct,P)(
	jxsml__st *st, jxsml__modular_t *m, const jxsml__transform_t *tr
) {
	typedef jxsml__intP_t intP_t;
	typedef jxsml__int2P_t int2P_t;

	static const uint8_t PERMUTATIONS[6][3] = {{0,1,2},{1,2,0},{2,0,1},{0,2,1},{1,0,2},{2,1,0}};

	jxsml__modular_channel_t *channel = m->channel + tr->rct.begin_c;
	intP_t *p[3];
	int32_t npixels, i;

	JXSML__ASSERT(tr->tr == JXSML__TR_RCT);
	JXSML__ASSERT(jxsml__is_modular_channel_equal_sized(st, channel, channel + 3));

	npixels = channel->width * channel->height;
	for (i = 0; i < 3; ++i) p[i] = channel[i].pixels;

	// TODO detect overflow
	switch (tr->rct.type % 7) {
	case 0: break;
	case 1:
		for (i = 0; i < npixels; ++i) p[2][i] = (intP_t) (p[2][i] + p[0][i]);
		break;
	case 2:
		for (i = 0; i < npixels; ++i) p[1][i] = (intP_t) (p[1][i] + p[0][i]);
		break;
	case 3:
		for (i = 0; i < npixels; ++i) {
			p[1][i] = (intP_t) (p[1][i] + p[0][i]);
			p[2][i] = (intP_t) (p[2][i] + p[0][i]);
		}
		break;
	case 4:
		for (i = 0; i < npixels; ++i) {
			p[1][i] = (intP_t) (p[1][i] + jxsml__(floor_avg,P)(p[0][i], p[2][i]));
		}
		break;
	case 5:
		for (i = 0; i < npixels; ++i) { // TODO avoid int2P_t if possible
			p[1][i] = (intP_t) ((int2P_t) p[1][i] + p[0][i] + (p[2][i] >> 1));
			p[2][i] = (intP_t) (p[2][i] + p[0][i]);
		}
		break;
	case 6: // YCgCo
		for (i = 0; i < npixels; ++i) { // TODO avoid int2P_t if possible
			int2P_t tmp = (int2P_t) p[0][i] - ((int2P_t) p[2][i] >> 1);
			int2P_t p1 = (int2P_t) p[2][i] + tmp;
			int2P_t p2 = tmp - ((int2P_t) p[1][i] >> 1);
			p[0][i] = (intP_t) (p2 + p[1][i]);
			p[1][i] = (intP_t) p1;
			p[2][i] = (intP_t) p2;
		}
		break;
	default: JXSML__UNREACHABLE();
	}

	for (i = 0; i < 3; ++i) {
		channel[PERMUTATIONS[tr->rct.type / 7][i]].pixels = p[i];
	}
	return 0;

error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__(inverse_palette,P)(
	jxsml__st *st, jxsml__modular_t *m, const jxsml__transform_t *tr
) {
	typedef jxsml__intP_t intP_t;
	typedef jxsml__int2P_t int2P_t;

	// `first` is the index channel index; restored color channels will be at indices [first,last],
	// where the original index channel is relocated to the index `last` and then repurposed.
	// the palette meta channel 0 will be removed at the very end.
	int32_t first = tr->pal.begin_c + 1, last = tr->pal.begin_c + tr->pal.num_c, bpp = st->out->bpp;
	int32_t i, j, y, x;
	jxsml__modular_channel_t *idxc = &m->channel[last];
	int32_t width = idxc->width, height = idxc->height;
	int use_pred = tr->pal.nb_deltas > 0, use_wp = use_pred && tr->pal.d_pred == 6;
	jxsml__(wp,2P) wp = {0};

	JXSML__ASSERT(tr->tr == JXSML__TR_PALETTE);

	memmove(m->channel + last, m->channel + first, sizeof(jxsml__modular_channel_t) * (size_t) (m->num_channels - first));
	m->num_channels += last - first;

	for (i = first; i < last; ++i) m->channel[i].pixels = NULL;
	for (i = first; i < last; ++i) {
		jxsml__modular_channel_t *c = &m->channel[i];
		c->width = width;
		c->height = height;
		JXSML__SHOULD(c->pixels = malloc(sizeof(intP_t) * (size_t) (width * height)), "!mem");
	}

	if (use_wp) JXSML__TRY(jxsml__(init_wp,2P)(st, m->wp, width, &wp));

	for (i = 0; i < tr->pal.num_c; ++i) {
		intP_t *palp = (intP_t*) m->channel[0].pixels + i * tr->pal.nb_colours;
		intP_t *pixels = m->channel[first + i].pixels;
		for (y = 0; y < height; ++y) {
			intP_t *idxline = (intP_t*) idxc->pixels + y * width, *line = pixels + y * width;
			for (x = 0; x < width; ++x) {
				intP_t idx = idxline[x], val;
				int is_delta = idx < tr->pal.nb_deltas;
				if (idx < 0) { // hard-coded delta for first 3 channels, otherwise 0
					if (i < 3) {
						idx = (intP_t) (~idx % 143); // say no to 1's complement
						val = JXSML__PALETTE_DELTAS[idx + 1][i];
						if (bpp > 8) val = (intP_t) (val << (jxsml__min32(bpp, 24) - 8));
					} else {
						val = 0;
					}
				} else if (idx < tr->pal.nb_colours) {
					val = palp[idx];
				} else { // synthesized from (idx - nb_colours)
					idx = (intP_t) (idx - tr->pal.nb_colours);
					if (idx < 64) { // idx == ..YX in base 4 -> {(X+0.5)/4, (Y+0.5)/4, ...}
						val = (intP_t) ((i < 3 ? idx >> (2 * i) : 0) * (((int2P_t) 1 << bpp) - 1) / 4 +
							(1 << jxsml__max32(0, bpp - 3)));
					} else { // idx + 64 == ..ZYX in base 5 -> {X/4, Y/4, Z/4, ...}
						val = (intP_t) (idx - 64);
						for (j = 0; j < i; ++j) val /= 5;
						val = (intP_t) ((val % 5) * ((1 << bpp) - 1) / 4);
					}
				}
				if (use_pred) {
					jxsml__(neighbors_t,P) p = jxsml__(neighbors,P)(pixels, x, y, width);
					jxsml__(wp_before_predict,2P)(&wp, x, y, &p);
					// TODO handle overflow
					if (is_delta) val = (intP_t) (val + jxsml__(predict,2P)(st, tr->pal.d_pred, &wp, &p));
					jxsml__(wp_after_predict,2P)(&wp, x, y, val);
				}
				line[x] = val;
			}
		}
		jxsml__(reset_wp,2P)(&wp);
	}

	jxsml__(free_wp,2P)(&wp);
	free(m->channel[0].pixels);
	memmove(m->channel, m->channel + 1, sizeof(jxsml__modular_channel_t) * (size_t) --m->num_channels);
	return 0;

error:
	jxsml__(free_wp,2P)(&wp);
	return st->err;
}

// ----------------------------------------
// end of recursion
	#undef jxsml__intP_t
	#undef jxsml__int2P_t
	#undef JXSML__P
	#undef JXSML__2P
#endif // JXSML__RECURSING == 300
#ifndef JXSML__RECURSING
// ----------------------------------------

JXSML_STATIC jxsml_err jxsml__inverse_transform(jxsml__st *st, jxsml__modular_t *m) {
	int32_t i;

	if (st->modular_16bit_buffers) {
		for (i = m->nb_transforms - 1; i >= 0; --i) {
			const jxsml__transform_t *tr = &m->transform[i];
			switch (tr->tr) {
			case JXSML__TR_RCT: JXSML__TRY(jxsml__inverse_rct16(st, m, tr)); break;
			case JXSML__TR_PALETTE: JXSML__TRY(jxsml__inverse_palette16(st, m, tr)); break;
			case JXSML__TR_SQUEEZE: JXSML__RAISE("TODO: squeeze inverse transformation"); break;
			default: JXSML__UNREACHABLE();
			}
		}
	} else {
		for (i = m->nb_transforms - 1; i >= 0; --i) {
			const jxsml__transform_t *tr = &m->transform[i];
			switch (tr->tr) {
			case JXSML__TR_RCT: JXSML__TRY(jxsml__inverse_rct32(st, m, tr)); break;
			case JXSML__TR_PALETTE: JXSML__TRY(jxsml__inverse_palette32(st, m, tr)); break;
			case JXSML__TR_SQUEEZE: JXSML__RAISE("TODO: squeeze inverse transformation"); break;
			default: JXSML__UNREACHABLE();
			}
		}
	}

error:
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// dequantization matrix

enum {
	JXSML__NUM_DCT_SELECT = 27, // the number of all possible varblock types (DctSelect)
	JXSML__NUM_DCT_PARAMS = 17, // the number of parameters, some shared by multiple DctSelects
	JXSML__NUM_ORDERS = 13, // the number of distinct varblock dimensions & orders, after transposition
};

enum { // hereafter DCTnm refers to DCT(2^n)x(2^m) in the spec, normalized so that n <= m
	JXSML__DCT33 = 0, JXSML__HORNUSS = 1, JXSML__DCT11 = 2, JXSML__DCT22 = 3, JXSML__DCT44 = 4,
	JXSML__DCT55 = 5, JXSML__DCT34 = 6, JXSML__DCT35 = 7, JXSML__DCT45 = 8, JXSML__DCT23 = 9,
	JXSML__AFV = 10, JXSML__DCT66 = 11, JXSML__DCT56 = 12, JXSML__DCT77 = 13, JXSML__DCT67 = 14,
	JXSML__DCT88 = 15, JXSML__DCT78 = 16, 
};

typedef struct {
	enum { // the number of params per channel follows:
		JXSML__DQ_ENC_LIBRARY = 0, // 0
		JXSML__DQ_ENC_HORNUSS = 1, // 3
		JXSML__DQ_ENC_DCT2 = 2, // 6
		JXSML__DQ_ENC_DCT4 = 3, // 2 + n
		JXSML__DQ_ENC_DCT4X8 = 4, // 1 + n
		JXSML__DQ_ENC_AFV = 5, // 6 + n + m
		JXSML__DQ_ENC_DCT = 6, // n
		JXSML__DQ_ENC_RAW = 7, // n rows * m columns (all other modes eventually decode to this one)
	} mode;
	int16_t n, m;
	float (*params)[4]; // the last element per each row is unused
} jxsml__dq_matrix_t;

static const struct jxsml__dct_params {
	int8_t log_rows, log_columns, def_offset, def_mode, def_n, def_m;
} JXSML__DCT_PARAMS[JXSML__NUM_DCT_PARAMS] = {
	/*DCT33*/ {3, 3, 0, JXSML__DQ_ENC_DCT, 0, 0}, /*Hornuss*/ {3, 3, 6, JXSML__DQ_ENC_HORNUSS, 0, 0},
	/*DCT11*/ {3, 3, 9, JXSML__DQ_ENC_DCT2, 0, 0}, /*DCT22*/ {3, 3, 15, JXSML__DQ_ENC_DCT4, 4, 0},
	/*DCT44*/ {4, 4, 21, JXSML__DQ_ENC_DCT, 7, 0}, /*DCT55*/ {5, 5, 28, JXSML__DQ_ENC_DCT, 8, 0},
	/*DCT34*/ {3, 4, 36, JXSML__DQ_ENC_DCT, 7, 0}, /*DCT35*/ {3, 5, 43, JXSML__DQ_ENC_DCT, 8, 0},
	/*DCT45*/ {4, 5, 51, JXSML__DQ_ENC_DCT, 8, 0}, /*DCT23*/ {3, 3, 59, JXSML__DQ_ENC_DCT4X8, 4, 0},
	/*AFV*/ {3, 3, 64, JXSML__DQ_ENC_AFV, 4, 4}, /*DCT66*/ {6, 6, 81, JXSML__DQ_ENC_DCT, 8, 0},
	/*DCT56*/ {5, 6, 89, JXSML__DQ_ENC_DCT, 8, 0}, /*DCT77*/ {7, 7, 97, JXSML__DQ_ENC_DCT, 8, 0},
	/*DCT67*/ {6, 7, 105, JXSML__DQ_ENC_DCT, 8, 0}, /*DCT88*/ {8, 8, 113, JXSML__DQ_ENC_DCT, 8, 0},
	/*DCT78*/ {7, 8, 121, JXSML__DQ_ENC_DCT, 8, 0},
};

#define JXSML__DCT4X4_DCT_PARAMS \
	{843.649426659137152f, 289.6948005482115584f, 137.04727932185712576f}, \
	{0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.5f} // (4)
#define JXSML__DCT4X8_DCT_PARAMS \
	{2198.050556016380522f, 764.3655248643528689f, 527.107573587542228f}, \
	{-0.96269623020744692f, -0.92630200888366945f, -1.4594385811273854f}, \
	{-0.76194253026666783f, -0.9675229603596517f, -1.450082094097871593f}, \
	{-0.6551140670773547f, -0.27845290869168118f, -1.5843722511996204f} // (4)
#define JXSML__LARGE_DCT_PARAMS(a,b,c) \
	{a, b, c}, {-1.025f, -0.3041958212306401f, -1.2f}, {-0.78f, 0.3633036457487539f, -1.2f}, \
	{-0.65012f, -0.35660379990111464f, -0.8f}, {-0.19041574084286472f, -0.3443074455424403f, -0.7f}, \
	{-0.20819395464f, -0.33699592683512467f, -0.7f}, {-0.421064f, -0.30180866526242109f, -0.4f}, \
	{-0.32733845535848671f, -0.27321683125358037f, -0.5f} // (8)
static const float JXSML__LIBRARY_DCT_PARAMS[129][4] = {
	// DCT33 dct_params (6)
	{2560.0f, 563.2f, 512.0f}, {0.0f, 0.0f, -3.0f}, {-0.4f, -0.3f, 0.0f},
	{-0.4f, -0.3f, 0.0f}, {-0.4f, -0.3f, -1.0f}, {-2.0f, -0.3f, -2.0f},
	// Hornuss params (3)
	{280.0f, 60.0f, 18.0f}, {3160.0f, 864.0f, 200.0f}, {3160.0f, 864.0f, 200.0f},
	// DCT11 params (6)
	{3840.0f, 960.0f, 640.0f}, {2560.0f, 640.0f, 320.0f}, {1280.0f, 320.0f, 128.0f},
	{640.0f, 180.0f, 64.0f}, {480.0f, 140.0f, 32.0f}, {300.0f, 120.0f, 16.0f},
	// DCT22 params (2) + dct_params (n=4)
	{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, JXSML__DCT4X4_DCT_PARAMS,
	// DCT44 dct_params (n=7)
	{8996.8725711814115328f, 3191.48366296844234752f, 1157.50408145487200256f},
	{-1.3000777393353804f, -0.67424582104194355f, -2.0531423165804414f},
	{-0.49424529824571225f, -0.80745813428471001f, -1.4f},
	{-0.439093774457103443f, -0.44925837484843441f, -0.50687130033378396f},
	{-0.6350101832695744f, -0.35865440981033403f, -0.42708730624733904f},
	{-0.90177264050827612f, -0.31322389111877305f, -1.4856834539296244f},
	{-1.6162099239887414f, -0.37615025315725483f, -4.9209142884401604f},
	// DCT55 dct_params (n=8)
	{15718.40830982518931456f, 7305.7636810695983104f, 3803.53173721215041536f},
	{-1.025f, -0.8041958212306401f, -3.060733579805728f},
	{-0.98f, -0.7633036457487539f, -2.0413270132490346f},
	{-0.9012f, -0.55660379990111464f, -2.0235650159727417f},
	{-0.4f, -0.49785304658857626f, -0.5495389509954993f},
	{-0.48819395464f, -0.43699592683512467f, -0.4f},
	{-0.421064f, -0.40180866526242109f, -0.4f},
	{-0.27f, -0.27321683125358037f, -0.3f},
	// DCT34 dct_params (n=7)
	{7240.7734393502f, 1448.15468787004f, 506.854140754517f},
	{-0.7f, -0.5f, -1.4f}, {-0.7f, -0.5f, -0.2f}, {-0.2f, -0.5f, -0.5f},
	{-0.2f, -0.2f, -0.5f}, {-0.2f, -0.2f, -1.5f}, {-0.5f, -0.2f, -3.6f},
	// DCT35 dct_params (n=8)
	{16283.2494710648897f, 5089.15750884921511936f, 3397.77603275308720128f},
	{-1.7812845336559429f, -0.320049391452786891f, -0.321327362693153371f},
	{-1.6309059012653515f, -0.35362849922161446f, -0.34507619223117997f},
	{-1.0382179034313539f, -0.30340000000000003f, -0.70340000000000003f},
	{-0.85f, -0.61f, -0.9f}, {-0.7f, -0.5f, -1.0f}, {-0.9f, -0.5f, -1.0f},
	{-1.2360638576849587f, -0.6f, -1.1754605576265209f},
	// DCT45 dct_params (n=8)
	{13844.97076442300573f, 4798.964084220744293f, 1807.236946760964614f},
	{-0.97113799999999995f, -0.61125308982767057f, -1.2f},
	{-0.658f, -0.83770786552491361f, -1.2f}, {-0.42026f, -0.79014862079498627f, -0.7f},
	{-0.22712f, -0.2692727459704829f, -0.7f}, {-0.2206f, -0.38272769465388551f, -0.7f},
	{-0.226f, -0.22924222653091453f, -0.4f}, {-0.6f, -0.20719098826199578f, -0.5f},
	// DCT23 params (1) + dct_params (n=4)
	{1.0f, 1.0f, 1.0f}, JXSML__DCT4X8_DCT_PARAMS,
	// AFV params (9) + dct_params (n=4) + dct4x4_params (m=4)
	{3072.0f, 1024.0f, 384.0f}, {3072.0f, 1024.0f, 384.0f}, {256.0f, 50.0f, 12.0f},
	{256.0f, 50.0f, 12.0f}, {256.0f, 50.0f, 12.0f}, {414.0f, 58.0f, 22.0f},
	{0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f},
	JXSML__DCT4X8_DCT_PARAMS, JXSML__DCT4X4_DCT_PARAMS,
	// DCT66 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(23966.1665298448605f, 8380.19148390090414f, 4493.02378009847706f),
	// DCT56 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(15358.89804933239925f, 5597.360516150652990f, 2919.961618960011210f),
	// DCT77 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(47932.3330596897210f, 16760.38296780180828f, 8986.04756019695412f),
	// DCT67 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(30717.796098664792f, 11194.72103230130598f, 5839.92323792002242f),
	// DCT88 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(95864.6661193794420f, 33520.76593560361656f, 17972.09512039390824f),
	// DCT78 dct_params (n=8)
	JXSML__LARGE_DCT_PARAMS(61435.5921973295970f, 24209.44206460261196f, 12979.84647584004484f),
};

JXSML_STATIC jxsml_err jxsml__dq_matrix(jxsml__st *st, int32_t rows, int32_t columns, jxsml__dq_matrix_t *dqmat) {
	int32_t c, i, j;

	dqmat->mode = jxsml__u(st, 3);
	dqmat->params = NULL;
	if (dqmat->mode == JXSML__DQ_ENC_RAW) { // read as a modular image
		float denom = jxsml__f16(st);
		JXSML__TRY(jxsml__zero_pad_to_byte(st));
		JXSML__RAISE("TODO: RAW dequant matrix");
	} else {
		// interpreted as 0xABCD: A is 1 if 8x8 matrix is required, B is the fixed params size,
		// C indicates that params[0..C-1] are to be scaled, D is the number of calls to ReadDctParams.
		static const int16_t HOW[7] = {0x0000, 0x1330, 0x1660, 0x1221, 0x1101, 0x1962, 0x1001};
		int32_t how = HOW[dqmat->mode];
		int32_t nparams = how >> 8 & 15, nscaled = how >> 4 & 15, ndctparams = how & 15;
		if (how >> 12) JXSML__SHOULD(rows == 8 && columns == 8, "dqm?");
		JXSML__SHOULD(dqmat->params = malloc(sizeof(float[3]) * (size_t) (nparams + ndctparams * 16)), "!mem");
		for (c = 0; c < 3; ++c) for (j = 0; j < nparams; ++j) {
			dqmat->params[j][c] = jxsml__f16(st) * (j < nscaled ? 64.0f : 1.0f);
		}
		for (i = 0; i < ndctparams; ++i) { // ReadDctParams
			int32_t n = *(i == 0 ? &dqmat->n : &dqmat->m) = (int16_t) (jxsml__u(st, 4) + 1);
			for (c = 0; c < 3; ++c) for (j = 0; j < n; ++j) {
				dqmat->params[nparams + j][c] = jxsml__f16(st) * (j == 0 ? 64.0f : 1.0f);
			}
			nparams += n;
		}
		JXSML__RAISE_DELAYED();
	}
	return 0;

error:
	free(dqmat->params);
	dqmat->params = NULL;
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__load_dq_matrix(jxsml__st *st, int32_t dct_select, jxsml__dq_matrix_t *dqmat) {
	const struct jxsml__dct_params dct = JXSML__DCT_PARAMS[dct_select];
	int32_t rows, columns, mode, n, m;
	const float (*params)[4];
	float (*raw)[4];
	int32_t c, i;

	mode = dqmat->mode;
	if (mode == JXSML__DQ_ENC_RAW) {
		return 0; // nothing to do
	} else if (mode == JXSML__DQ_ENC_LIBRARY) {
		mode = dct.def_mode;
		n = dct.def_n;
		m = dct.def_m;
		params = JXSML__LIBRARY_DCT_PARAMS + dct.def_offset;
	} else {
		n = dqmat->n;
		m = dqmat->m;
		params = dqmat->params;
	}

	rows = 1 << dct.log_rows;
	columns = 1 << dct.log_columns;
	JXSML__SHOULD(raw = malloc(sizeof(float[4]) * (size_t) (rows * columns)), "!mem");

	switch (dqmat->mode) {
	case JXSML__DQ_ENC_DCT:
		JXSML__RAISE("TODO: encoding mode DCT");
		break;

	case JXSML__DQ_ENC_DCT4:
		JXSML__ASSERT(rows == 8 && columns == 8);
		JXSML__RAISE("TODO: encoding mode DCT4");
		break;

	case JXSML__DQ_ENC_DCT2:
		JXSML__ASSERT(rows == 8 && columns == 8);
		for (c = 0; c < 3; ++c) {
			static const int8_t MAP[64] = {
				// TODO spec issue: coefficient (0,0) is unspecified; means it shouldn't be touched
				0,0,2,2,4,4,4,4,
				0,1,2,2,4,4,4,4,
				2,2,3,3,4,4,4,4,
				2,2,3,3,4,4,4,4,
				4,4,4,4,5,5,5,5,
				4,4,4,4,5,5,5,5,
				4,4,4,4,5,5,5,5,
				4,4,4,4,5,5,5,5,
			};
			for (i = 0; i < 64; ++i) raw[i][c] = params[MAP[i]][c];
			raw[0][c] = -1.0f;
		}
		break;

	case JXSML__DQ_ENC_HORNUSS:
		JXSML__ASSERT(rows == 8 && columns == 8);
		for (c = 0; c < 3; ++c) {
			for (i = 0; i < 64; ++i) raw[i][c] = params[0][c];
			raw[000][c] = 1.0f;
			raw[001][c] = raw[010][c] = params[1][c];
			raw[011][c] = params[2][c];
		}
		break;

	case JXSML__DQ_ENC_DCT4X8:
		JXSML__ASSERT(rows == 8 && columns == 8);
		JXSML__RAISE("TODO: encoding mode DCT4x8");
		break;

	case JXSML__DQ_ENC_AFV:
		JXSML__ASSERT(rows == 8 && columns == 8);
		JXSML__RAISE("TODO: encoding mode AFV");
		break;

	default: JXSML__UNREACHABLE();
	}

	free(dqmat->params);
	dqmat->mode = JXSML__DQ_ENC_RAW;
	dqmat->n = (int16_t) rows;
	dqmat->m = (int16_t) columns;
	dqmat->params = raw;
	return 0;

error:
	free(raw);
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// frame header & TOC

enum { JXSML__MAX_PASSES = 11 };

enum {
	JXSML__BLEND_REPLACE = 0, // new
	JXSML__BLEND_ADD = 1,     // old + new
	JXSML__BLEND_BLEND = 2,   // new + old * (1 - new alpha) or equivalent, optionally clamped
	JXSML__BLEND_MUL_ADD = 3, // old + new * alpha or equivalent, optionally clamped
	JXSML__BLEND_MUL = 4,     // old * new, optionally clamped
};
typedef struct { int8_t mode, alpha_chan, clamp, src_ref_frame; } jxsml__blend_info;

typedef struct {
	int is_last;
	enum { JXSML__FRAME_REGULAR = 0, JXSML__FRAME_LF = 1, JXSML__FRAME_REFONLY = 2, JXSML__FRAME_REGULAR_SKIPPROG = 3 } type;
	int is_modular; // VarDCT if false
	int has_noise, has_patches, has_splines, use_lf_frame, skip_adapt_lf_smooth;
	int do_ycbcr;
	int32_t jpeg_upsampling; // [0] | [1] << 2 | [2] << 4
	int32_t log_upsampling, *ec_log_upsampling;
	int32_t group_size_shift;
	int32_t x_qm_scale, b_qm_scale;
	int32_t num_passes;
	int8_t shift[JXSML__MAX_PASSES];
	int8_t log_ds[JXSML__MAX_PASSES + 1]; // pass i shift range is [log_ds[i+1], log_ds[i])
	int32_t lf_level;
	int32_t x0, y0, width, height;
	int32_t num_groups, num_lf_groups;
	int32_t duration, timecode;
	jxsml__blend_info blend_info, *ec_blend_info;
	int32_t save_as_ref;
	int save_before_ct;
	int32_t name_len;
	char *name;
	struct {
		int enabled;
		float weights[3 /*xyb*/][2 /*0=weight1, 1=weight2*/];
	} gab;
	struct {
		int32_t iters;
		float sharp_lut[8], channel_scale[3];
		float quant_mul, pass0_sigma_circle, pass2_sigma_circle, border_sad_mul, sigma_for_modular;
	} epf;
	float m_lf_unscaled[3 /*xyb*/];
	jxsml__tree_t *global_tree;
	jxsml__code_spec_t global_codespec;

	// modular only
	jxsml__modular_t gmodular;
	int32_t num_gm_channels; // <= gmodular.num_channels

	// vardct only
	int32_t global_scale, quant_lf;
	uint8_t *block_ctx_map;
	int32_t block_ctx_size, nb_block_ctx;
	int32_t colour_factor, x_factor_lf, b_factor_lf;
	float base_corr_x, base_corr_b;
	int32_t lf_thr[3][15], qf_thr[15];
	int32_t nb_lf_thr[3], nb_qf_thr;
	jxsml__dq_matrix_t dq_matrix[JXSML__NUM_DCT_PARAMS];
	int32_t num_hf_presets;
} jxsml__frame_t;

JXSML_STATIC jxsml_err jxsml__frame_header(jxsml__st *st, jxsml__frame_t *f) {
	int32_t i, j;

	f->is_last = 1;
	f->type = JXSML__FRAME_REGULAR;
	f->is_modular = 0;
	f->has_noise = f->has_patches = f->has_splines = f->use_lf_frame = f->skip_adapt_lf_smooth = 0;
	f->do_ycbcr = 0;
	f->jpeg_upsampling = 0;
	f->log_upsampling = 0;
	f->ec_log_upsampling = NULL;
	f->group_size_shift = 8;
	f->x_qm_scale = 3;
	f->b_qm_scale = 2;
	f->num_passes = 1;
	f->shift[0] = 0; // last pass if default
	f->log_ds[0] = 3; f->log_ds[1] = 0; // last pass if default
	f->lf_level = 0;
	f->x0 = f->y0 = 0;
	f->width = st->out->width;
	f->height = st->out->height;
	f->duration = f->timecode = 0;
	f->blend_info.mode = JXSML__BLEND_REPLACE;
	f->blend_info.alpha_chan = 0; // XXX set to the actual alpha channel
	f->blend_info.clamp = 0;
	f->blend_info.src_ref_frame = 0;
	f->ec_blend_info = NULL;
	f->save_as_ref = 0;
	f->save_before_ct = 1;
	f->name_len = 0;
	f->name = NULL;
	f->gab.enabled = 1;
	f->gab.weights[0][0] = f->gab.weights[1][0] = f->gab.weights[2][0] = 0.115169525f;
	f->gab.weights[0][1] = f->gab.weights[1][1] = f->gab.weights[2][1] = 0.061248592f;
	f->epf.iters = 2;
	for (i = 0; i < 8; ++i) f->epf.sharp_lut[i] = (float) i / 7.0f;
	f->epf.channel_scale[0] = 40.0f;
	f->epf.channel_scale[1] = 5.0f;
	f->epf.channel_scale[2] = 3.5f;
	f->epf.quant_mul = 0.46f;
	f->epf.pass0_sigma_circle = 0.9f;
	f->epf.pass2_sigma_circle = 6.5f;
	f->epf.border_sad_mul = 2.0f / 3.0f;
	f->epf.sigma_for_modular = 1.0f;
	f->m_lf_unscaled[0] = 4096.0f;
	f->m_lf_unscaled[1] = 512.0f;
	f->m_lf_unscaled[2] = 256.0f;
	f->global_tree = NULL;
	memset(&f->global_codespec, 0, sizeof(jxsml__code_spec_t));
	memset(&f->gmodular, 0, sizeof(jxsml__modular_t));
	f->block_ctx_map = NULL;
	f->colour_factor = 84;
	f->x_factor_lf = 127;
	f->b_factor_lf = 127;
	f->base_corr_x = 0.0f;
	f->base_corr_b = 1.0f;
	memset(f->dq_matrix, 0, sizeof(f->dq_matrix));

	JXSML__TRY(jxsml__zero_pad_to_byte(st));
	fprintf(stderr, "frame starts at %d\n", (int32_t) st->bits_read);

	if (!jxsml__u(st, 1)) { // !all_default
		int full_frame = 1;
		uint64_t flags;
		f->type = jxsml__u(st, 2);
		f->is_modular = jxsml__u(st, 1);
		flags = jxsml__u64(st);
		f->has_noise = (int) (flags & 1);
		f->has_patches = (int) (flags >> 1 & 1);
		f->has_splines = (int) (flags >> 4 & 1);
		f->use_lf_frame = (int) (flags >> 5 & 1);
		f->skip_adapt_lf_smooth = (int) (flags >> 7 & 1);
		if (!st->xyb_encoded) f->do_ycbcr = jxsml__u(st, 1);
		if (!f->use_lf_frame) {
			if (f->do_ycbcr) f->jpeg_upsampling = jxsml__u(st, 6); // yes, we are lazy
			f->log_upsampling = jxsml__u(st, 2);
			JXSML__SHOULD(f->log_upsampling == 0, "TODO: upsampling is not yet implemented");
			JXSML__SHOULD(f->ec_log_upsampling = malloc(sizeof(int32_t) * (size_t) st->num_extra_channels), "!mem");
			for (i = 0; i < st->num_extra_channels; ++i) {
				f->ec_log_upsampling[i] = jxsml__u(st, 2);
				JXSML__SHOULD(f->ec_log_upsampling[i] == 0, "TODO: upsampling is not yet implemented");
			}
		}
		if (f->is_modular) {
			f->group_size_shift = 7 + jxsml__u(st, 2);
		} else if (st->xyb_encoded) {
			f->x_qm_scale = jxsml__u(st, 3);
			f->b_qm_scale = jxsml__u(st, 3);
		}
		if (f->type != JXSML__FRAME_REFONLY) {
			f->num_passes = jxsml__u32(st, 1, 0, 2, 0, 3, 0, 4, 3);
			if (f->num_passes > 1) {
				// this part is especially flaky and the spec and libjxl don't agree to each other;
				// we do the most sensible thing that is still compatible to libjxl:
				// - downsample should be decreasing (or stay same)
				// - last_pass should be strictly increasing and last_pass[0] (if any) should be 0
				int8_t log_ds[4];
				int32_t ppass = 0, num_ds = jxsml__u32(st, 0, 0, 1, 0, 2, 0, 3, 1);
				JXSML__SHOULD(num_ds < f->num_passes, "pass");
				for (i = 0; i < f->num_passes - 1; ++i) f->shift[i] = (int8_t) jxsml__u(st, 2);
				f->shift[f->num_passes - 1] = 0;
				for (i = 0; i < num_ds; ++i) {
					log_ds[i] = (int8_t) jxsml__u(st, 2);
					if (i > 0) JXSML__SHOULD(log_ds[i - 1] >= log_ds[i], "pass");
				}
				for (i = 0; i < num_ds; ++i) {
					int32_t pass = jxsml__u32(st, 0, 0, 1, 0, 2, 0, 0, 3);
					JXSML__SHOULD(i > 0 ? ppass < pass && pass < f->num_passes : pass == 0, "pass");
					while (ppass < pass) f->log_ds[++ppass] = i > 0 ? log_ds[i - 1] : 3;
				}
				while (ppass < f->num_passes) f->log_ds[++ppass] = i > 0 ? log_ds[num_ds - 1] : 3;
			}
		}
		if (f->type == JXSML__FRAME_LF) {
			f->lf_level = jxsml__u(st, 2) + 1;
		} else if (jxsml__u(st, 1)) { // have_crop
			if (f->type != JXSML__FRAME_REFONLY) {
				f->x0 = jxsml__to_signed(jxsml__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30));
				f->y0 = jxsml__to_signed(jxsml__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30));
			}
			f->width = jxsml__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30);
			f->height = jxsml__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30);
			full_frame = f->x0 <= 0 && f->y0 <= 0 &&
				f->width + f->x0 >= st->out->width && f->height + f->y0 >= st->out->height;
		}
		if (f->type == JXSML__FRAME_REGULAR || f->type == JXSML__FRAME_REGULAR_SKIPPROG) {
			JXSML__SHOULD(f->ec_blend_info = malloc(sizeof(jxsml__blend_info) * (size_t) st->num_extra_channels), "!mem");
			for (i = -1; i < st->num_extra_channels; ++i) {
				jxsml__blend_info *blend = i < 0 ? &f->blend_info : &f->ec_blend_info[i];
				blend->mode = (int8_t) jxsml__u32(st, 0, 0, 1, 0, 2, 0, 3, 2);
				if (st->num_extra_channels > 0) {
					if (blend->mode == JXSML__BLEND_BLEND || blend->mode == JXSML__BLEND_MUL_ADD) {
						blend->alpha_chan = (int8_t) jxsml__u32(st, 0, 0, 1, 0, 2, 0, 3, 3);
						blend->clamp = (int8_t) jxsml__u(st, 1);
					} else if (blend->mode == JXSML__BLEND_MUL) {
						blend->clamp = (int8_t) jxsml__u(st, 1);
					}
				}
				if (!full_frame || blend->mode != JXSML__BLEND_REPLACE) {
					blend->src_ref_frame = (int8_t) jxsml__u(st, 2);
				}
			}
			if (st->out->anim_tps_denom) { // have_animation stored implicitly
				f->duration = jxsml__u32(st, 0, 0, 1, 0, 0, 8, 0, 32); // TODO uh, u32?
				if (st->out->anim_have_timecodes) {
					f->timecode = jxsml__u(st, 32); // TODO uh, u32??
				}
			}
			f->is_last = jxsml__u(st, 1);
		} else {
			f->is_last = 0;
		}
		if (f->type != JXSML__FRAME_LF && !f->is_last) f->save_as_ref = jxsml__u(st, 2);
		if (f->type == JXSML__FRAME_REFONLY || (
			full_frame &&
			(f->type == JXSML__FRAME_REGULAR || f->type == JXSML__FRAME_REGULAR_SKIPPROG) &&
			f->blend_info.mode == JXSML__BLEND_REPLACE &&
			(f->duration == 0 || f->save_as_ref != 0) &&
			!f->is_last
		)) {
			f->save_before_ct = jxsml__u(st, 1);
		} else {
			f->save_before_ct = (f->type == JXSML__FRAME_LF);
		}
		JXSML__TRY(jxsml__name(st, &f->name_len, &f->name));
		{ // RestorationFilter
			int restoration_all_default = jxsml__u(st, 1);
			f->gab.enabled = restoration_all_default ? 1 : jxsml__u(st, 1);
			if (f->gab.enabled) {
				if (jxsml__u(st, 1)) { // gab_custom
					for (i = 0; i < 3; ++i) for (j = 0; j < 2; ++j) f->gab.weights[i][j] = jxsml__f16(st);
				}
			}
			f->epf.iters = restoration_all_default ? 2 : jxsml__u(st, 2);
			if (f->epf.iters) {
				if (!f->is_modular && jxsml__u(st, 1)) { // epf_sharp_custom
					for (i = 0; i < 8; ++i) f->epf.sharp_lut[i] = jxsml__f16(st);
				}
				if (jxsml__u(st, 1)) { // epf_weight_custom
					for (i = 0; i < 3; ++i) f->epf.channel_scale[i] = jxsml__f16(st);
					JXSML__TRY(jxsml__skip(st, 32)); // ignored
				}
				if (jxsml__u(st, 1)) { // epf_sigma_custom
					if (!f->is_modular) f->epf.quant_mul = jxsml__f16(st);
					f->epf.pass0_sigma_circle = jxsml__f16(st);
					f->epf.pass2_sigma_circle = jxsml__f16(st);
					f->epf.border_sad_mul = jxsml__f16(st);
				}
				if (f->epf.iters && f->is_modular) f->epf.sigma_for_modular = jxsml__f16(st);
			}
			if (!restoration_all_default) JXSML__TRY(jxsml__extensions(st));
		}
		JXSML__TRY(jxsml__extensions(st));
	}
	JXSML__RAISE_DELAYED();

	if (st->xyb_encoded && st->want_icc) f->save_before_ct = 1; // ignores the decoded bit
	f->num_groups = ((f->width + (1 << f->group_size_shift) - 1) >> f->group_size_shift) *
		((f->height + (1 << f->group_size_shift) - 1) >> f->group_size_shift);
	f->num_lf_groups = ((f->width + (8 << f->group_size_shift) - 1) >> (3 + f->group_size_shift)) *
		((f->height + (8 << f->group_size_shift) - 1) >> (3 + f->group_size_shift));
	return 0;

error:
	free(f->ec_log_upsampling);
	free(f->ec_blend_info);
	free(f->name);
	f->ec_log_upsampling = NULL;
	f->ec_blend_info = NULL;
	f->name = NULL;
	return st->err;
}

// also used in jxsml__hf_global
// TODO permutation may have to handle more than 2^31 entries
JXSML_STATIC jxsml_err jxsml__permutation(
	jxsml__st *st, jxsml__code_t *code,
	int32_t size, int32_t skip, int32_t *outend, int32_t **outlehmer // has *outend entries
) {
	int32_t *arr = NULL;
	int32_t i, prev, end;

	JXSML__ASSERT(code->spec->num_dist == 8 + !!code->spec->lz77_enabled);

	end = jxsml__code(st, jxsml__min32(7, jxsml__ceil_lg32((uint32_t) size + 1)), 0, code);
	JXSML__SHOULD(end <= size - skip, "perm");
	JXSML__SHOULD(arr = malloc(sizeof(int32_t) * (size_t) end), "!mem");

	prev = 0;
	for (i = 0; i < end; ++i) {
		prev = arr[i] = jxsml__code(st, jxsml__min32(7, jxsml__ceil_lg32((uint32_t) prev + 1)), 0, code);
		JXSML__SHOULD(prev < size - (skip + i), "perm");
	}

	*outend = end;
	*outlehmer = arr;
error:
	return st->err;
}

// target is pre-shifted by skip
#define JXSML__APPLY_PERMUTATION(type, target, lehmer, size, end) do { \
	int32_t i_, x_; \
	for (i_ = 0; i_ < (end); ++i_) { \
		type temp = (target)[x_ = lehmer[i_]]; \
		memmove((target) + i_ + 1, (target) + i_, sizeof(type) * (size_t) (x_ - i_)); \
		(target)[i_] = temp; \
	} \
} while (0)

JXSML_STATIC jxsml_err jxsml__toc(jxsml__st *st, jxsml__frame_t *f) {
	typedef struct { int32_t lo, hi; } toc_t;
	int32_t size = f->num_passes == 1 && f->num_groups == 1 ? 1 :
		1 /*lf_global*/ + f->num_lf_groups /*lf_group*/ +
		1 /*hf_global + hf_pass*/ + f->num_passes * f->num_groups /*group_pass*/;
	toc_t *toc;
	int32_t *lehmer = NULL, lehmer_end;
	jxsml__code_spec_t codespec = {0};
	jxsml__code_t code = { .spec = &codespec };
	int32_t base, i;

	JXSML__SHOULD(toc = malloc(sizeof(toc_t) * (size_t) size), "!mem");

	if (jxsml__u(st, 1)) { // permuted
		JXSML__TRY(jxsml__code_spec(st, 8, &codespec));
		JXSML__TRY(jxsml__permutation(st, &code, size, 0, &lehmer_end, &lehmer));
		JXSML__TRY(jxsml__finish_and_free_code(st, &code));
		jxsml__free_code_spec(&codespec);
	}
	JXSML__TRY(jxsml__zero_pad_to_byte(st));

	for (i = 0; i < size; ++i) {
		toc[i].lo = i > 0 ? toc[i - 1].hi : 0;
		toc[i].hi = toc[i].lo + jxsml__u32(st, 0, 10, 1024, 14, 17408, 22, 4211712, 30);
	}
	JXSML__TRY(jxsml__zero_pad_to_byte(st));
	base = (int32_t) (st->bits_read / 8);
	for (i = 0; i < size; ++i) toc[i].lo += base, toc[i].hi += base;

	if (lehmer) JXSML__APPLY_PERMUTATION(toc_t, toc, lehmer, size, lehmer_end);

	fprintf(stderr, "TOC: lf_global %d-%d\n", toc[0].lo, toc[0].hi);
	if (size > 1) {
		int32_t j, k;
		fprintf(stderr, "     lf_group");
		for (i = 1, j = 0; j < f->num_lf_groups; ++i, ++j) fprintf(stderr, " %d:%d-%d", j, toc[i].lo, toc[i].hi);
		fprintf(stderr, "\n     hf_global/hf_pass %d-%d\n", toc[i].lo, toc[i].hi); ++i;
		for (j = 0; j < f->num_passes; ++j) {
			fprintf(stderr, "     pass[%d]", j);
			for (k = 0; k < f->num_groups; ++i, ++k) fprintf(stderr, " %d:%d-%d", k, toc[i].lo, toc[i].hi);
			fprintf(stderr, "\n");
		}
	}

	free(lehmer);
	free(toc); // TODO use toc somehow (especially required for permuted one)
	return 0;

error:
	free(lehmer);
	free(toc);
	jxsml__free_code(&code);
	jxsml__free_code_spec(&codespec);
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////
// frame

JXSML_STATIC jxsml_err jxsml__lf_global(jxsml__st *st, jxsml__frame_t *f) {
	int32_t i, j;

	if (f->has_patches) JXSML__RAISE("TODO: patches");
	if (f->has_splines) JXSML__RAISE("TODO: splines");
	if (f->has_noise) JXSML__RAISE("TODO: noise");

	if (!jxsml__u(st, 1)) { // LfChannelDequantization.all_default
		for (i = 0; i < 3; ++i) f->m_lf_unscaled[i] = jxsml__f16(st);
	}

	if (!f->is_modular) {
		f->global_scale = jxsml__u32(st, 1, 11, 2049, 11, 4097, 12, 8193, 16);
		f->quant_lf = jxsml__u32(st, 16, 0, 1, 5, 1, 8, 1, 16);
		// TODO pretty sure that their formulae are somehow off

		// HF block context
		if (jxsml__u(st, 1)) {
			static const uint8_t DEFAULT_BLKCTX[] = {
				0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 6, 6, 6,
				7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
				7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
			};
			f->block_ctx_size = sizeof(DEFAULT_BLKCTX) / sizeof(*DEFAULT_BLKCTX);
			JXSML__SHOULD(f->block_ctx_map = malloc(sizeof(DEFAULT_BLKCTX)), "!mem");
			memcpy(f->block_ctx_map, DEFAULT_BLKCTX, sizeof(DEFAULT_BLKCTX));
		} else {
			JXSML__RAISE_DELAYED();
			f->block_ctx_size = 39;
			for (i = 0; i < 3; ++i) {
				f->nb_lf_thr[i] = jxsml__u(st, 4);
				for (j = 0; j < f->nb_lf_thr[i]; ++j) {
					f->lf_thr[i][j] = jxsml__to_signed(jxsml__u32(st, 0, 4, 16, 8, 272, 16, 65808, 32));
				}
				f->block_ctx_size *= f->nb_lf_thr[i] + 1;
			}
			f->nb_qf_thr = jxsml__u(st, 4);
			for (i = 0; i < f->nb_qf_thr; ++i) f->qf_thr[i] = jxsml__u32(st, 0, 2, 4, 3, 12, 5, 44, 8);
			f->block_ctx_size *= f->nb_qf_thr + 1;
			JXSML__SHOULD(f->block_ctx_size <= 39 * 64, "hfbc"); // block_ctx_size <= 39*15^4 and never overflows
			JXSML__SHOULD(f->block_ctx_map = malloc(sizeof(uint8_t) * (size_t) f->block_ctx_size), "!mem");
			JXSML__TRY(jxsml__cluster_map(st, f->block_ctx_size, 16, &f->nb_block_ctx, f->block_ctx_map));
		}

		if (!jxsml__u(st, 1)) { // LfChannelCorrelation.all_default
			f->colour_factor = jxsml__u32(st, 84, 0, 256, 0, 2, 8, 258, 16);
			f->base_corr_x = jxsml__f16(st);
			f->base_corr_b = jxsml__f16(st);
			f->x_factor_lf = jxsml__u(st, 8);
			f->b_factor_lf = jxsml__u(st, 8);
		}
	}

	if (jxsml__u(st, 1)) { // global tree present
		JXSML__TRY(jxsml__tree(st, &f->global_tree, &f->global_codespec));
	}
	JXSML__TRY(jxsml__init_modular_for_global(st, f->is_modular, f->do_ycbcr,
		f->log_upsampling, f->ec_log_upsampling, f->width, f->height, &f->gmodular));
	if (f->gmodular.num_channels > 0) {
		JXSML__TRY(jxsml__modular_header(st, f->global_tree, &f->global_codespec, &f->gmodular));
		JXSML__TRY(jxsml__allocate_modular(st, &f->gmodular));
		if (f->width <= (1 << f->group_size_shift) && f->height <= (1 << f->group_size_shift)) {
			f->num_gm_channels = f->gmodular.num_channels;
		} else {
			f->num_gm_channels = f->gmodular.nb_meta_channels;
		}
		for (i = 0; i < f->num_gm_channels; ++i) {
			JXSML__TRY(jxsml__modular_channel(st, &f->gmodular, i, 0));
		}
		JXSML__TRY(jxsml__finish_and_free_code(st, &f->gmodular.code));
	} else {
		f->num_gm_channels = 0;
	}

	JXSML__TRY(jxsml__zero_pad_to_byte(st));
	return 0;

error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__lf_group(jxsml__st *st, jxsml__frame_t *f, int32_t gw, int32_t gh, int32_t sidx) {
	jxsml__modular_t m = {0};
	int32_t i;

	// TODO factor into jxsml__init_modular_for_lf_group
	for (i = f->num_gm_channels; i < f->gmodular.num_channels; ++i) {
		jxsml__modular_channel_t *c = &f->gmodular.channel[i];
		if (c->hshift >= 3 && c->vshift >= 3) {
			JXSML__RAISE("TODO: ModularLfGroup decoding should continue here");
		}
	}

	if (!f->is_modular) {
		int32_t gw8 = (gw + 7) / 8, gh8 = (gh + 7) / 8;
		int32_t gw64 = (gw + 63) / 64, gh64 = (gh + 63) / 64;
		int32_t w[4], h[4];

		// LfQuant
		if (!f->use_lf_frame) {
			int32_t extra_prec = jxsml__u(st, 2);
			JXSML__SHOULD(f->jpeg_upsampling == 0, "TODO: subimage w/h depends on jpeg_upsampling");
			w[0] = w[1] = w[2] = gw8;
			h[0] = h[1] = h[2] = gh8;
			JXSML__TRY(jxsml__init_modular(st, 3, w, h, &m));
			JXSML__TRY(jxsml__modular_header(st, f->global_tree, &f->global_codespec, &m));
			JXSML__TRY(jxsml__allocate_modular(st, &m));
			for (i = 0; i < 3; ++i) JXSML__TRY(jxsml__modular_channel(st, &m, i, sidx));
			JXSML__TRY(jxsml__finish_and_free_code(st, &m.code));
			JXSML__TRY(jxsml__inverse_transform(st, &m));
			// TODO extract m.channel[0..2] and dequantize (I.4.2)
			jxsml__free_modular(&m);
		}

		// HF metadata
		// TODO this can get VERY large!
		int32_t nb_blocks = jxsml__u(st, jxsml__ceil_lg64((uint64_t) gw8 * (uint64_t) gh8)) + 1;
		w[0] = w[1] = gw64; h[0] = h[1] = gh64; // XFromY, BFromY
		w[2] = nb_blocks; h[2] = 2; // BlockInfo
		w[3] = gw8; h[3] = gh8; // Sharpness
		JXSML__TRY(jxsml__init_modular(st, 4, w, h, &m));
		JXSML__TRY(jxsml__modular_header(st, f->global_tree, &f->global_codespec, &m));
		JXSML__TRY(jxsml__allocate_modular(st, &m));
		for (i = 0; i < 4; ++i) {
			JXSML__TRY(jxsml__modular_channel(st, &m, i, sidx + 2 * f->num_lf_groups));
		}
		JXSML__TRY(jxsml__finish_and_free_code(st, &m.code));
		JXSML__TRY(jxsml__inverse_transform(st, &m));
		// TODO extract m.channel[0..3] and place blocks and whatever (G.2.4)
		jxsml__free_modular(&m);
	}

	JXSML__TRY(jxsml__zero_pad_to_byte(st));
	return 0;

error:
	jxsml__free_modular(&m);
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__hf_global(jxsml__st *st, jxsml__frame_t *f, int32_t raw_sidx_base) {
	int32_t i, j, c;

	JXSML__ASSERT(!f->is_modular);

	// dequantization matrices
	if (!jxsml__u(st, 1)) {
		// TODO spec improvement: encoding mode 1..5 are only valid for 0-3/9-10 since it requires 8x8 matrix, explicitly note this
		for (i = 0; i < JXSML__NUM_DCT_PARAMS; ++i) {
			const struct jxsml__dct_params dct = JXSML__DCT_PARAMS[i];
			JXSML__TRY(jxsml__dq_matrix(st, 1 << (int32_t) dct.log_rows, 1 << (int32_t) dct.log_columns, &f->dq_matrix[i]));
		}
	}

	// TODO is it possible that num_hf_presets > num_groups? otherwise jxsml__at_most is better
	f->num_hf_presets = jxsml__u(st, jxsml__ceil_lg32((uint32_t) f->num_groups)) + 1;
	JXSML__RAISE_DELAYED();

	// HfPass
	for (i = 0; i < f->num_passes; ++i) {
		int32_t used_orders = jxsml__u32(st, 0x5f, 0, 0x13, 0, 0, 0, 0, 13);
		jxsml__code_spec_t codespec = {0};
		jxsml__code_t code = { .spec = &codespec };
		if (used_orders > 0) JXSML__TRY(jxsml__code_spec(st, 8, &codespec));
		for (j = 0; j < JXSML__NUM_ORDERS; ++j) {
			static const int8_t LOG_ORDER_SIZE[JXSML__NUM_ORDERS][2] = {
				{3,3},{3,3},{4,4},{5,5},{4,3},{5,3},{5,4},{6,6},{6,5},{7,7},{7,6},{8,8},{8,7}
			};
			// TODO initialize order[j] from natural_coeff_order[j] (I.6.5)
			if (used_orders >> j & 1) {
				int32_t size = 1 << (LOG_ORDER_SIZE[j][0] + LOG_ORDER_SIZE[j][1]);
				int32_t *lehmer = NULL, lehmer_end;
				for (c = 0; c < 3; ++c) {
					JXSML__TRY(jxsml__permutation(st, &code, size, size / 64, &lehmer_end, &lehmer));
					// TODO use this to permute order[j]
					fprintf(stderr, "pass %d permutation %d channel %d:", i, j, c);
					int k;
					for (k = 0; k < lehmer_end - size/64; ++k) fprintf(stderr, " %d", lehmer[k]);
					fprintf(stderr, "\n");
				}
			}
		}
		if (used_orders > 0) {
			JXSML__TRY(jxsml__finish_and_free_code(st, &code));
			jxsml__free_code_spec(&codespec);
		}

		JXSML__TRY(jxsml__code_spec(st, 495 * f->nb_block_ctx * f->num_hf_presets, &codespec));
	}

	return 0;

error:
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__pass_group(
	jxsml__st *st, jxsml__frame_t *f, int32_t gx, int32_t gy, int32_t gw, int32_t gh, int32_t sidx
) {
	jxsml__modular_t m = {0};
	int32_t i;

	if (!f->is_modular) {
		int32_t hfp = 495 * f->nb_block_ctx * jxsml__u(st, jxsml__ceil_lg32((uint32_t) f->num_hf_presets));
		JXSML__RAISE("TODO: HF coefficients in PassGroup");
	}

	JXSML__TRY(jxsml__init_modular_for_pass_group(st, f->num_gm_channels, gw, gh, 0, 3, &f->gmodular, &m));
	if (m.num_channels > 0) {
		JXSML__TRY(jxsml__modular_header(st, f->global_tree, &f->global_codespec, &m));
		JXSML__TRY(jxsml__allocate_modular(st, &m));
		for (i = 0; i < m.num_channels; ++i) {
			JXSML__TRY(jxsml__modular_channel(st, &m, i, sidx));
		}
		JXSML__TRY(jxsml__finish_and_free_code(st, &m.code));
		JXSML__TRY(jxsml__inverse_transform(st, &m));
		JXSML__TRY(jxsml__combine_modular_from_pass_group(st, f->num_gm_channels, gy, gx, 0, 3, &f->gmodular, &m));
		jxsml__free_modular(&m);
	}

	JXSML__TRY(jxsml__zero_pad_to_byte(st));
	return 0;

error:
	jxsml__free_modular(&m);
	return st->err;
}

JXSML_STATIC jxsml_err jxsml__frame(jxsml__st *st, jxsml__frame_t *f) {
	int32_t i, gx, gy, gw, gh, gsize, sidx;

	JXSML__TRY(jxsml__frame_header(st, f));
	JXSML__TRY(jxsml__toc(st, f));
	JXSML__TRY(jxsml__lf_global(st, f));

	sidx = 1;
	gsize = 8 << f->group_size_shift;
	for (gy = 0; gy < f->height; gy += gsize) {
		gh = jxsml__min32(gsize, f->height - gy);
		for (gx = 0; gx < f->width; gx += gsize, ++sidx) {
			gw = jxsml__min32(gsize, f->width - gx);
			fprintf(stderr, "lf group gx %d gy %d gw %d gh %d sidx %d\n", gx, gy, gw, gh, sidx);
			JXSML__TRY(jxsml__lf_group(st, f, gw, gh, sidx));
		}
	}

	if (!f->is_modular) JXSML__TRY(jxsml__hf_global(st, f, 1 + 3 * f->num_lf_groups));

	for (i = 0; i < f->num_passes; ++i) {
		if (i > 0) JXSML__RAISE("TODO: more passes");
		sidx = 1 + 3 * f->num_lf_groups + JXSML__NUM_DCT_PARAMS + i * f->num_groups;
		gsize = 1 << f->group_size_shift;
		for (gy = 0; gy < f->height; gy += gsize) {
			gh = jxsml__min32(gsize, f->height - gy);
			for (gx = 0; gx < f->width; gx += gsize, ++sidx) {
				gw = jxsml__min32(gsize, f->width - gx);
				fprintf(stderr, "pass %d gx %d gy %d gw %d gh %d sidx %d\n", i, gx, gy, gw, gh, sidx);
				JXSML__TRY(jxsml__pass_group(st, f, gx, gy, gw, gh, sidx));
			}
		}
	}

	JXSML__TRY(jxsml__inverse_transform(st, &f->gmodular));
error:
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////

static const char *dumppath = NULL;
void end(const jxsml img, const jxsml__st st) {}

static void update_cksum(uint8_t b, uint32_t *crc, uint32_t *adler) {
	static const uint32_t TAB[16] = {
		0x00000000,0x1DB71064,0x3B6E20C8,0x26D930AC,0x76DC4190,0x6B6B51F4,0x4DB26158,0x5005713C,
		0xEDB88320,0xF00F9344,0xD6D6A3E8,0xCB61B38C,0x9B64C2B0,0x86D3D2D4,0xA00AE278,0xBDBDF21C,
	};
	uint32_t lo, hi;
	*crc = TAB[(*crc ^ b) & 15] ^ (*crc >> 4);
	*crc = TAB[(*crc ^ (b >> 4)) & 15] ^ (*crc >> 4);
	lo = ((*adler & 0xffff) + b) % 65521;
	hi = ((*adler >> 16) + lo) % 65521;
	*adler = (hi << 16) | lo;
}

jxsml_err jxsml_load_from_memory(jxsml *out, const void *buf, size_t bufsize) {
	jxsml__st st_ = {
		.out = out,
		.modular_16bit_buffers = 1,
		.xyb_encoded = 1,
		.opsin_inv_mat = {
			{11.031566901960783f, -9.866943921568629f, -0.16462299647058826f},
			{-3.254147380392157f, 4.418770392156863f, -0.16462299647058826f},
			{-3.6588512862745097f, 2.7129230470588235f, 1.9459282392156863f},
		},
		.opsin_bias = {-0.0037930732552754493f, -0.0037930732552754493f, -0.0037930732552754493f},
		.quant_bias = {1-0.05465007330715401f, 1-0.07005449891748593f, 1-0.049935103337343655f},
		.quant_bias_num = 0.145f,
	};
	jxsml__st *st = &st_;
	int32_t i, j, k;

	JXSML__TRY(jxsml__init_container(st, buf, bufsize));

	JXSML__SHOULD(st->bufsize >= 2, "shrt");
	JXSML__SHOULD(st->buf[0] == 0xff && st->buf[1] == 0x0a, "!jxl");
	st->buf += 2;
	st->bufsize -= 2;
	st->bits_read = 16;

	JXSML__TRY(jxsml__size_header(st, &st->out->width, &st->out->height));
	JXSML__TRY(jxsml__image_metadata(st));
	if (st->want_icc) JXSML__TRY(jxsml__icc(st));

	jxsml__frame_t frame;
	do {
		JXSML__TRY(jxsml__frame(st, &frame));
		fprintf(stderr, "%sframe: %s, %d+%d+%dx%d\n", frame.is_last ? "last " : "",
			(char*[]){"regular", "lf", "refonly", "regular-but-skip-progressive"}[frame.type],
			frame.x0, frame.y0, frame.width, frame.height);

		if (dumppath && frame.type == JXSML__FRAME_REGULAR) {
			JXSML__ASSERT(st->modular_16bit_buffers && st->out->bpp >= 8 && st->out->exp_bits == 0);
			FILE *f = fopen(dumppath, "wb");
			uint32_t crc, adler, unused = 0, idatsize;
			int16_t *c[4];
			int32_t nchan;
			uint8_t buf[32];
			nchan = (!frame.do_ycbcr && !st->xyb_encoded && st->out->cspace == JXSML_CS_GREY ? 1 : 3);
			for (i = 0; i < nchan; ++i) c[i] = frame.gmodular.channel[i].pixels;
			for (i = nchan; i < frame.gmodular.num_channels; ++i) {
				jxsml__ec_info *ec = &st->ec_info[i - nchan];
				if (ec->type == JXSML_EC_ALPHA) {
					JXSML__ASSERT(ec->bpp == st->out->bpp && ec->exp_bits == st->out->exp_bits);
					JXSML__ASSERT(ec->dim_shift == 0 && !ec->data.alpha_associated);
					c[nchan++] = frame.gmodular.channel[i].pixels;
					break;
				}
			}
			fwrite("\x89PNG\x0d\x0a\x1a\x0a" "\0\0\0\x0d", 12, 1, f);
			memcpy(buf, "IHDR" "wwww" "hhhh" "\x08" "C" "\0\0\0" "crcc" "lenn" "IDAT" "\x78\x01", 31);
			for (i = 0; i < 4; ++i) buf[i + 4] = (uint8_t) ((frame.width >> (24 - i * 8)) & 0xff);
			for (i = 0; i < 4; ++i) buf[i + 8] = (uint8_t) ((frame.height >> (24 - i * 8)) & 0xff);
			buf[13] = (uint8_t) ((nchan % 2 == 0 ? 4 : 0) | (nchan > 2 ? 2 : 0));
			for (i = 0, crc = ~0u; i < 17; ++i) update_cksum(buf[i], &crc, &unused);
			for (i = 0; i < 4; ++i) buf[i + 17] = (uint8_t) ((~crc >> (24 - i * 8)) & 0xff);
			idatsize = (uint32_t) (6 + (nchan * frame.width + 6) * frame.height);
			for (i = 0; i < 4; ++i) buf[i + 21] = (uint8_t) ((idatsize >> (24 - i * 8)) & 0xff);
			fwrite(buf, 31, 1, f);
			for (i = 25, crc = ~0u; i < 31; ++i) update_cksum(buf[i], &crc, &unused);
			adler = 1;
			for (i = 0; i < frame.height; ++i) {
				update_cksum(buf[0] = (i == frame.height - 1), &crc, &unused);
				update_cksum(buf[1] = (uint8_t) ((nchan * frame.width + 1) & 0xff), &crc, &unused);
				update_cksum(buf[2] = (uint8_t) ((nchan * frame.width + 1) >> 8), &crc, &unused);
				update_cksum(buf[3] = (uint8_t) ~((nchan * frame.width + 1) & 0xff), &crc, &unused);
				update_cksum(buf[4] = (uint8_t) ~((nchan * frame.width + 1) >> 8), &crc, &unused);
				update_cksum(buf[5] = 0, &crc, &adler);
				fwrite(buf, 6, 1, f);
				for (j = 0; j < frame.width; ++j) {
					for (k = 0; k < nchan; ++k) {
						int32_t p = jxsml__min32(jxsml__max32(0, c[k][i * frame.width + j]), 1 << st->out->bpp);
						update_cksum(buf[k] = (uint8_t) (p * 255 / ((1 << st->out->bpp) - 1)), &crc, &adler);
					}
					fwrite(buf, (size_t) nchan, 1, f);
				}
			}
			for (i = 0; i < 4; ++i) update_cksum(buf[i] = (uint8_t) ((adler >> (24 - i * 8)) & 0xff), &crc, &unused);
			for (i = 0; i < 4; ++i) buf[i + 4] = (uint8_t) ((~crc >> (24 - i * 8)) & 0xff);
			memcpy(buf + 8, "\0\0\0\0" "IEND" "\xae\x42\x60\x82", 12);
			fwrite(buf, 20, 1, f);
			fclose(f);
			break;
		}
	} while (!frame.is_last);

	end(*st->out, st_);
	return 0;

error:
	return st->err;
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {
	if (argc < 2) return 1;
	if (argc > 2) dumppath = argv[2];
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) return 2;
	if (fseek(fp, 0, SEEK_END)) return 2;
	long bufsize = ftell(fp);
	if (bufsize < 0) return 2;
	if (fseek(fp, 0, SEEK_SET)) return 2;
	void *buf = malloc((size_t) bufsize);
	if (!buf) return 3;
	if (fread(buf, (size_t) bufsize, 1, fp) != 1) return 2;
	fclose(fp);

	jxsml img;
	jxsml_err ret = jxsml_load_from_memory(&img, buf, (size_t) bufsize);
	if (ret) {
		fprintf(stderr, "error: %c%c%c%c\n", ret >> 24 & 0xff, ret >> 16 & 0xff, ret >> 8 & 0xff, ret & 0xff);
		return 1;
	}
	fprintf(stderr, "ok\n");
	return 0;
}

#endif // !JXSML__RECURSING (there should be no actual code below, as JXSML may #include itself!)
