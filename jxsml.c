// JXSML: An extra small JPEG XL decoder
// Kang Seonghoon, 2022-05, Public Domain (CC0)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <stdio.h>

enum {
	JXSML_CHROMA_WHITE = 0, JXSML_CHROMA_RED = 1,
	JXSML_CHROMA_GREEN = 2, JXSML_CHROMA_BLUE = 3,
};

typedef struct {
	uint32_t width, height;
	enum {
		JXSML_ORIENT_TL = 1, JXSML_ORIENT_TR = 2, JXSML_ORIENT_BR = 3, JXSML_ORIENT_BL = 4,
		JXSML_ORIENT_LT = 5, JXSML_ORIENT_RT = 6, JXSML_ORIENT_RB = 7, JXSML_ORIENT_LB = 8,
	} orientation;
	uint32_t intr_width, intr_height; // 0 if not specified
	int bpp, exp_bits;

	uint32_t anim_tps_num, anim_tps_denom; // num=denom=0 if not animated
	uint32_t anim_nloops; // 0 if infinity
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
#define JXSML__ASSERT(cond) JXSML__SHOULD(cond, "!exp")
#define JXSML__UNREACHABLE() JXSML__ASSERT(0)

int jxsml__always_refill(jxsml__st *st) {
	size_t consumed = (32 - st->nbits) >> 3;
	if (st->bufsize < consumed) return JXSML__ERR("shrt");
	st->bufsize -= consumed;
	do {
		st->bits |= *st->buf++ << st->nbits;
		st->nbits += 8;
	} while (st->nbits <= 24);
	return st->err;
}

// ensure st->nbits is at least n; otherwise pull as many bytes as possible into st->bits
#define jxsml__refill(st, n) (st->nbits < (n) ? jxsml__always_refill(st) : st->err)

inline int jxsml__zero_pad_to_byte(jxsml__st *st) {
	int n = st->nbits & 7;
	if (st->bits & ((1 << n) - 1)) return JXSML__ERR("pad0");
	st->bits >>= n;
	st->nbits -= n;
	return st->err;
}

inline uint32_t jxsml__u(jxsml__st *st, int n) {
	uint32_t ret;
	if (st->err || jxsml__refill(st, n)) return 0;
	ret = st->bits & ((1 << n) - 1);
	st->bits >>= n;
	st->nbits -= n;
	return ret;
}

inline uint32_t jxsml__u32(jxsml__st *st, uint32_t o0, int n0, uint32_t o1, int n1, uint32_t o2, int n2, uint32_t o3, int n3) {
	const uint32_t o[4] = { o0, o1, o2, o3 };
	const int n[4] = { n0, n1, n2, n3 };
	int sel = jxsml__u(st, 2);
	return jxsml__u(st, n[sel]) + o[sel];
}

uint64_t jxsml__u64(jxsml__st *st) {
	int sel = jxsml__u(st, 2), shift;
	uint64_t ret = jxsml__u(st, sel * 4);
	if (sel < 3) {
		ret += 17 >> (8 - sel * 4);
	} else {
		for (shift = 12; shift < 64 && jxsml__u(st, 1); shift += 8) {
			ret |= (uint64_t) jxsml__u(st, shift < 56 ? 8 : 64 - shift) << shift;
		}
	}
	return ret;
}

inline int jxsml__enum(jxsml__st *st) {
	int ret = jxsml__u32(st, 0, 0, 1, 0, 2, 4, 18, 6);
	// this should really be 64, but this is more than enough and avoids unintentional overflow
	if (ret >= 31) return JXSML__ERR("enum"), 0;
	return ret;
}

inline float jxsml__f16(jxsml__st *st) {
	int bits = jxsml__u(st, 16);
	int biased_exp = (bits >> 10) & 0x1f;
	if (biased_exp == 31) return JXSML__ERR("!fin"), 0;
	return (bits >> 15 ? -1 : 1) * ldexpf((bits & 0x3ff) | (biased_exp > 0 ? 0x400 : 0), biased_exp - 25);
}

uint64_t jxsml__varint(jxsml__st *st) { // ICC only
	uint64_t value = 0;
	int shift = 0;
	do {
		if (st->bufsize == 0) return JXSML__ERR("shrt"), 0;
		int b = jxsml__u(st, 8);
		value |= (b & 0x7f) << shift;
		if (b < 128) return value;
		shift += 7;
	} while (shift < 63);
	return JXSML__ERR("vint"), 0;
}

inline uint8_t jxsml__u8(jxsml__st *st) { // ANS distribution decoding only
	if (jxsml__u(st, 1)) {
		int n = jxsml__u(st, 3);
		return jxsml__u(st, n) + (1 << n);
	} else {
		return 0;
	}
}

// equivalent to u(ceil(log2(max + 1))), decodes [0, max] with the minimal number of bits
inline uint32_t jxsml__at_most(jxsml__st *st, uint32_t max) {
	// TODO: provide a portable version
	uint32_t v = max > 0 ? jxsml__u(st, 32 - __builtin_clz(max)) : 0;
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

int32_t jxsml__match_overflow(jxsml__st *st, int fast_len, int start, const int32_t *table) {
	int32_t entry, code, code_len;
	st->nbits -= fast_len;
	st->bits >>= fast_len;
	do {
		entry = table[start++];
		code = (entry >> 4) & 0xfff;
		code_len = entry & 15;
	} while ((uint32_t) code != (st->bits & ((1 << code_len) - 1)));
	return entry;
}

inline int jxsml__prefix_code(jxsml__st *st, int fast_len, int max_len, const int32_t *table) {
	int32_t entry, code_len;
	if (jxsml__refill(st, max_len)) return 0;
	entry = table[st->bits & ((1 << fast_len) - 1)];
	if (entry < 0 && fast_len < max_len) entry = jxsml__match_overflow(st, fast_len, -entry, table);
	code_len = entry & 15;
	st->nbits -= code_len;
	st->bits >>= code_len;
	return entry >> 16;
}

// read a prefix code tree, as specified in RFC 7932 section 3
int jxsml__init_prefix_code(jxsml__st *st, int l2size, int *out_fast_len, int *out_max_len, int32_t **out_table) {
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

	int l1lengths[L1SIZE] = {0}, *l2lengths = NULL;
	int l1counts[L1MAXLEN + 1] = {0}, l2counts[L2MAXLEN + 1] = {0};
	int l1starts[L1MAXLEN + 1], l2starts[L2MAXLEN + 1], l2overflows[L2MAXLEN + 1];
	int l1table[1 << L1MAXLEN] = {0}, *l2table = NULL;
	int total, code, hskip, fast_len, i, j;

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
		int nsym = jxsml__u(st, 2) + 1, syms[4];
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
			int n = l1lengths[i], *start = &l1starts[n];
			if (n == 0) continue;
			for (code = JXSML__REV5[*start]; code < L1CODESUM; code += 1 << n) l1table[code] = (i << 16) | n;
			*start += L1CODESUM >> n;
		}
	}

	{ // read layer 2 code lengths using the layer 1 code
		int prev = 8, rep, prev_rep = 0; // prev_rep: prev repeat count of 16(pos)/17(neg) so far
		JXSML__SHOULD(l2lengths = calloc(l2size, sizeof(int)), "!mem");
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
		JXSML__SHOULD(l2table = malloc(sizeof(int) << fast_len), "!mem");
	} else {
		// if the distribution is flat enough the max fast_len might be slow
		// because most LUT entries will be overflow refs so we will hit slow paths for most cases.
		// we therefore calculate the table size with the max fast_len,
		// then find the largest fast_len within the specified table growth factor.
		int size, size_limit, size_used;
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
		JXSML__SHOULD(l2table = malloc(sizeof(int) * size_used), "!mem");
	}

	// fill the layer 2 table
	for (i = 0; i < l2size; ++i) {
		int n = l2lengths[i], *start = &l2starts[n];
		if (n == 0) continue;
		code = (int) JXSML__REV5[*start & 31] << 10 |
			(int) JXSML__REV5[*start >> 5 & 31] << 5 |
			(int) JXSML__REV5[*start >> 10];
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

typedef struct { int split_exp, msb_in_token, lsb_in_token; } jxsml__hybrid_int_config_t;
int jxsml__hybrid_int_config(jxsml__st *st, int log_alphabet_size, jxsml__hybrid_int_config_t *out) {
	out->split_exp = jxsml__at_most(st, log_alphabet_size);
	if (out->split_exp != log_alphabet_size) {
		out->msb_in_token = jxsml__at_most(st, out->split_exp);
		out->lsb_in_token = jxsml__at_most(st, out->split_exp - out->msb_in_token);
	} else {
		out->msb_in_token = out->lsb_in_token = 0;
	}
	// TODO what happens if split_exp > log_alphabet_size?
	return st->err;
}

enum { JXSML__MAX_DIST = 41 + 1 }; // includes a synthetic LZ77 length distribution

typedef struct {
	int num_dist;
	int lz77_enabled, min_symbol, min_length;
	int num_clusters;
	uint8_t clusters[JXSML__MAX_DIST];
	jxsml__hybrid_int_config_t lz_len_config, configs[JXSML__MAX_DIST];
	int use_prefix_code;
	uint16_t *D; // when use_prefix_code is false

	// prefix code only:
	

} jxsml__entropy_code_t;

// aka DecodeHybridVarLenUint
int jxsml__entropy_code_internal(jxsml__st *st, int cluster, jxsml__entropy_code_t *code) {
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

	int token;

	if (lst->num_to_copy > 0) {
		--lst->num_to_copy;
		return lst->window[lst->num_decoded++ & 0xfffff] = lst->window[lst->copy_pos++ & 0xfffff];
	}

	if (code->use_prefix_code) {
		token = jxsml__prefix_code(st, code->prefix_fast_len, code->prefix_max_len, code->prefix_table);
	} else {
		token = 
	}
}

int jxsml__init_alias_map(jxsml__st *st, const uint16_t *D, int log_alphabet_size) {
	int log_bucket_size = 12 - log_alphabet_size;
	int bucket_size = 1 << log_bucket_size;

	if (/*D has a single number at index i*/0) {
		for (j = 0; j < (1 << 12); ++j) {
			symbols[j] = i;
			offsets[j] = j << log_bucket_size;
			cutoffs[j] = 0;
		}
		return;
	}

	
}

int jxsml__init_entropy_code(jxsml__st *st, int num_dist, jxsml__entropy_code_t *out) {
	int i, j;

	JXSML__ASSERT(num_dist <= JXSML__MAX_DIST);

	// LZ77Params
	out->lz77_enabled = jxsml__u(st, 1);
	if (out->lz77_enabled) {
		out->min_symbol = jxsml__u32(st, 224, 0, 512, 0, 4096, 0, 8, 15);
		out->min_length = jxsml__u32(st, 3, 0, 4, 0, 5, 2, 9, 8);
		JXSML__TRY(jxsml__hybrid_int_config(st, 8, &out->lz_len_config));
		++num_dist; // num_dist - 1 is a synthesized LZ77 length distribution
	}

	// clusters: a mapping from non-clustered dist. to clustered dist.
	if (num_dist > 1) {
		uint64_t clusters_seen;

		if (jxsml__u(st, 1)) { // is_simple (# clusters < 8)
			int nbits = jxsml__u(st, 2);
			for (i = 0; i < num_dist; ++i) out->clusters[i] = jxsml__u(st, nbits);
		} else {
			int use_mtf = jxsml__u(st, 1);
			// TODO ensure every number is < 64
			//jxsml__entropy_t nested_entropy;
			//JXSML__TRY(jxsml__entropy_code(st, 1, &nested_entropy));
			JXSML__RAISE("TODO: inverse MTF");
		}

		// verify clusters and determine the implicit num_clusters
		clusters_seen = 0;
		for (i = 0; i < num_dist; ++i) clusters_seen |= (uint64_t) 1 << out->clusters[i];
		JXSML__ASSERT(clusters_seen != 0);
		JXSML__SHOULD((clusters_seen & (clusters_seen + 1)) == 0, "clst");
		for (out->num_clusters = 0; clusters_seen; ++out->num_clusters) clusters_seen >>= 1;
	} else {
		out->num_clusters = 1;
		out->clusters[0] = 0;
	}

	out->use_prefix_code = jxsml__u(st, 1);
	if (out->use_prefix_code) {
		int count[JXSML__MAX_DIST], fast_len[JXSML__MAX_DIST], max_len[JXSML__MAX_DIST];
		int32_t *table[JXSML__MAX_DIST] = {0};

		for (i = 0; i < out->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, 15, &out->configs[i]));
		}

		for (i = 0; i < out->num_clusters; ++i) {
			if (jxsml__u(st, 1)) {
				int n = jxsml__u(st, 4);
				count[i] = 1 + (1 << n) + jxsml__u(st, n);
				JXSML__SHOULD(count[i] <= (1 << 15), "hufd");
			} else {
				count[i] = 1;
			}
		}

		for (i = 0; i < out->num_clusters; ++i) {
			JXSML__TRY(jxsml__init_prefix_code(st, count[i], &fast_len[i], &max_len[i], &table[i]));
		}

		JXSML__RAISE("TODO: use_prefix_code");

		// RFC 7932 3.5 then 3.2 via u(1) concatenated left-to-right

	} else {
		enum { DISTBITS = 12, DISTSUM = 1 << DISTBITS };
		int log_alphabet_size = 5 + jxsml__u(st, 2);
		for (i = 0; i < out->num_clusters; ++i) {
			JXSML__TRY(jxsml__hybrid_int_config(st, log_alphabet_size, &out->configs[i]));
		}

		for (i = 0; i < out->num_clusters; ++i) {
			int table_size = 1 << log_alphabet_size;
			int D[table_size]; // TODO eliminate VLA
			for(j=0;j<table_size;++j)D[j]=0;

			switch (jxsml__u(st, 2)) {
			case 1: // one entry
				D[jxsml__u8(st)] = DISTSUM;
				break;

			case 3: { // two entries
				int v1 = jxsml__u8(st);
				int v2 = jxsml__u8(st);
				JXSML__SHOULD(v1 != v2 && v1 < table_size && v2 < table_size, "ansd");
				D[v1] = jxsml__u(st, DISTBITS);
				D[v2] = DISTSUM - D[v1];
				break;
			}

			case 2: { // evenly distribute to first `alphabet_size` entries (false -> true)
				int alphabet_size = jxsml__u8(st) + 1;
				int d = DISTSUM / alphabet_size;
				int bias_size = DISTSUM - d * alphabet_size;
				for (j = 0; j < bias_size; ++j) D[j] = d + 1;
				for (; j < alphabet_size; ++j) D[j] = d;
				break;
			}

			case 0: { // bit counts + RLE (false -> false)
				int len, shift, alphabet_size, omit_log, omit_pos, code, total, n;
				int ncodes, codes[259]; // # bits to read if >= 0, minus repeat count if < 0

				len = jxsml__u(st, 1) ? jxsml__u(st, 1) ? jxsml__u(st, 1) ? 3 : 2 : 1 : 0;
				shift = jxsml__u(st, len) + (1 << len) - 1;
				JXSML__SHOULD(shift <= 13, "ansd");
				alphabet_size = jxsml__u8(st) + 3;

				omit_log = -1;
				for (j = ncodes = 0; j < alphabet_size; ) {
					// reinterpretation of kLogCountLut; only handle 3- and 4-bit-long codes here
					// consume only the 4-bit prefix ...0001 for longer codes
					static const uint8_t LUT4[16] = {10,99,7,3,6,8,9,5,10,4,7,1,6,8,9,2};
					JXSML__TRY(jxsml__refill(st, 7));
					code = LUT4[st->bits & 15];
					n = code > 5 ? 4 : 3;
					st->bits >>= n;
					st->nbits -= n;
					if (code == 99) code = jxsml__u(st, 1) ? 0 : jxsml__u(st, 1) ? 11 : 12 + jxsml__u(st, 1);
					if (code < 13) {
						++j;
						codes[ncodes++] = code;
						if (omit_log < code) omit_log = code;
					} else {
						j += code = jxsml__u8(st) + 4;
						codes[ncodes++] = -code;
					}
				}
				// the distribution overflows, or there is no non-RLE code
				JXSML__SHOULD(j == alphabet_size && omit_log >= 0, "ansd");

				omit_pos = -1;
				for (j = n = total = 0; j < ncodes; ++j) {
					code = codes[j];
					if (code < 0) { // repeat
						int prev = n > 0 ? D[n - 1] : 0;
						JXSML__SHOULD(prev >= 0, "ansd"); // implicit D[n] followed by RLE
						while (code++ < 0) total += D[n++] = prev;
					} else if (code == omit_log) { // the first longest D[n] is "omitted" (implicit)
						omit_pos = n;
						omit_log = -1; // this branch runs at most once
						D[n++] = -1;
					} else if (code < 2) {
						total += D[n++] = code;
					} else {
						int bitcount;
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
		// C.2.3

	}

	return 0;

error:
	return st->err;
}

void jxsml__free_entropy_code(jxsml__entropy_code_t *code) {
}

int jxsml__size_header(jxsml__st *st, uint32_t *outw, uint32_t *outh) {
	int div8 = jxsml__u(st, 1);
	*outh = div8 ? (jxsml__u(st, 5) + 1) * 8 : jxsml__u32(st, 1, 9, 1, 13, 1, 18, 1, 30);
	switch (jxsml__u(st, 3)) { // ratio
	case 0: *outw = div8 ? (jxsml__u(st, 5) + 1) * 8 : jxsml__u32(st, 1, 9, 1, 13, 1, 18, 1, 30); break;
	case 1: *outw = *outh; break;
	case 2: *outw = (uint64_t) *outh * 6 / 5; break;
	case 3: *outw = (uint64_t) *outh * 4 / 3; break;
	case 4: *outw = (uint64_t) *outh * 3 / 2; break;
	case 5: *outw = (uint64_t) *outh * 16 / 9; break;
	case 6: *outw = (uint64_t) *outh * 5 / 4; break;
	case 7: *outw = *outh * 2; break;
	default: JXSML__UNREACHABLE();
	}
error:
	return st->err;
}

int jxsml__bit_depth(jxsml__st *st, int *outbpp, int *outexpbits) {
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
	xy[0] = JXSML__TOSIGNED(ux) / 1000000.0f;
	xy[1] = JXSML__TOSIGNED(uy) / 1000000.0f;
	return st->err;
}

int jxsml__extensions(jxsml__st *st) {
	uint64_t extensions = jxsml__u64(st);
	int i;
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
	int byte = 0, prev = 0, pprev = 0, ctx;

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
		byte = jxsml__entropy_code(st, ctx, &entropy);
		fprintf(stderr, "ctx=%d byte=%#x\n", ctx, (int)byte);
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
	int i;

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
		int extra_fields = jxsml__u(st, 1);
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
		int cw_mask, i, j;
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
	void *buf = malloc(bufsize);
	if (!buf) return 3;
	if (fread(buf, bufsize, 1, fp) != 1) return 2;
	fclose(fp);

	jxsml img;
	int ret = jxsml_load_from_memory(&img, buf, bufsize);
	if (ret) {
		fprintf(stderr, "error: %c%c%c%c\n", ret >> 24 & 0xff, ret >> 16 & 0xff, ret >> 8 & 0xff, ret & 0xff);
		return 1;
	}
	fprintf(stderr, "ok\n");
	return 0;
}
