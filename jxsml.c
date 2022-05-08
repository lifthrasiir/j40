// JXSML: An extra small JPEG XL decoder
// Kang Seonghoon, 2022-05, Public Domain (CC0)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
} jxsml;

typedef struct {
	jxsml *out;
	int err;

	const uint8_t *buf;
	size_t bufsize;
	uint32_t bits;
	int nbits;

	int modular_16bit_buffers;
	int num_extra_channels;
	int xyb_encoded;
	float opsin_inv_mat[3][3], opsin_bias[3], quant_bias[3], quant_bias_num;
	int want_icc;
} jxsml__st;

#define JXSML__4(s) (int) (((uint32_t) s[0] << 24) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 8) | (uint32_t) s[3])
#define JXSML__ERR(s) (st->err ? st->err : (st->err = JXSML__4(s)))
#define JXSML__SHOULD(cond, s) do { if (st->err) goto error; else if (!(cond)) { st->err = JXSML__4(s); goto error; } } while (0)
#define JXSML__RAISE(s) do { if (!st->err) st->err = JXSML__4(s); goto error; } while (0)
#define JXSML__RAISE_DELAYED() do { if (st->err) goto error; } while (0)
#define JXSML__TRY(expr) do { if (expr) goto error; } while (0)
#ifdef JXSML_DEBUG
#define JXSML__ASSERT(cond) JXSML__SHOULD(cond, "!exp")
#define JXSML__UNREACHABLE() JXSML__ASSERT(0)
#else
#define JXSML__ASSERT(cond) (void) 0
#define JXSML__UNREACHABLE() (void) 0
#endif

int jxsml__always_refill(jxsml__st *st) {
	size_t consumed = (size_t) ((32 - st->nbits) >> 3);
	if (st->bufsize < consumed) return JXSML__ERR("shrt");
	st->bufsize -= consumed;
	do {
		st->bits |= (uint32_t) *st->buf++ << st->nbits;
		st->nbits += 8;
	} while (st->nbits <= 24);
	return st->err;
}

// ensure st->nbits is at least n; otherwise pull as many bytes as possible into st->bits
#define jxsml__refill(st, n) (st->nbits < (n) ? jxsml__always_refill(st) : st->err)

inline int jxsml__zero_pad_to_byte(jxsml__st *st) {
	int32_t n = st->nbits & 7;
	if (st->bits & ((1u << n) - 1)) return JXSML__ERR("pad0");
	st->bits >>= n;
	st->nbits -= n;
	return st->err;
}

inline int32_t jxsml__u(jxsml__st *st, int32_t n) {
	int32_t ret;
#ifdef JXSML_DEBUG
	if (n < 0 || n > 31) return JXSML__ERR("!exp"), 0;
#endif
	if (jxsml__refill(st, n)) return 0;
	ret = (int32_t) (st->bits & ((1u << n) - 1));
	st->bits >>= n;
	st->nbits -= n;
	return ret;
}

// the maximum value U32() actually reads is 2^30 + 4211711, so int32_t should be enough
inline int32_t jxsml__u32(jxsml__st *st, int32_t o0, int32_t n0, int32_t o1, int32_t n1, int32_t o2, int32_t n2, int32_t o3, int32_t n3) {
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

inline int32_t jxsml__enum(jxsml__st *st) {
	int32_t ret = jxsml__u32(st, 0, 0, 1, 0, 2, 4, 18, 6);
	// the spec says it should be 64, but the largest enum value in use is 18 (kHLG);
	// we have to reject unknown enum values anyway so we use a smaller limit to avoid overflow
	if (ret >= 31) return JXSML__ERR("enum"), 0;
	return ret;
}

inline float jxsml__f16(jxsml__st *st) {
	int32_t bits = jxsml__u(st, 16);
	int32_t biased_exp = (bits >> 10) & 0x1f;
	if (biased_exp == 31) return JXSML__ERR("!fin"), 0.0f;
	return (bits >> 15 ? -1 : 1) * ldexpf((float) ((bits & 0x3ff) | (biased_exp > 0 ? 0x400 : 0)), biased_exp - 25);
}

uint64_t jxsml__varint(jxsml__st *st) { // ICC only
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

inline int32_t jxsml__u8(jxsml__st *st) { // ANS distribution decoding only
	if (jxsml__u(st, 1)) {
		int32_t n = jxsml__u(st, 3);
		return jxsml__u(st, n) + (1 << n);
	} else {
		return 0;
	}
}

// equivalent to u(ceil(log2(max + 1))), decodes [0, max] with the minimal number of bits
inline int32_t jxsml__at_most(jxsml__st *st, int32_t max) {
	// TODO: provide a portable version
	int32_t v = max > 0 ? jxsml__u(st, 32 - __builtin_clz((unsigned) max)) : 0;
	if (v > max) return JXSML__ERR("rnge"), 0;
	return v;
}

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

int32_t jxsml__match_overflow(jxsml__st *st, int32_t fast_len, int32_t start, const int32_t *table) {
	int32_t entry, code, code_len;
	st->nbits -= fast_len;
	st->bits >>= fast_len;
	do {
		entry = table[start++];
		code = (entry >> 4) & 0xfff;
		code_len = entry & 15;
	} while (code != (int32_t) (st->bits & ((1u << code_len) - 1)));
	return entry;
}

inline int32_t jxsml__prefix_code(jxsml__st *st, int32_t fast_len, int32_t max_len, const int32_t *table) {
	int32_t entry, code_len;
	if (jxsml__refill(st, max_len)) return 0;
	entry = table[st->bits & ((1u << fast_len) - 1)];
	if (entry < 0 && fast_len < max_len) entry = jxsml__match_overflow(st, fast_len, -entry, table);
	code_len = entry & 15;
	st->nbits -= code_len;
	st->bits >>= code_len;
	return entry >> 16;
}

// read a prefix code tree, as specified in RFC 7932 section 3
int jxsml__init_prefix_code(jxsml__st *st, int32_t l2size, int32_t *out_fast_len, int32_t *out_max_len, int32_t **out_table) {
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

	hskip = jxsml__u(st, 2);
	if (hskip == 1) { // simple prefix codes (section 3.4)
		static const struct { int8_t maxlen, len[8], symref[8]; } TEMPLATES[5] = {
			{ 3, {1,2,1,3,1,2,1,3}, {0,1,0,2,0,1,0,3} }, // NSYM=4 tree-select 1 (1233)
			{ 0, {0}, {0} },                             // NSYM=1 (0)
			{ 1, {1,1}, {0,1} },                         // NSYM=2 (11)
			{ 2, {1,2,1,2}, {0,1,0,2} },                 // NSYM=3 (122)
			{ 2, {2,2,2,2}, {0,1,2,3} },                 // NSYM=4 tree-select 0 (2222)
		};
		int32_t nsym = jxsml__u(st, 2) + 1, syms[4];
		for (i = 0; i < nsym; ++i) {
			syms[i] = jxsml__at_most(st, l2size - 1);
			for (j = 0; j < i; ++j) JXSML__SHOULD(syms[i] != syms[j], "hufd");
		}
		if (nsym == 4 && jxsml__u(st, 1)) nsym = 0; // tree-select
		JXSML__RAISE_DELAYED();
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
				rep = (prev_rep < 0 ? 8 * prev_rep + 15 : -3) - jxsml__u(st, 3);
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
		JXSML__SHOULD(l2table = malloc(sizeof(int32_t) * (size_t) size_used), "!mem");
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
			// conversely, if the *next* code will be such a code,
			// this is the last overflow entry so we can make it zero length code to always match
			if (*start & ((L2CODESUM >> fast_len) - 1)) {
				l2table[l2overflows[n]++] = (i << 16) | (code >> fast_len << 4) | (n - fast_len);
			} else {
				l2table[l2overflows[n]++] = (i << 16);
			}
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

// hybrid integer encoding:
// - token < 2^split_exp is interpreted as is.
// - otherwise (token - 2^split_exp) is split into NNNHHHLLL where config determines H/L lengths.
//   then MMM = u(NNN + split_exp) is read; the decoded value is 1HHHMMMLLL.
typedef struct { int8_t split_exp, msb_in_token, lsb_in_token; } jxsml__hybrid_int_config_t;

int jxsml__hybrid_int_config(jxsml__st *st, int32_t log_alphabet_size, jxsml__hybrid_int_config_t *out) {
	out->split_exp = (int8_t) jxsml__at_most(st, log_alphabet_size);
	if (out->split_exp != log_alphabet_size) {
		out->msb_in_token = (int8_t) jxsml__at_most(st, out->split_exp);
		out->lsb_in_token = (int8_t) jxsml__at_most(st, out->split_exp - out->msb_in_token);
	} else {
		out->msb_in_token = out->lsb_in_token = 0;
	}
	return st->err;
}

inline int32_t jxsml__hybrid_int(jxsml__st *st, int32_t token, const jxsml__hybrid_int_config_t *config) {
	int32_t midbits, lo, mid, hi, top, split = 1 << config->split_exp;
	if (token < split) return token;
	midbits = config->split_exp + ((token - split) >> (config->msb_in_token + config->lsb_in_token));
	// TODO midbits can overflow!
	top = (int32_t) 1 << config->msb_in_token;
	lo = token & (((int32_t) 1 << config->lsb_in_token) - 1);
	mid = jxsml__u(st, midbits);
	hi = (token >> config->lsb_in_token) & (top - 1);
	return ((top | hi) << (midbits + config->lsb_in_token)) | ((mid << config->lsb_in_token) | lo);
}

enum { JXSML__MAX_DIST = 41 + 1 }; // includes a synthetic LZ77 length distribution
enum { JXSML__DIST_BITS = 12 };

// the alias table of size N is conceptually an array of N buckets with probability 1/N each,
// where each bucket corresponds to at most two symbols distinguished by the cutoff point.
// this is done by rearranging symbols so that every symbol boundary falls into distinct buckets.
// so it allows *any* distribution of N symbols to be decoded in a constant time after the setup.
// the table is not unique though, so the spec needs to specify the exact construction algorithm.
//
//   input range: 0     cutoff         bucket_size
//                +------------------------------+
// output symbol: |   i   |        symbol        | <- bucket i
//                +------------------------------+
//  output range: offset        offset+bucket_size
typedef struct { int16_t cutoff, offset, symbol; } jxsml__alias_bucket;

jxsml__alias_bucket *jxsml__init_alias_map(jxsml__st *st, const int16_t *D, int32_t log_alphabet_size) {
	int16_t log_bucket_size = (int16_t) (JXSML__DIST_BITS - log_alphabet_size);
	int16_t bucket_size = (int16_t) 1 << log_bucket_size;
	// the underfull and overfull stacks are implicit linked lists; u/o resp. is the top index,
	// buckets[u/o].next is the second-to-top index and so on. an index -1 indicates the bottom.
	int16_t u = -1, o = -1, i, j;

	jxsml__alias_bucket *buckets = malloc(sizeof(jxsml__alias_bucket) << log_alphabet_size);
	if (!buckets) return NULL;

	for (i = j = 0; i < (1 << log_alphabet_size); ++i) {
		if (D[i] && ++j == 2) break;
	}
	if (j == 1) { // D[i] is the only non-zero probability
		for (j = 0; j < (1 << log_alphabet_size); ++j) {
			buckets[j].symbol = i;
			buckets[j].offset = j << log_bucket_size;
			buckets[j].cutoff = 0;
		}
		return buckets;
	}

	// each bucket is either settled (fields fully set) or unsettled (only `cutoff` is set).
	// unsettled buckets are either in the underfull stack, in which case `cutoff < bucket_size`,
	// or in the overfull stack, in which case `cutoff > bucket_size`. other fields are left
	// unused, so `symbol` in settled buckets is aliased to `next` in unsettled buckets.
	// when rearranging results in buckets with `cutoff == bucket_size`,
	// final fields are set and they become settled; eventually every bucket has to be settled.
	#define next symbol
	for (i = 0; i < (1 << log_alphabet_size); ++i) {
		int16_t cutoff = D[i];
		buckets[i].cutoff = cutoff;
		if (cutoff > bucket_size) {
			buckets[i].next = o;
			o = i;
		} else if (cutoff < bucket_size) {
			buckets[i].next = u;
			u = i;
		} else { // immediately settled
			buckets[i].symbol = i;
			buckets[i].offset = 0;
		}
	}

	while (o >= 0) {
		int16_t by, tmp;
#ifdef JXSML__DEBUG
		if (u < 0) {
			free(buckets);
			return JXSML__ERR("!exp"), NULL;
		}
#endif
		by = bucket_size - buckets[u].cutoff;
		// move the input range [cutoff[o] - by, cutoff[o]] of the bucket o into
		// the input range [cutoff[u], bucket_size] of the bucket u (which is settled after this)
		tmp = buckets[u].next;
		buckets[o].cutoff -= by;
		buckets[u].symbol = o;
		buckets[u].offset = buckets[o].cutoff - buckets[u].cutoff;
		u = tmp;
		if (buckets[o].cutoff < bucket_size) { // o is now underfull, move to the underfull stack
			tmp = o;
			buckets[tmp].next = u;
			o = buckets[tmp].next;
			u = tmp;
		} else if (buckets[o].cutoff == bucket_size) { // o is also settled
			buckets[o].offset = 0;
			o = buckets[o].next;
		}
	}
	#undef next

	(void) st;
	return buckets;
}

enum { JXSML__ANS_FINAL_STATE = 0x130000 };

int32_t jxsml__ans_code(jxsml__st *st, uint32_t *state, int32_t log_bucket_size, const int16_t *D, const jxsml__alias_bucket *aliases) {
	int32_t index = (int32_t) (*state & 0xfff);
	int32_t i = index >> log_bucket_size;
	int32_t pos = index & ((1 << log_bucket_size) - 1);
	const jxsml__alias_bucket *bucket = &aliases[i];
	int32_t symbol = pos < bucket->offset ? i : bucket->symbol;
	*state = (uint32_t) D[symbol] * (*state >> 12) + (uint32_t) bucket->offset + (uint32_t) pos;
	if (*state < (1u << 16)) *state = (*state << 16) | (uint32_t) jxsml__u(st, 16);
	return symbol;
}

typedef union {
	struct {
		uint32_t state;
		int32_t log_bucket_size;
		int16_t *D;
		jxsml__alias_bucket *aliases;
	} ans; // if parent use_prefix_code is false
	struct {
		int32_t fast_len, max_len, *table;
	} prefix; // if parent use_prefix_code is true
} jxsml__entropy_code_cluster_t;

int jxsml__entropy_code_cluster(jxsml__st *st, int use_prefix_code, jxsml__entropy_code_cluster_t *cluster) {
	if (use_prefix_code) {
		return jxsml__prefix_code(st, cluster->prefix.fast_len, cluster->prefix.max_len, cluster->prefix.table);
	} else {
		return jxsml__ans_code(st, &cluster->ans.state, cluster->ans.log_bucket_size, cluster->ans.D, cluster->ans.aliases);
	}
}

typedef struct {
	int32_t num_dist;
	int lz77_enabled, use_prefix_code;
	int32_t min_symbol, min_length;
	int32_t num_clusters; // in [1, num_dist]
	int8_t cluster_map[JXSML__MAX_DIST]; // each in [0, num_clusters)

	// the following values are indexed by the distribution index *before* cluster mapping
	jxsml__hybrid_int_config_t lz_len_config, configs[JXSML__MAX_DIST];
	jxsml__entropy_code_cluster_t clusters[JXSML__MAX_DIST]; // also contains per-cluster states

	// LZ77 states
	int32_t num_to_copy, copy_pos, num_decoded;
	int32_t *window;
} jxsml__entropy_code_t;

// aka DecodeHybridVarLenUint
int jxsml__entropy_code(jxsml__st *st, int32_t ctx, int32_t dist_mult, jxsml__entropy_code_t *code) {
	int32_t token, distance;

	if (code->num_to_copy > 0) {
continue_lz77:
		--code->num_to_copy;
		return code->window[code->num_decoded++ & 0xfffff] = code->window[code->copy_pos++ & 0xfffff];
	}

#ifdef JXSML__DEBUG
	if (ctx >= code->num_dist) return JXSML__ERR("!exp"), 0;
#endif
	token = jxsml__entropy_code_cluster(st, code->use_prefix_code, &code->clusters[ctx]);
	if (token >= code->min_symbol) { // this is large enough if lz77_enabled is false
		code->num_to_copy = jxsml__hybrid_int(st, token - code->min_symbol, &code->lz_len_config) + code->min_length;
		token = jxsml__entropy_code_cluster(st, code->use_prefix_code, &code->clusters[code->num_dist - 1]);
		distance = jxsml__hybrid_int(st, token, &code->configs[code->num_dist - 1]);
		if (st->err) return 0;
		if (!dist_mult) {
			++distance;
		} else if (distance >= 120) {
			distance -= 120;
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

	token = jxsml__hybrid_int(st, token, &code->configs[ctx]);
	if (st->err) return 0;
	if (code->lz77_enabled) code->window[code->num_decoded++ & 0xfffff] = token;
	return token;
}

int jxsml__init_entropy_code(jxsml__st *st, int32_t num_dist, jxsml__entropy_code_t *out) {
	jxsml__hybrid_int_config_t configs[JXSML__MAX_DIST];
	jxsml__entropy_code_cluster_t clusters[JXSML__MAX_DIST] = {0};
	int32_t i, j;

	JXSML__ASSERT(num_dist <= JXSML__MAX_DIST);

	out->use_prefix_code = 0;
	out->window = NULL;

	// LZ77Params
	out->lz77_enabled = jxsml__u(st, 1);
	if (out->lz77_enabled) {
		out->min_symbol = jxsml__u32(st, 224, 0, 512, 0, 4096, 0, 8, 15);
		out->min_length = jxsml__u32(st, 3, 0, 4, 0, 5, 2, 9, 8);
		JXSML__TRY(jxsml__hybrid_int_config(st, 8, &out->lz_len_config));
		++num_dist; // num_dist - 1 is a synthesized LZ77 length distribution
		JXSML__SHOULD(out->window = malloc(sizeof(int32_t) << 20), "!mem");
	}

	// cluster_map: a mapping from non-clustered dist. to clustered dist.
	if (num_dist > 1) {
		uint64_t clusters_seen;

		if (jxsml__u(st, 1)) { // is_simple (# clusters < 8)
			int32_t nbits = jxsml__u(st, 2);
			for (i = 0; i < num_dist; ++i) out->cluster_map[i] = (int8_t) jxsml__u(st, nbits);
		} else {
			//int32_t use_mtf = jxsml__u(st, 1);
			// TODO ensure every number is < 64
			//jxsml__entropy_t nested_entropy;
			//JXSML__TRY(jxsml__entropy_code(st, 1, &nested_entropy));
			JXSML__RAISE("TODO: nested entropy code");
		}

		// verify cluster_map and determine the implicit num_clusters
		clusters_seen = 0;
		for (i = 0; i < num_dist; ++i) clusters_seen |= (uint64_t) 1 << out->cluster_map[i];
		JXSML__ASSERT(clusters_seen != 0);
		JXSML__SHOULD((clusters_seen & (clusters_seen + 1)) == 0, "clst");
		for (out->num_clusters = 0; clusters_seen; ++out->num_clusters) clusters_seen >>= 1;
	} else {
		out->num_clusters = 1;
		out->cluster_map[0] = 0;
	}

	out->use_prefix_code = jxsml__u(st, 1);
	if (out->use_prefix_code) {
		int32_t count[JXSML__MAX_DIST];

		for (i = 0; i < out->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, 15, &configs[i]));
		}

		for (i = 0; i < out->num_clusters; ++i) {
			if (jxsml__u(st, 1)) {
				int32_t n = jxsml__u(st, 4);
				count[i] = 1 + (1 << n) + jxsml__u(st, n);
				JXSML__SHOULD(count[i] <= (1 << 15), "hufd");
			} else {
				count[i] = 1;
			}
		}

		for (i = 0; i < out->num_clusters; ++i) {
			jxsml__entropy_code_cluster_t *c = &clusters[i];
			JXSML__TRY(jxsml__init_prefix_code(st, count[i], &c->prefix.fast_len, &c->prefix.max_len, &c->prefix.table));
		}
	} else {
		enum { DISTBITS = JXSML__DIST_BITS, DISTSUM = 1 << DISTBITS };
		int32_t log_alphabet_size = 5 + jxsml__u(st, 2);
		for (i = 0; i < out->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, log_alphabet_size, &out->configs[i]));
		}

		for (i = 0; i < out->num_clusters; ++i) {
			int32_t table_size = 1 << log_alphabet_size;
			int32_t D[table_size]; // TODO eliminate VLA
			for(j=0;j<table_size;++j)D[j]=0;

			switch (jxsml__u(st, 2)) {
			case 1: // one entry
				D[jxsml__u8(st)] = DISTSUM;
				break;

			case 3: { // two entries
				int32_t v1 = jxsml__u8(st);
				int32_t v2 = jxsml__u8(st);
				JXSML__SHOULD(v1 != v2 && v1 < table_size && v2 < table_size, "ansd");
				D[v1] = jxsml__u(st, DISTBITS);
				D[v2] = DISTSUM - D[v1];
				break;
			}

			case 2: { // evenly distribute to first `alphabet_size` entries (false -> true)
				int32_t alphabet_size = jxsml__u8(st) + 1;
				int32_t d = DISTSUM / alphabet_size;
				int32_t bias_size = DISTSUM - d * alphabet_size;
				for (j = 0; j < bias_size; ++j) D[j] = d + 1;
				for (; j < alphabet_size; ++j) D[j] = d;
				break;
			}

			case 0: { // bit counts + RLE (false -> false)
				int32_t len, shift, alphabet_size, omit_log, omit_pos, code, total, n;
				int32_t ncodes, codes[259]; // # bits to read if >= 0, minus repeat count if < 0

				len = jxsml__u(st, 1) ? jxsml__u(st, 1) ? jxsml__u(st, 1) ? 3 : 2 : 1 : 0;
				shift = jxsml__u(st, len) + (1 << len) - 1;
				JXSML__SHOULD(shift <= 13, "ansd");
				alphabet_size = jxsml__u8(st) + 3;

				omit_log = -1; // there should be at least one non-RLE code
				for (j = ncodes = 0; j < alphabet_size; ) {
					static const int32_t TABLE[] = {
						0xa0003,     -16, 0x70003, 0x30004, 0x60003, 0x80003, 0x90003, 0x50004,
						0xa0003, 0x40004, 0x70003, 0x10004, 0x60003, 0x80003, 0x90003, 0x20004,
						0x00011, 0xb0022, 0xc0003, 0xd0000, // overflow for ...0001
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
				JXSML__SHOULD(j == alphabet_size && omit_log >= 0, "ansd");

				omit_pos = -1;
				for (j = n = total = 0; j < ncodes; ++j) {
					code = codes[j];
					if (code < 0) { // repeat
						int32_t prev = n > 0 ? D[n - 1] : 0;
						JXSML__SHOULD(prev >= 0, "ansd"); // implicit D[n] followed by RLE
						while (code++ < 0) total += D[n++] = prev;
					} else if (code == omit_log) { // the first longest D[n] is "omitted" (implicit)
						omit_pos = n;
						omit_log = -1; // this branch runs at most once
						D[n++] = -1;
					} else if (code < 2) {
						total += D[n++] = code;
					} else {
						int32_t bitcount;
						--code;
						bitcount = shift - ((DISTBITS - code) >> 1);
						if (bitcount < 0) bitcount = 0;
						if (bitcount > code) bitcount = code;
						total += D[n++] = (1 << code) + (jxsml__u(st, bitcount) << (code - bitcount));
					}
				}
				JXSML__ASSERT(omit_pos >= 0);
				JXSML__SHOULD(total <= DISTSUM, "ansd");
				D[omit_pos] = DISTSUM - total;
				break;
			}

			default: JXSML__UNREACHABLE();
			}

			JXSML__RAISE_DELAYED();
		}

		JXSML__RAISE("TODO: alias mapping needed");
	}

	// permute out->configs/clusters so that we don't need to consult out->cluster_map directly
	for (i = 0; i < num_dist; ++i) {
		out->configs[i] = configs[out->cluster_map[i]];
		out->clusters[i] = clusters[out->cluster_map[i]];
	}
	return 0;

error:
	for (i = 0; i < num_dist; ++i) {
		if (out->use_prefix_code) {
			free(clusters[i].prefix.table);
		} else {
			/*
			free(clusters[i].ans.D);
			free(clusters[i].ans.aliases);
			*/
		}
	}
	free(out->window);
	return st->err;
}

void jxsml__free_entropy_code(jxsml__entropy_code_t *code) {
}

int jxsml__size_header(jxsml__st *st, int32_t *outw, int32_t *outh) {
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

int jxsml__bit_depth(jxsml__st *st, int32_t *outbpp, int32_t *outexpbits) {
	if (jxsml__u(st, 1)) { // float_sample
		*outbpp = jxsml__u32(st, 32, 0, 16, 0, 24, 0, 1, 6);
		*outexpbits = jxsml__u(st, 4) + 1;
	} else {
		*outbpp = jxsml__u32(st, 8, 0, 10, 0, 12, 0, 1, 6);
		*outexpbits = 0;
	}
	return st->err;
}

#define JXSML__TOSIGNED(u) ((u) & 1 ? -((u) + 1) / 2 : (u) / 2)

int jxsml__customxy(jxsml__st *st, float xy[2]) {
	int32_t ux = jxsml__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21);
	int32_t uy = jxsml__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21);
	xy[0] = (float) JXSML__TOSIGNED(ux) / 1000000.0f;
	xy[1] = (float) JXSML__TOSIGNED(uy) / 1000000.0f;
	return st->err;
}

int jxsml__extensions(jxsml__st *st) {
	uint64_t extensions = jxsml__u64(st);
	//int32_t i;
	JXSML__SHOULD(extensions == 0, "TODO: arbitrary skip required");
	/*
	for (i = 0; i < 64; ++i) {
		if (extensions >> i & 1) nbits = jxsml__u64(st);
	}
	for (i = 0; i < 64; ++i) {
	// read nbits
	}
	*/
error:
	return st->err;
}

int jxsml__icc(jxsml__st *st) {
	size_t enc_size, index;
	jxsml__entropy_code_t entropy = {0};
	int32_t byte = 0, prev = 0, pprev = 0, ctx;

	//JXSML__TRY(jxsml__zero_pad_to_byte(st)); // XXX libjxl doesn't have this???
	enc_size = jxsml__u64(st);
	JXSML__TRY(jxsml__init_entropy_code(st, 41, &entropy));

	for (index = 0; index < enc_size; ++index) {
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
		pprev = prev;
		prev = byte;
		byte = jxsml__entropy_code(st, ctx, 0, &entropy);
		fprintf(stderr, "ctx=%d byte=%#x %c\n", ctx, (int)byte, 0x20 <= byte && byte < 0x7f ? byte : ' ');
		JXSML__RAISE_DELAYED();
	}

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
	jxsml__free_entropy_code(&entropy);
	return st->err;
}

void end(const jxsml img, const jxsml__st st) {}

int jxsml_load_from_memory(jxsml *out, const void *buf, size_t bufsize) {
	static const float SRGB_CHROMA[4][2] = { // default chromacity (kD65, kSRGB)
		{0.3127f, 0.3290f}, {0.639998686f, 0.330010138f},
		{0.300003784f, 0.600003357f}, {0.150002046f, 0.059997204f},
	};

	jxsml__st st_ = {
		.out = out,
		.buf = (const uint8_t *) buf,
		.bufsize = bufsize,
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
	int32_t i;

	JXSML__SHOULD(st->bufsize >= 2, "shrt");
	JXSML__SHOULD(st->buf[0] == 0xff && st->buf[1] == 0x0a, "!jxl");
	st->buf += 2;
	st->bufsize -= 2;

	// SizeHeader
	JXSML__TRY(jxsml__size_header(st, &st->out->width, &st->out->height));

	// ImageMetadata
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
		for (i = 0; i < st->num_extra_channels; ++i) {
			JXSML__RAISE("TODO: ec_info");
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
			JXSML__RAISE("TODO: tone_mapping");
		}
		JXSML__TRY(jxsml__extensions(st));
	}
	if (!jxsml__u(st, 1)) { // !default_m
		int32_t cw_mask, i, j;
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

	// icc
	if (st->want_icc) {
		JXSML__TRY(jxsml__icc(st));
	}

	end(*st->out, st_);
	return 0;

error:
	return st->err;
}

int main(int argc, char **argv) {
	if (argc < 2) return 1;
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
	int ret = jxsml_load_from_memory(&img, buf, (size_t) bufsize);
	if (ret) {
		fprintf(stderr, "error: %c%c%c%c\n", ret >> 24 & 0xff, ret >> 16 & 0xff, ret >> 8 & 0xff, ret & 0xff);
		return 1;
	}
	fprintf(stderr, "ok\n");
	return 0;
}
