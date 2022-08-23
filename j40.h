// J40: Self-contained JPEG XL Decoder
// Kang Seonghoon, 2022-08, Public Domain (CC0)
//
// This is a decoder for JPEG XL (ISO/IEC 18181) image format. It intends to be a fully compatible
// reimplementation to the reference implementation, libjxl, and also serves as a verification that
// the specification allows for an independent implementation besides from libjxl.
//
// "SPEC" comments are used for incorrect, ambiguous or misleading specification issues.
// "TODO spec" comments are roughly same, but not yet fully confirmed & reported.

////////////////////////////////////////////////////////////////////////////////
// preamble (only reachable via the user `#include`)

// controls whether each `#if`-`#endif` section in this file should be included or not.
// there are multiple purposes of this macro:
// - `J40__RECURSING` is always defined after the first ever `#include`, so that:
//   - the preamble will precede every other code in the typical usage, and
//   - the preamble won't be included twice.
// - `J40__RECURSING` is either 0 (public) or -1 (internal) depending on the logical visibility,
//   so that the preamble can choose whether to include the internal code or not.
// - larger values (>= 100) are used to repeat a specific section of code with
//   slightly different parameters, i.e. templated code.
// - one value (currently 9999) is reserved and used to ignore subsequent top-level `#include`s.
#ifndef J40__RECURSING

#define J40_VERSION 2270 // (fractional gregorian year - 2000) * 100, with a liberal rounding

#define J40_DEBUG

#ifndef J40_FILENAME // should be provided if this file has a different name than `j40.h`
#define J40_FILENAME "j40.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef J40_IMPLEMENTATION
	#include <string.h>
	#include <math.h>
	#include <limits.h>
	#include <errno.h>
	#include <stdio.h>
	#ifdef J40_DEBUG
		#include <assert.h>
	#endif
	#ifndef J40__EXPOSE_INTERNALS
		#define J40__EXPOSE_INTERNALS
	#endif
#endif

#ifdef J40__EXPOSE_INTERNALS
	#define J40__RECURSING (-1)
#else
	#define J40__RECURSING 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#endif // !defined J40__RECURSING

////////////////////////////////////////////////////////////////////////////////
// platform macros (partially public)

#if J40__RECURSING <= 0

// just in case:
#if CHAR_BIT != 8 // in fact, pretty much every file processing wouldn't make sense if CHAR_BIT > 8
	#error "J40 requires CHAR_BIT == 8"
#endif

#ifndef J40_STATIC_ASSERT
	#if __STDC_VERSION__ >= 199901L
		#define J40_STATIC_ASSERT(cond, msg) _Static_assert(cond, #msg)
	#else
		#define J40_STATIC_ASSERT(cond, msg) typedef char j40__##msg[(cond) ? 1 : -1]
	#endif
#endif // !defined J40_STATIC_ASSERT

// just in case again, because it is still possible for them to have padding bits (that we needn't):
J40_STATIC_ASSERT(sizeof(uint8_t) == 1, uint8_t_should_have_no_padding_bits);
J40_STATIC_ASSERT(sizeof(uint16_t) == 2, uint16_t_should_have_no_padding_bits);
J40_STATIC_ASSERT(sizeof(uint32_t) == 4, uint32_t_should_have_no_padding_bits);
J40_STATIC_ASSERT(sizeof(uint64_t) == 8, uint64_t_should_have_no_padding_bits);

#ifndef J40_RESTRICT
	#if __STDC_VERSION__ >= 199901L
		#define J40_RESTRICT restrict
	#elif defined __GNUC__ || __MSC_VER >= 1900 // since pretty much every GCC/Clang and VS 2015
		#define J40_RESTRICT __restrict
	#else
		#define J40_RESTRICT
	#endif
#endif // !defined J40_RESTRICT

#define J40_API // TODO

#ifdef J40_IMPLEMENTATION

#ifdef __has_attribute // since GCC 5.0.0 and clang 2.9.0
	#if __has_attribute(always_inline)
		#define J40__HAS_ALWAYS_INLINE_ATTR 1
	#endif
	#if __has_attribute(warn_unused_result)
		#define J40__HAS_WARN_UNUSED_RESULT_ATTR 1
	#endif
#endif

#ifdef __has_builtin // since GCC 10.0.0 and clang 1.0.0 (which thus requires no version check)
	#if __has_builtin(__builtin_expect)
		#define J40__HAS_BUILTIN_EXPECT 1
	#endif
	#if __has_builtin(__builtin_add_overflow)
		#define J40__HAS_BUILTIN_ADD_OVERFLOW 1
	#endif
	#if __has_builtin(__builtin_sub_overflow)
		#define J40__HAS_BUILTIN_SUB_OVERFLOW 1
	#endif
	#if __has_builtin(__builtin_mul_overflow)
		#define J40__HAS_BUILTIN_MUL_OVERFLOW 1
	#endif
	#if __has_builtin(__builtin_unreachable)
		#define J40__HAS_BUILTIN_UNREACHABLE 1
	#endif
	#if __has_builtin(__builtin_assume_aligned)
		#define J40__HAS_BUILTIN_ASSUME_ALIGNED 1
	#endif
#endif

// clang (among many others) fakes GCC version by default, but we handle clang separately
#if defined __GNUC__ && !defined __clang__
	#define J40__GCC_VER (__GNUC__ * 0x10000 + __GNUC_MINOR__ * 0x100 + __GNUC_PATCHLEVEL__)
#else
	#define J40__GCC_VER 0
#endif

#ifdef __clang__
	#define J40__CLANG_VER (__clang_major__ * 0x10000 + __clang_minor__ * 0x100 + __clang_patchlevel__)
#else
	#define J40__CLANG_VER 0
#endif

#ifndef J40_STATIC
	#define J40_STATIC static
#endif

#ifndef J40_INLINE
	#define J40_INLINE J40_STATIC inline
#endif

#ifndef J40_ALWAYS_INLINE
	#if J40__HAS_ALWAYS_INLINE_ATTR || J40__GCC_VER >= 0x30100 || J40__CLANG_VER >= 0x10000
		#define J40_ALWAYS_INLINE __attribute__((always_inline)) J40_INLINE
	#elif defined _MSC_VER
		#define J40_ALWAYS_INLINE __forceinline J40_INLINE
	#else
		#define J40_ALWAYS_INLINE J40_INLINE
	#endif
#endif // !defined J40_ALWAYS_INLINE

#ifndef J40_NODISCARD
	#if __cplusplus >= 201703L /*|| __STDC_VERSION__ >= 2023xxL */
		#define J40_NODISCARD [[nodiscard]] // since C++17 and C23
	#elif J40__HAS_WARN_UNUSED_RESULT_ATTR || J40__GCC_VER >= 0x30400 || J40__CLANG_VER >= 0x10000
		// this is stronger than [[nodiscard]] in that it's much harder to suppress
		#define J40_NODISCARD __attribute__((warn_unused_result)) // since GCC 3.4 and clang 1.0.0
	#else
		#define J40_NODISCARD
	#endif
#endif // !defined J40_NODISCARD

// rule of thumb: sparingly use them, except for the obvious error cases
#ifndef J40_EXPECT
	#if J40__HAS_BUILTIN_EXPECT || J40__GCC_VER >= 0x30000
		#define J40_EXPECT(p, v) __builtin_expect(p, v)
	#else
		#define J40_EXPECT(p, v) (p)
	#endif
#endif // !defined J40_EXPECT
#ifndef J40_LIKELY
	#define J40_LIKELY(p) J40_EXPECT(!!(p), 1)
#endif
#ifndef J40_UNLIKELY
	#define J40_UNLIKELY(p) J40_EXPECT(!!(p), 0)
#endif

#if !defined J40_ADD_OVERFLOW && (J40__HAS_BUILTIN_ADD_OVERFLOW || J40__GCC_VER >= 0x50000)
	#define J40_ADD_OVERFLOW(a, b, res) __builtin_add_overflow(a, b, res)
#endif
#if !defined J40_SUB_OVERFLOW && (J40__HAS_BUILTIN_SUB_OVERFLOW || J40__GCC_VER >= 0x50000)
	#define J40_SUB_OVERFLOW(a, b, res) __builtin_sub_overflow(a, b, res)
#endif
#if !defined J40_MUL_OVERFLOW && (J40__HAS_BUILTIN_MUL_OVERFLOW || J40__GCC_VER >= 0x50000)
	#define J40_MUL_OVERFLOW(a, b, res) __builtin_mul_overflow(a, b, res)
#endif

#endif // defined J40_IMPLEMENTATION
#endif // J40__RECURSING <= 0

////////////////////////////////////////////////////////////////////////////////
// public API

#if J40__RECURSING <= 0

// an internal error type. non-zero indicates a different error condition.
// user callbacks can also emit error codes, which should not exceed `J40_MIN_RESERVED_ERR`.
// it can be interpreted as a four-letter code, but such encoding is not guaranteed.
typedef uint32_t j40_err;
#define J40_MIN_RESERVED_ERR (j40_err) (1 << 24) // anything below this can be used freely

#ifdef J40_IMPLEMENTATION
#define J40_RETURNS_ERR J40_NODISCARD j40_err
#endif // defined J40_IMPLEMENTATION

#endif // J40__RECURSING <= 0

////////////////////////////////////////////////////////////////////////////////
#if J40__RECURSING < 0                      // internal code starts from here //
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// state

// a common context ("state") for all internal functions.
typedef struct {
	j40_err err; // first error code encountered, or 0
	int saved_errno;
	int cannot_retry; // a fatal error was encountered and no more additional input will fix it

	//                 |<------------------- capacity ------------------->|
	//                 |<------------- size -------------->|              |
	//                 +-----------------------------------|-----------+  |
	//                 |    +---------+    +---------------|---+       |  |
	// file:           |    |  box 1  |    |       box 2   |   |       |  |
	//                 |    +---------+    +---------------|---+       |  |
	//                 +-----------------------------------+---|-------+--+
	// backing buffer: | buf                               |///|//////////|
	//                 +----------------------+------------+---|----|-----+
	// logical buffer:                        | ptr        |   | garbage
	//                                        +------------+   |
	//                              remaining |<---------->|<->| box_remaining
	//
	// the _backing buffer_ is a region of memory managed by the input source, which may have
	// been provided from the caller or allocated by the input source itself.
	// its valid extent is [buf, buf + size) while the allocated extent is [buf, buf + capacity).
	// the valid portion of the backing buffer is guaranteed to be a contiguous region of the file.
	//
	// the _logical buffer_ [ptr, ptr + remaining) is a slice of the backing buffer and
	// normally a contiguous region of the codestream. if the file is a bare codestream
	// (starting with `FF 0A`) there is no actual distinction and the logical buffer covers
	// the whole valid portion of the backing buffer. if the file is an ISOBMFF container however,
	// the actual codestream may span multiple disconnected regions ("boxes") of the file,
	// and the logical buffer can hold only one such region at a time.
	//
	// normally the parser has to scan the file sequentially, so at some point its pointer will
	// be between codestream boxes. we call this the "container" mode, as opposed to the typical
	// "codestream" mode. in the container mode the logical buffer temporarily can cover multiple
	// boxes or even no boxes at all; the parser should shrink the logical buffer into a single box
	// before it switches back to the codestream mode. if the current logical buffer doesn't cover
	// the whole box, the size of the remainder (i.e. the region beyond the current backing buffer)
	// is recorded to `box_remaining`.
	//
	// if the parser proceeds to the end of the logical buffer, it rolls back the state and
	// issues a partial input condition (error code `shrt`). this is a soft error and can be
	// fixed by the input source providing more input; the parser prepares this by moving
	// the uncommitted input since the last checkpoint to the beginning of the backing buffer
	// and reading the next input into the rest. the parser retries back from that checkpoint.
	// if the checkpoint didn't advance after retry, this means that the current backing buffer is
	// too small to continue on. the backing buffer gets expanded until the checkpoint advances.

	// bit buffer
	int nbits;
	uint64_t bits;

	// logical buffer
	uint8_t *ptr;
	size_t remaining;

	// the input source; only used to repopulate `ptr` when `remaining == 0`.
	struct j40__source *source;

	// different subsystems make use of additional contexts, all accessible from here.
	struct j40__container_st *container;
	struct j40__image_st *image;
	struct j40__frame_st *frame;
	//struct j40__lfgroup_st *lfgroup;
} j40__st;

////////////////////////////////////////////////////////////////////////////////
// error handling

#define J40__4(s) (j40_err) (((uint32_t) s[0] << 24) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 8) | (uint32_t) s[3])
#define J40__ERR(s) j40__set_error(st, J40__4(s))
#define J40__SHOULD_OR(cond, s, stmt) do { \
		if (J40_UNLIKELY(st->err)) { stmt; } \
		else if (J40_UNLIKELY(!(cond))) { j40__set_error(st, J40__4(s)); stmt; } \
	} while (0)
#define J40__ON_ERROR error // goto label
#define J40__SHOULD(cond, s) J40__SHOULD_OR(cond, s, goto J40__ON_ERROR)
#define J40__RAISE(s) do { j40__set_error(st, J40__4(s)); goto J40__ON_ERROR; } while (0)
#define J40__RAISE_DELAYED() do { if (J40_UNLIKELY(st->err)) goto J40__ON_ERROR; } while (0)
#define J40__TRY(expr) do { if (J40_UNLIKELY(expr)) goto J40__ON_ERROR; } while (0)
// this *should* use casting because C/C++ don't allow comparison between pointers
// that came from different arrays at all: https://stackoverflow.com/a/39161283
#define J40__INBOUNDS(ptr, start, size) ((uintptr_t) (ptr) - (uintptr_t) (start) <= (uintptr_t) (size))
#ifdef J40_DEBUG
	#define J40__ASSERT(cond) assert(cond)
	#define J40__UNREACHABLE() J40__ASSERT(0)
#elif J40__HAS_BUILTIN_UNREACHABLE || J40__GCC_VER >= 0x40500
	#define J40__ASSERT(cond) (J40_UNLIKELY(!(cond)) ? __builtin_unreachable() : (void) 0)
	#define J40__UNREACHABLE() __builtin_unreachable()
#else
	#define J40__ASSERT(cond) ((void) 0)
	#define J40__UNREACHABLE() ((void) 0) // TODO also check for MSVC __assume
#endif

#define J40__TRY_REALLOC(ptr, itemsize, len, cap) \
	do { \
		void *newptr = j40__realloc(st, *(ptr), itemsize, len, cap); \
		if (J40_LIKELY(newptr)) *(ptr) = newptr; else goto error; \
	} while (0)

J40_STATIC j40_err j40__set_error(j40__st *st, j40_err err);
J40_STATIC void *j40__realloc(j40__st *st, void *ptr, size_t itemsize, int32_t len, int32_t *cap);

#ifdef J40_IMPLEMENTATION

J40_STATIC j40_err j40__set_error(j40__st *st, j40_err err) {
	if (err != J40__4("shrt")) st->cannot_retry = 1;
	if (!st->err) st->err = err;
	return err;
}

J40_STATIC void *j40__realloc(j40__st *st, void *ptr, size_t itemsize, int32_t len, int32_t *cap) {
	void *newptr;
	uint32_t newcap;
	J40__ASSERT(len >= 0);
	if (len <= *cap) return ptr;
	newcap = (uint32_t) *cap * 2;
	if (newcap > (uint32_t) INT32_MAX) newcap = (uint32_t) INT32_MAX;
	if (newcap < (uint32_t) len) newcap = (uint32_t) len;
	J40__SHOULD(newcap <= SIZE_MAX / itemsize, "!mem");
	J40__SHOULD(newptr = realloc(ptr, itemsize * newcap), "!mem");
	*cap = (int32_t) newcap;
	return newptr;
J40__ON_ERROR:
	return NULL;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// utility

#define J40__CONCAT_(a,b) a##b
#define J40__CONCAT(a,b) J40__CONCAT_(a,b)
#define J40__CONCAT3(a,b,c) J40__CONCAT(a,J40__CONCAT(b,c))

// `j40__(foo, X)` and its uppercase version is `j40__foo` followed by a macro `J40__V` expanded;
// this greatly simplifies the construction of templated names.
#define J40__PARAMETRIC_NAME_(prefix, x, J40__V) J40__CONCAT3(prefix, x, J40__V)
#define j40__(x, V) J40__PARAMETRIC_NAME_(j40__, x, J40__CONCAT(J40__, V))
#define J40__(x, V) J40__PARAMETRIC_NAME_(J40__, x, J40__CONCAT(J40__, V))

J40_ALWAYS_INLINE int32_t j40__unpack_signed(int32_t x);
J40_ALWAYS_INLINE int32_t j40__ceil_div32(int32_t x, int32_t y);

#ifdef J40_IMPLEMENTATION

J40_ALWAYS_INLINE int32_t j40__unpack_signed(int32_t x) {
	return (int32_t) (x & 1 ? -(x / 2 + 1) : x / 2);
}

// equivalent to ceil(x / y)
J40_ALWAYS_INLINE int32_t j40__ceil_div32(int32_t x, int32_t y) { return (x + y - 1) / y; }

#endif // defined J40_IMPLEMENTATION

// ----------------------------------------
// recursion for bit-dependent math functions
#undef J40__RECURSING
#define J40__RECURSING 100
#define J40__N 16
#include J40_FILENAME
#define J40__N 32
#include J40_FILENAME
#define J40__N 64
#include J40_FILENAME
#undef J40__RECURSING
#define J40__RECURSING (-1)

#endif // J40__RECURSING < 0
#if J40__RECURSING == 100
	#define j40__intN_t J40__CONCAT3(int, J40__N, _t)
	#define j40__uintN_t J40__CONCAT3(uint, J40__N, _t)
	#define J40__INTN_MAX J40__CONCAT3(INT, J40__N, _MAX)
	#define J40__INTN_MIN J40__CONCAT3(INT, J40__N, _MIN)
// ----------------------------------------

J40_ALWAYS_INLINE j40__intN_t j40__(floor_avg,N)(j40__intN_t x, j40__intN_t y);
J40_ALWAYS_INLINE j40__intN_t j40__(abs,N)(j40__intN_t x);
J40_ALWAYS_INLINE j40__intN_t j40__(min,N)(j40__intN_t x, j40__intN_t y);
J40_ALWAYS_INLINE j40__intN_t j40__(max,N)(j40__intN_t x, j40__intN_t y);
J40_ALWAYS_INLINE j40__intN_t j40__(add,N)(j40__st *st, j40__intN_t x, j40__intN_t y);
J40_ALWAYS_INLINE j40__intN_t j40__(sub,N)(j40__st *st, j40__intN_t x, j40__intN_t y);
J40_ALWAYS_INLINE j40__intN_t j40__(mul,N)(j40__st *st, j40__intN_t x, j40__intN_t y);

#ifdef J40_IMPLEMENTATION

// same to `(a + b) >> 1` but doesn't overflow, useful for tight loops with autovectorization
// https://devblogs.microsoft.com/oldnewthing/20220207-00/?p=106223
J40_ALWAYS_INLINE j40__intN_t j40__(floor_avg,N)(j40__intN_t x, j40__intN_t y) {
	return (j40__intN_t) (x / 2 + y / 2 + (x & y & 1));
}

J40_ALWAYS_INLINE j40__intN_t j40__(abs,N)(j40__intN_t x) {
	return (j40__intN_t) (x < 0 ? -x : x);
}
J40_ALWAYS_INLINE j40__intN_t j40__(min,N)(j40__intN_t x, j40__intN_t y) {
	return (j40__intN_t) (x < y ? x : y);
}
J40_ALWAYS_INLINE j40__intN_t j40__(max,N)(j40__intN_t x, j40__intN_t y) {
	return (j40__intN_t) (x > y ? x : y);
}

J40_ALWAYS_INLINE j40__intN_t j40__(add,N)(j40__st *st, j40__intN_t x, j40__intN_t y) {
#ifdef J40_ADD_OVERFLOW
	j40__intN_t res;
	if (J40_UNLIKELY(J40_ADD_OVERFLOW(x, y, &res))) J40__ERR("over");
	return res;
#else
	if (J40_UNLIKELY((x > 0 && y > J40__INTN_MAX - x) || (x < 0 && y < J40__INTN_MIN - x))) {
		return J40__ERR("over"), x;
	} else {
		return (j40__intN_t) (x + y);
	}
#endif
}

J40_ALWAYS_INLINE j40__intN_t j40__(sub,N)(j40__st *st, j40__intN_t x, j40__intN_t y) {
#ifdef J40_SUB_OVERFLOW
	j40__intN_t res;
	if (J40_UNLIKELY(J40_SUB_OVERFLOW(x, y, &res))) J40__ERR("over");
	return res;
#else
	if (J40_UNLIKELY((y < 0 && x > J40__INTN_MAX + y) || (y > 0 && x < J40__INTN_MIN + y))) {
		return J40__ERR("over"), x;
	} else {
		return (j40__intN_t) (x - y);
	}
#endif
}

J40_ALWAYS_INLINE j40__intN_t j40__(mul,N)(j40__st *st, j40__intN_t x, j40__intN_t y) {
#ifdef J40_MUL_OVERFLOW
	j40__intN_t res;
	if (J40_UNLIKELY(J40_MUL_OVERFLOW(x, y, &res))) J40__ERR("over");
	return res;
#else
	if (J40_UNLIKELY(
		(x == -1 && y == J40__INTN_MIN) || (y == -1 && x == J40__INTN_MIN) ||
		(x != 0 && (y > J40__INTN_MAX / x || y < J40__INT_MIN / x))
	)) {
		return J40__ERR("over"), x;
	} else {
		return (j40__intN_t) (x * y);
	}
#endif
}

#endif // defined J40_IMPLEMENTATION

#define J40__UINTN_MAX J40__CONCAT3(UINT, J40__N, _MAX)
#if UINT_MAX == J40__UINTN_MAX
	#define J40__CLZN __builtin_clz
#elif ULONG_MAX == J40__UINTN_MAX
	#define J40__CLZN __builtin_clzl
#elif ULLONG_MAX == J40__UINTN_MAX
	#define J40__CLZN __builtin_clzll
#endif
#undef J40__UINTN_MAX
#ifdef J40__CLZN
	J40_ALWAYS_INLINE int j40__(floor_lg,N)(j40__uintN_t x);
	J40_ALWAYS_INLINE int j40__(ceil_lg,N)(j40__uintN_t x);

	#ifdef J40_IMPLEMENTATION
	// both requires x to be > 0
	J40_ALWAYS_INLINE int j40__(floor_lg,N)(j40__uintN_t x) {
		return J40__N - 1 - J40__CLZN(x);
	}
	J40_ALWAYS_INLINE int j40__(ceil_lg,N)(j40__uintN_t x) {
		return x > 1 ? J40__N - J40__CLZN(x - 1) : 0;
	}
	#endif

	#undef J40__CLZN
#endif

// ----------------------------------------
// end of recursion
	#undef j40__intN_t
	#undef j40__uintN_t
	#undef J40__INTN_MAX
	#undef J40__INTN_MIN
	#undef J40__N
#endif // J40__RECURSING == 100
#if J40__RECURSING < 0
// ----------------------------------------

////////////////////////////////////////////////////////////////////////////////
// aligned pointers

// TODO simplify this if possible

#ifndef J40_ASSUME_ALIGNED
	#if J40__HAS_BUILTIN_ASSUME_ALIGNED || J40__GCC_VER >= 0x40700
		#define J40_ASSUME_ALIGNED(p, align) __builtin_assume_aligned(p, align)
	#else
		#define J40_ASSUME_ALIGNED(p, align) (p)
	#endif
#endif // !defined J40_ASSUME_ALIGNED

J40_ALWAYS_INLINE void *j40__alloc_aligned(size_t sz, size_t align, size_t *outmisalign);
J40_ALWAYS_INLINE void j40__free_aligned(void *ptr, size_t align, size_t misalign);

J40_STATIC void *j40__alloc_aligned_fallback(size_t sz, size_t align, size_t *outmisalign);
J40_STATIC void j40__free_aligned_fallback(void *ptr, size_t align, size_t misalign);

#ifdef J40_IMPLEMENTATION

#if _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600
	J40_ALWAYS_INLINE void *j40__alloc_aligned(size_t sz, size_t align, size_t *outmisalign) {
		void *ptr = NULL;
		*outmisalign = 0;
		return posix_memalign(&ptr, align, sz) ? NULL : ptr;
	}
	J40_ALWAYS_INLINE void j40__free_aligned(void *ptr, size_t align, size_t misalign) {
		(void) align; (void) misalign;
		free(ptr);
	}
#elif defined _ISOC11_SOURCE
	J40_ALWAYS_INLINE void *j40__alloc_aligned(size_t sz, size_t align, size_t *outmisalign) {
		if (sz > SIZE_MAX / align * align) return NULL; // overflow
		*outmisalign = 0;
		return aligned_alloc(align, (sz + align - 1) / align * align);
	}
	J40_ALWAYS_INLINE void j40__free_aligned(void *ptr, size_t align, size_t misalign) {
		(void) align; (void) misalign;
		free(ptr);
	}
#else
	J40_ALWAYS_INLINE void *j40__alloc_aligned(size_t sz, size_t align, size_t *outmisalign) {
		return j40__alloc_aligned_fallback(sz, align, outmisalign);
	}
	J40_ALWAYS_INLINE void j40__free_aligned(void *ptr, size_t align, size_t misalign) {
		j40__free_aligned_fallback(ptr, align, misalign);
	}
#endif

// a fallback implementation; the caller should store the misalign amount [0, align) separately.
// used when the platform doesn't provide aligned malloc at all, or the platform implementation
// is not necessarily better; e.g. MSVC _aligned_malloc has the same amount of overhead as of Win10
J40_STATIC void *j40__alloc_aligned_fallback(size_t sz, size_t align, size_t *outmisalign) {
	// while this is almost surely an overestimate (can be improved if we know the malloc alignment)
	// there is no standard way to compute a better estimate in C99 so this is inevitable.
	size_t maxmisalign = align - 1, misalign;
	void *ptr;
	if (sz > SIZE_MAX - maxmisalign) return NULL; // overflow
	ptr = malloc(sz + maxmisalign);
	if (!ptr) return NULL;
	misalign = align - (uintptr_t) ptr % align;
	if (misalign == align) misalign = 0;
	*outmisalign = misalign;
	return (void*) ((uintptr_t) ptr + misalign);
}

J40_ALWAYS_INLINE void j40__free_aligned_fallback(void *ptr, size_t align, size_t misalign) {
	if (!ptr) return;
	J40__ASSERT((uintptr_t) ptr % align == 0);
	free((void*) ((uintptr_t) ptr - misalign));
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// two-dimensional view

typedef struct { int32_t logw, logh; float *J40_RESTRICT ptr; } j40__view_f32;

J40_ALWAYS_INLINE j40__view_f32 j40__make_view_f32(int32_t logw, int32_t logh, float *J40_RESTRICT ptr);
J40_ALWAYS_INLINE void j40__adapt_view_f32(j40__view_f32 *outv, int32_t logw, int32_t logh);
J40_ALWAYS_INLINE void j40__reshape_view_f32(j40__view_f32 *outv, int32_t logw, int32_t logh);
J40_ALWAYS_INLINE void j40__copy_view_f32(j40__view_f32 *outv, const j40__view_f32 inv);
J40_ALWAYS_INLINE void j40__transpose_view_f32(j40__view_f32 *outv, const j40__view_f32 inv);
J40_ALWAYS_INLINE void j40__oddeven_columns_to_halves_f32(j40__view_f32 *outv, const j40__view_f32 inv);
J40_ALWAYS_INLINE void j40__oddeven_rows_to_halves_f32(j40__view_f32 *outv, const j40__view_f32 inv);
J40_STATIC void j40__print_view_f32(j40__view_f32 v, const char *name, const char *file, int32_t line);

#ifdef J40_IMPLEMENTATION

J40_ALWAYS_INLINE j40__view_f32 j40__make_view_f32(int32_t logw, int32_t logh, float *J40_RESTRICT ptr) {
	j40__view_f32 ret = { logw, logh, ptr };
	return ret;
}

J40_ALWAYS_INLINE void j40__adapt_view_f32(j40__view_f32 *outv, int32_t logw, int32_t logh) {
	J40__ASSERT(outv->logw + outv->logh >= logw + logh);
	outv->logw = logw;
	outv->logh = logh;
}

J40_ALWAYS_INLINE void j40__reshape_view_f32(j40__view_f32 *outv, int32_t logw, int32_t logh) {
	J40__ASSERT(outv->logw + outv->logh == logw + logh);
	outv->logw = logw;
	outv->logh = logh;
}

J40_ALWAYS_INLINE void j40__copy_view_f32(j40__view_f32 *outv, const j40__view_f32 inv) {
	int32_t x, y;
	float *outptr = outv->ptr;
	j40__adapt_view_f32(outv, inv.logw, inv.logh);
	for (y = 0; y < (1 << inv.logh); ++y) for (x = 0; x < (1 << inv.logw); ++x) {
		outptr[y << inv.logw | x] = inv.ptr[y << inv.logw | x];
	}
}

J40_ALWAYS_INLINE void j40__transpose_view_f32(j40__view_f32 *outv, const j40__view_f32 inv) {
	int32_t x, y;
	float *outptr = outv->ptr;
	j40__adapt_view_f32(outv, inv.logh, inv.logw);
	for (y = 0; y < (1 << inv.logh); ++y) for (x = 0; x < (1 << inv.logw); ++x) {
		outptr[x << inv.logh | y] = inv.ptr[y << inv.logw | x];
	}
}

// shuffles columns 01234567 into 02461357 and so on
J40_ALWAYS_INLINE void j40__oddeven_columns_to_halves_f32(j40__view_f32 *outv, const j40__view_f32 inv) {
	int32_t x, y;
	float *outptr = outv->ptr;
	J40__ASSERT(inv.logw > 0);
	j40__adapt_view_f32(outv, inv.logw, inv.logh);
	for (y = 0; y < (1 << inv.logh); ++y) for (x = 0; x < (1 << inv.logw); ++x) {
		int32_t outx = ((x & 1) << (inv.logw - 1)) | (x >> 1);
		outptr[y << inv.logw | outx] = inv.ptr[y << inv.logw | x];
	}
}

// shuffles rows 01234567 into 02461357 and so on
J40_ALWAYS_INLINE void j40__oddeven_rows_to_halves_f32(j40__view_f32 *outv, const j40__view_f32 inv) {
	int32_t x, y;
	float *outptr = outv->ptr;
	J40__ASSERT(inv.logh > 0);
	j40__adapt_view_f32(outv, inv.logw, inv.logh);
	for (y = 0; y < (1 << inv.logh); ++y) {
		int32_t outy = ((y & 1) << (inv.logh - 1)) | (y >> 1);
		for (x = 0; x < (1 << inv.logw); ++x) outptr[outy << inv.logw | x] = inv.ptr[y << inv.logw | x];
	}
}

#define J40__AT(view, x, y) \
	(J40__ASSERT(0 <= (x) && (x) < (1 << (view).logw) && 0 <= (y) && (y) < (1 << (view).logh)), \
	 (view).ptr + ((y) << (view).logw | (x)))

#define J40__VIEW_FOREACH(view, y, x, v) \
	for (y = 0; y < (1 << (view).logh); ++y) \
		for (x = 0; x < (1 << (view).logw) && (v = (view).ptr + (y << (view).logw | x), 1); ++x)

J40_STATIC void j40__print_view_f32(j40__view_f32 v, const char *name, const char *file, int32_t line) {
	int32_t x, y;
	printf(".--- %s:%d: %s (w=%d h=%d @%p)", file, line, name, 1 << v.logw, 1 << v.logh, v.ptr);
	for (y = 0; y < (1 << v.logh); ++y) {
		printf("\n|");
		for (x = 0; x < (1 << v.logw); ++x) printf(" %f", *J40__AT(v, x, y));
	}
	printf("\n'--- %s:%d\n", file, line);
}

#endif // defined J40_IMPLEMENTATION

#define j40__print_view_f32(v) j40__print_view_f32(v, #v, __FILE__, __LINE__)

////////////////////////////////////////////////////////////////////////////////
// plane

enum {
	J40__PLANE_U8 = (uint8_t) 0x20,
	J40__PLANE_U16 = (uint8_t) 0x21,
	J40__PLANE_I16 = (uint8_t) 0x41,
	J40__PLANE_I32 = (uint8_t) 0x42,
	J40__PLANE_F32 = (uint8_t) 0x62,
};

#define J40__PIXELS_ALIGN 32

typedef struct {
	uint8_t type; // 0 means uninitialized (all fields besides from pixels are considered garbage)
	uint8_t misalign;
	int8_t vshift, hshift;
	int32_t width, height;
	int32_t stride_bytes; // the number of *bytes* between each row
	uintptr_t pixels;
} j40__plane;

#define J40__TYPED_PIXELS(plane, y, typeconst, pixel_t) \
	(J40__ASSERT((plane)->type == typeconst), \
	 J40__ASSERT(0 <= (y) && (y) < (plane)->height), \
	 (pixel_t*) J40_ASSUME_ALIGNED( \
		(void*) ((plane)->pixels + (size_t) (plane)->stride_bytes * (size_t) (y)), \
		J40__PIXELS_ALIGN))

#define J40__U8_PIXELS(plane, y) J40__TYPED_PIXELS(plane, y, J40__PLANE_U8, uint8_t)
#define J40__U16_PIXELS(plane, y) J40__TYPED_PIXELS(plane, y, J40__PLANE_U16, uint16_t)
#define J40__I16_PIXELS(plane, y) J40__TYPED_PIXELS(plane, y, J40__PLANE_I16, int16_t)
#define J40__I32_PIXELS(plane, y) J40__TYPED_PIXELS(plane, y, J40__PLANE_I32, int32_t)
#define J40__F32_PIXELS(plane, y) J40__TYPED_PIXELS(plane, y, J40__PLANE_F32, float)

#define J40__PLANE_STRIDE(plane) ((plane)->stride_bytes >> ((plane)->type & 31))

J40_STATIC J40_RETURNS_ERR j40__init_plane(
	j40__st *st, uint8_t type, int32_t width, int32_t height, j40__plane *out
);
J40_STATIC J40_RETURNS_ERR j40__init_and_clear_plane(
	j40__st *st, uint8_t type, int32_t width, int32_t height, j40__plane *out
);
J40_STATIC int j40__plane_all_equal_sized(const j40__plane *begin, const j40__plane *end);
J40_STATIC void j40__free_plane(j40__plane *plane);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__init_plane(
	j40__st *st, uint8_t type, int32_t width, int32_t height, j40__plane *out
) {
	int32_t pixelsize = 1 << (type & 31);
	void *pixels;
	int32_t stride_bytes = j40__mul32(st,
		j40__ceil_div32(j40__mul32(st, width, pixelsize), J40__PIXELS_ALIGN),
		J40__PIXELS_ALIGN);
	size_t misalign;

	J40__ASSERT(width > 0 && height > 0);
	J40__SHOULD((size_t) stride_bytes <= SIZE_MAX / (uint32_t) height, "over");
	J40__SHOULD(
		pixels = j40__alloc_aligned((size_t) stride_bytes * (size_t) height, J40__PIXELS_ALIGN, &misalign),
		"!mem");
	out->stride_bytes = stride_bytes;
	out->width = width;
	out->height = height;
	out->type = type;
	out->vshift = out->hshift = 0;
	out->misalign = (uint8_t) misalign;
	out->pixels = (uintptr_t) pixels;
J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__init_and_clear_plane(
	j40__st *st, uint8_t type, int32_t width, int32_t height, j40__plane *out
) {
	J40__TRY(j40__init_plane(st, type, width, height, out));
	memset((void*) out->pixels, 0, (size_t) out->stride_bytes * (size_t) height);
J40__ON_ERROR:
	return st->err;
}

J40_STATIC int j40__plane_all_equal_sized(const j40__plane *begin, const j40__plane *end) {
	j40__plane c;
	int shift_should_match;
	if (begin >= end) return 0; // do not allow edge cases
	c = *begin;
	shift_should_match = (begin->vshift >= 0 && begin->hshift >= 0);
	while (++begin < end) {
		if (c.width != begin->width || c.height != begin->height) return 0;
		// even though the sizes match, different shifts can't be mixed as per the spec
		if (shift_should_match) {
			if (c.vshift >= 0 && c.hshift >= 0 && (c.vshift != begin->vshift || c.hshift != begin->hshift)) return 0;
		}
	}
	return 1;
}

J40_STATIC void j40__free_plane(j40__plane *plane) {
	// we don't touch pixels if plane is zero-initialized via memset, because while `plane->type` is
	// definitely zero in this case but `(void*) plane->pixels` might NOT be a null pointer!
	if (plane->type) j40__free_aligned((void*) plane->pixels, J40__PIXELS_ALIGN, plane->misalign);
	plane->width = plane->height = plane->stride_bytes = 0;
	plane->type = 0;
	plane->vshift = plane->hshift = 0;
	plane->misalign = 0;
	plane->pixels = (uintptr_t) (void*) 0; 
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// input source

typedef void (*j40_memory_free_func)(void *data);

typedef j40_err (*j40_source_read_func)(uint8_t *buf, size_t maxsize, size_t *size, void *data);
//typedef j40_err (*j40_source_jump_func)(size_t offset, void *data, void **outdata);
typedef void (*j40_source_free_func)(void *data);

typedef struct j40__source {
	// the backing buffer for `ptr`. the user may directly provide the buffer, optionally owned,
	// or the input source may allocate an appropriate amount of the backing buffer.
	uint8_t *buf;
	j40_memory_free_func buf_free_func; // set if the buffer is owned, used to deallocate `buf`
	size_t size, capacity;

	// the first position where the parser can ever backtrack. should point to the backing buffer.
	uint8_t *checkpoint;

	// the number of bytes read before the backing buffer has been replenished; diagnostics only.
	size_t bytes_before_buf;

	// callbacks used when the backing buffer has been exhausted and the retry has been scheduled.
	j40_source_read_func read_func;
	//j40_source_jump_func jump_func;
	j40_source_free_func free_func;
	void *data;
} j40__source;

J40_STATIC void j40__init_memory_source(uint8_t *, size_t, j40_memory_free_func, j40__source *);
J40_STATIC J40_RETURNS_ERR j40__init_file_source(j40__st *, const char *, size_t, j40__source *);
J40_STATIC J40_RETURNS_ERR j40__refill_backing_buffer(j40__st *);
J40_STATIC void j40__free_source(j40__source *);

#ifdef J40_IMPLEMENTATION

J40_STATIC void j40__init_memory_source(
	uint8_t *buf, size_t size, j40_memory_free_func freefunc, j40__source *s
) {
	s->checkpoint = s->buf = buf;
	s->buf_free_func = freefunc;
	s->size = s->capacity = size;
	s->bytes_before_buf = 0;
	s->read_func = NULL;
	s->free_func = NULL;
	s->data = NULL;
}

J40_STATIC J40_RETURNS_ERR j40__file_source_read(uint8_t *buf, size_t maxsize, size_t *size, void *data) {
	FILE *fp = data;
	size_t read = fread(buf, 1, maxsize, fp);
	if (read > 0) {
		*size = read;
		return 0;
	} else if (feof(fp)) {
		*size = 0;
		return 0;
	} else {
		return J40__4("read");
	}
}

J40_STATIC void j40__file_source_free(void *data) {
	FILE *fp = data;
	fclose(fp);
}

J40_STATIC J40_RETURNS_ERR j40__init_file_source(j40__st *st, const char *path, size_t bufsize, j40__source *s) {
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		st->saved_errno = errno;
		J40__RAISE("open");
	}

	J40__SHOULD(s->checkpoint = s->buf = malloc(bufsize), "!mem");
	s->buf_free_func = free;
	s->size = 0;
	s->capacity = bufsize;
	s->bytes_before_buf = 0;
	s->read_func = j40__file_source_read;
	s->free_func = j40__file_source_free;
	s->data = fp;
	return 0;

J40__ON_ERROR:
	if (fp) fclose(fp);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__refill_backing_buffer(j40__st *st) {
	j40__source *source = st->source;
	size_t committed_size, ptr_pos;

	if (!source->read_func) {
		st->cannot_retry = 1;
		return J40__ERR("shrt");
	}

	if (!st->ptr) { // initial state
		source->checkpoint = st->ptr = source->buf;
		st->remaining = 0;
	}

	J40__ASSERT(J40__INBOUNDS(st->ptr, source->buf, source->size));
	J40__ASSERT(J40__INBOUNDS(source->checkpoint, source->buf, source->size));
	J40__ASSERT(source->checkpoint <= st->ptr);

	// trim the committed portion from the backing buffer
	source->bytes_before_buf += committed_size = (size_t) (source->checkpoint - source->buf);
	ptr_pos = (size_t) (st->ptr - source->checkpoint);
	memmove(source->buf, source->checkpoint, source->size - committed_size);
	source->checkpoint = source->buf;
	source->size -= committed_size;
	st->ptr = source->checkpoint + ptr_pos;

	// if there is no room left in the backing buffer, it's time to grow it
	if (source->size == source->capacity) {
		size_t newcap = (source->capacity <= SIZE_MAX / 2 ? source->capacity * 2 : SIZE_MAX);
		uint8_t *newbuf;
		J40__SHOULD(newcap < source->capacity, "!mem");
		newbuf = realloc(source->buf, newcap);
		J40__SHOULD(newbuf, "!mem");
		source->checkpoint = source->buf = newbuf;
		source->capacity = newcap;
		st->ptr = source->checkpoint + ptr_pos;
	}

	while (source->size < source->capacity) {
		size_t added_size;
		st->err = source->read_func(
			source->buf + source->size, source->capacity - source->size, &added_size, source->data);
		J40__RAISE_DELAYED();
		if (added_size == 0) break; // EOF or blocking condition
		source->size += added_size;
	}

J40__ON_ERROR:
	return st->err;
}

J40_STATIC void j40__free_source(j40__source *s) {
	if (s->buf_free_func) s->buf_free_func(s->buf);
	if (s->free_func) s->free_func(s->data);
	s->buf = NULL;
	s->buf_free_func = NULL;
	s->read_func = NULL;
	s->free_func = NULL;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// container

typedef struct j40__container_st {
	// the remaining number of bytes in the current box with respect to the reference point,
	// which is `ptr + remaining` in the codestream mode and `ptr` in the container mode.
	// this can be also `UINT64_MAX` in which case this box extends to the end of the file.
	uint64_t box_remaining;

	enum {
		// if set, initial jxl & ftyp boxes have been read
		J40__CONTAINER_CONFIRMED = 1 << 0,

		// currently seen box types, as they have a cardinality and positional requirement
		J40__SEEN_JXLL = 1 << 1, // at most once, before jxlc/jxlp
		J40__SEEN_JXLI = 1 << 2, // at most once
		J40__SEEN_JXLC = 1 << 3, // precludes jxlp, at most once
		J40__SEEN_JXLP = 1 << 4, // precludes jxlc

		// if set, no more jxlc/jxlp boxes are allowed
		J40__NO_MORE_CODESTREAM_BOX = 1 << 5,
	} flags;

	// only applicable after J40__CONTAINER_CONFIRMED is set
	enum {
		J40__BOX_HEADER = 0,
		J40__CODESTREAM_BOX = 1, // box contents of jxlc/jxlp
		J40__NON_CODESTREAM_BOX = 2, // box contents of everything else
	} next;
} j40__container_st;

J40_STATIC J40_RETURNS_ERR j40__container(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__container_u32(j40__st *st, uint32_t *v) {
	J40__SHOULD(st->remaining >= 4, "shrt");
	*v = ((uint32_t) st->ptr[0] << 24) | ((uint32_t) st->ptr[1] << 16) | ((uint32_t) st->ptr[2] << 8) | (uint32_t) st->ptr[3];
	st->ptr += 4;
	st->remaining -= 4;
J40__ON_ERROR:
	return st->err;
}

// size is UINT64_MAX if the box extends indefinitely until the end of file
J40_STATIC J40_RETURNS_ERR j40__box_header(j40__st *st, uint32_t *type, int *brotli, uint64_t *size) {
	uint32_t size32;

	J40__TRY(j40__container_u32(st, &size32));
	J40__TRY(j40__container_u32(st, type));
	if (size32 == 0) {
		*size = UINT64_MAX;
	} else if (size32 == 1) {
		uint32_t sizehi, sizelo;
		J40__TRY(j40__container_u32(st, &sizehi));
		J40__TRY(j40__container_u32(st, &sizelo));
		*size = ((uint64_t) sizehi << 32) | (uint64_t) sizelo;
		J40__SHOULD(*size >= 16, "boxx");
		*size -= 16;
		J40__SHOULD(st->remaining >= *size, "shrt");
	} else {
		J40__SHOULD(size32 >= 8, "boxx");
		*size = size32 - 8;
		J40__SHOULD(st->remaining >= *size, "shrt");
	}

	*brotli = (*type == 0x62726f62 /*brob*/);
	if (*brotli) {
		J40__SHOULD(*size > 4, "brot"); // Brotli stream is never empty so 4 is also out
		J40__TRY(j40__container_u32(st, type));
		J40__SHOULD(*type != 0x62726f62 /*brob*/ && (*type >> 8) != 0x6a786c /*jxl*/, "brot");
		if (*size != UINT64_MAX) *size -= 4;
	}

J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__container(j40__st *st) {
	static const uint8_t JXL_BOX[12] = { // type `JXL `, value 0D 0A 87 0A
		0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
	}, FTYP_BOX[20] = { // type `ftyp`, brand `jxl `, version 0, only compatible w/ brand `jxl `
		0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x6a, 0x78, 0x6c, 0x20,
		0x00, 0x00, 0x00, 0x00, 0x6a, 0x78, 0x6c, 0x20,
	};

	j40__source *source = st->source;
	j40__container_st *container = st->container;

	// ensure that we came here because the logical buffer has been exhausted...
	J40__ASSERT(st->remaining == 0);
	// ...because in the container mode the logical buffer may have to hold multiple boxes
	J40__ASSERT(J40__INBOUNDS(st->ptr, source->buf, source->size));
	st->remaining = source->size - (size_t) (st->ptr - source->buf);

	if (!(container->flags & J40__CONTAINER_CONFIRMED)) {
		// format identification, which should work correctly even with a partial input!
		J40__SHOULD(st->remaining > 0, "shrt");
		if (st->ptr[0] == 0xff) return 0; // a bare codestream
		if (st->remaining < sizeof(JXL_BOX)) {
			// should be a partial container input, otherwise this file stands no chance
			J40__SHOULD(memcmp(st->ptr, JXL_BOX, st->remaining) == 0, "!jxl");
			J40__RAISE("shrt");
		}

		J40__SHOULD(memcmp(st->ptr, JXL_BOX, sizeof(JXL_BOX)) == 0, "!jxl");
		st->ptr += sizeof(JXL_BOX);
		st->remaining -= sizeof(JXL_BOX);
		J40__SHOULD(st->remaining >= sizeof(FTYP_BOX), "shrt");
		J40__SHOULD(memcmp(st->ptr, FTYP_BOX, sizeof(FTYP_BOX)) == 0, "ftyp");
		st->ptr += sizeof(FTYP_BOX);
		st->remaining -= sizeof(FTYP_BOX);

		source->checkpoint = st->ptr;
		container->flags |= J40__CONTAINER_CONFIRMED;
	}

	while (st->remaining > 0) {
		if (container->next == J40__BOX_HEADER) {
			uint32_t type;
			int brotli;
			uint64_t size;
			unsigned new_flags = 0;

			J40__TRY(j40__box_header(st, &type, &brotli, &size));

			// TODO the ordering rule for jxll/jxli may change in the future version of 18181-2
			switch (type) {
			case 0x6a786c6c: // jxll: codestream level
				J40__SHOULD(!(container->flags & J40__SEEN_JXLL), "box?");
				new_flags = J40__SEEN_JXLL;
				break;

			case 0x6a786c69: // jxli: frame index
				J40__SHOULD(!(container->flags & J40__SEEN_JXLI), "box?");
				new_flags = J40__SEEN_JXLI;
				break;

			case 0x6a786c63: // jxlc: single codestream
				J40__ASSERT(!brotli);
				J40__SHOULD(!(container->flags & J40__NO_MORE_CODESTREAM_BOX), "box?");
				J40__SHOULD(!(container->flags & (J40__SEEN_JXLP | J40__SEEN_JXLC)), "box?");
				new_flags = J40__SEEN_JXLC;
				break;

			case 0x6a786c70: // jxlp: partial codestreams
				J40__ASSERT(!brotli);
				J40__SHOULD(!(container->flags & J40__NO_MORE_CODESTREAM_BOX), "box?");
				J40__SHOULD(!(container->flags & J40__SEEN_JXLC), "box?");
				new_flags = J40__SEEN_JXLP;
				J40__SHOULD(size >= 4, "jxlp");
				J40__SHOULD(st->remaining >= 4, "shrt");
				// TODO the partial codestream index is ignored right now
				if (!(st->ptr[0] >> 7)) new_flags |= J40__NO_MORE_CODESTREAM_BOX;
				st->ptr += 4;
				st->remaining -= 4;
				if (size < SIZE_MAX) size -= 4;
				break;
			} // other boxes have no additional requirements and are simply skipped

			container->flags |= new_flags;
			if (new_flags & (J40__SEEN_JXLP | J40__SEEN_JXLC)) {
				container->next = J40__CODESTREAM_BOX;
			} else {
				container->next = J40__NON_CODESTREAM_BOX;
			}
			container->box_remaining = size;
			source->checkpoint = st->ptr;
		}

		J40__ASSERT(container->next != J40__BOX_HEADER);
		if (container->next == J40__CODESTREAM_BOX) {
			// immediately switch to the codestream mode and resume parsing
			if (st->remaining > container->box_remaining) st->remaining = container->box_remaining;
			if (container->box_remaining < SIZE_MAX) {
				container->box_remaining -= st->remaining;
				if (container->box_remaining == 0) container->next = J40__BOX_HEADER;
			}
			return 0;
		} else {
			// try to skip the whole box (and even if it's not possible, commit as much as possible)
			size_t skipsize = st->remaining;
			if (skipsize > container->box_remaining) skipsize = container->box_remaining;
			source->checkpoint = st->ptr += skipsize;
			st->remaining -= skipsize;
			if (container->box_remaining < SIZE_MAX) {
				container->box_remaining -= skipsize;
				J40__SHOULD(container->box_remaining == 0, "shrt");
				container->next = J40__BOX_HEADER;
			}
		}
	}

	J40__SHOULD(container->flags & J40__NO_MORE_CODESTREAM_BOX, "shrt");

J40__ON_ERROR:
	return st->err;
}

// TODO j40__finish_container

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// bitstream

J40_STATIC size_t j40__bits_read(const j40__st *st);

J40_STATIC J40_RETURNS_ERR j40__always_refill(j40__st *st, int32_t n);
// ensure st->nbits is at least n; otherwise pull as many bytes as possible into st->bits
#define j40__refill(st, n) (J40_UNLIKELY(st->nbits < (n)) ? j40__always_refill(st, n) : st->err)

J40_INLINE J40_RETURNS_ERR j40__zero_pad_to_byte(j40__st *st);
J40_STATIC J40_RETURNS_ERR j40__skip(j40__st *st, uint64_t n);

J40_INLINE int32_t j40__u(j40__st *st, int32_t n);
J40_INLINE int32_t j40__u32(
	j40__st *st,
	int32_t o0, int32_t n0, int32_t o1, int32_t n1,
	int32_t o2, int32_t n2, int32_t o3, int32_t n3
);
J40_STATIC uint64_t j40__u64(j40__st *st);
J40_INLINE int32_t j40__enum(j40__st *st);
J40_INLINE float j40__f16(j40__st *st);
J40_STATIC uint64_t j40__varint(j40__st *st);
J40_INLINE int32_t j40__u8(j40__st *st);
J40_INLINE int32_t j40__at_most(j40__st *st, int32_t max);

#ifdef J40_IMPLEMENTATION

J40_STATIC size_t j40__bits_read(const j40__st *st) {
	return (st->source->bytes_before_buf + (size_t) (st->ptr - st->source->buf)) * 8 - (size_t) st->nbits;
}

J40_STATIC J40_RETURNS_ERR j40__always_refill(j40__st *st, int32_t n) {
	J40__ASSERT(0 <= n && n <= 31);
	do {
		size_t consumed = (size_t) ((32 - st->nbits) >> 3);
		if (st->remaining < consumed) {
			while (st->remaining > 0) {
				st->bits |= (uint32_t) *st->ptr++ << st->nbits;
				st->nbits += 8;
				--st->remaining;
			}
			if (st->nbits < n) {
				J40__SHOULD(st->container, "shrt");
				J40__SHOULD(!(st->container->flags & J40__NO_MORE_CODESTREAM_BOX), "shrt");
				J40__TRY(j40__container(st));
				continue; // now we have possibly more bits to refill, try again
			}
		} else {
			st->remaining -= consumed;
			J40__ASSERT(st->nbits <= 24);
			do {
				st->bits |= (uint32_t) *st->ptr++ << st->nbits;
				st->nbits += 8;
			} while (st->nbits <= 24);
		}
	} while (0);
J40__ON_ERROR:
	return st->err;
}

J40_INLINE J40_RETURNS_ERR j40__zero_pad_to_byte(j40__st *st) {
	int32_t n = st->nbits & 7;
	if (st->bits & ((1u << n) - 1)) return J40__ERR("pad0");
	st->bits >>= n;
	st->nbits -= n;
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__skip(j40__st *st, uint64_t n) {
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
	if (st->remaining < bytes) return J40__ERR("shrt");
	st->remaining -= bytes;
	st->ptr += bytes;
	n &= 7;
	if (j40__refill(st, (int32_t) n)) return st->err;
	st->bits >>= (int32_t) n;
	st->nbits -= (int32_t) n;
	return st->err;
}

// TODO there are cases where n > 31
J40_INLINE int32_t j40__u(j40__st *st, int32_t n) {
	int32_t ret;
	J40__ASSERT(0 <= n && n <= 31);
	if (j40__refill(st, n)) return 0;
	ret = (int32_t) (st->bits & ((1u << n) - 1));
	st->bits >>= n;
	st->nbits -= n;
	return ret;
}

// the maximum value U32() actually reads is 2^30 + 4211711, so int32_t should be enough
J40_INLINE int32_t j40__u32(
	j40__st *st,
	int32_t o0, int32_t n0, int32_t o1, int32_t n1,
	int32_t o2, int32_t n2, int32_t o3, int32_t n3
) {
	const int32_t o[4] = { o0, o1, o2, o3 };
	const int32_t n[4] = { n0, n1, n2, n3 };
	int32_t sel;
	J40__ASSERT(0 <= n0 && n0 <= 30 && o0 <= 0x7fffffff - (1 << n0));
	J40__ASSERT(0 <= n1 && n1 <= 30 && o1 <= 0x7fffffff - (1 << n1));
	J40__ASSERT(0 <= n2 && n2 <= 30 && o2 <= 0x7fffffff - (1 << n2));
	J40__ASSERT(0 <= n3 && n3 <= 30 && o3 <= 0x7fffffff - (1 << n3));
	sel = j40__u(st, 2);
	return j40__u(st, n[sel]) + o[sel];
}

J40_STATIC uint64_t j40__u64(j40__st *st) {
	int32_t sel = j40__u(st, 2), shift;
	uint64_t ret = (uint64_t) j40__u(st, sel * 4);
	if (sel < 3) {
		ret += 17u >> (8 - sel * 4);
	} else {
		for (shift = 12; shift < 64 && j40__u(st, 1); shift += 8) {
			ret |= (uint64_t) j40__u(st, shift < 56 ? 8 : 64 - shift) << shift;
		}
	}
	return ret;
}

J40_INLINE int32_t j40__enum(j40__st *st) {
	int32_t ret = j40__u32(st, 0, 0, 1, 0, 2, 4, 18, 6);
	// the spec says it should be 64, but the largest enum value in use is 18 (kHLG);
	// we have to reject unknown enum values anyway so we use a smaller limit to avoid overflow
	if (ret >= 31) return J40__ERR("enum"), 0;
	return ret;
}

J40_INLINE float j40__f16(j40__st *st) {
	int32_t bits = j40__u(st, 16);
	int32_t biased_exp = (bits >> 10) & 0x1f;
	if (biased_exp == 31) return J40__ERR("!fin"), 0.0f;
	return (bits >> 15 ? -1 : 1) * ldexpf((float) ((bits & 0x3ff) | (biased_exp > 0 ? 0x400 : 0)), biased_exp - 25);
}

J40_STATIC uint64_t j40__varint(j40__st *st) { // ICC only
	uint64_t value = 0;
	int32_t shift = 0;
	do {
		if (st->remaining == 0) return J40__ERR("shrt"), (uint64_t) 0;
		int32_t b = j40__u(st, 8);
		value |= (uint64_t) (b & 0x7f) << shift;
		if (b < 128) return value;
		shift += 7;
	} while (shift < 63);
	return J40__ERR("vint"), (uint64_t) 0;
}

J40_INLINE int32_t j40__u8(j40__st *st) { // ANS distribution decoding only
	if (j40__u(st, 1)) {
		int32_t n = j40__u(st, 3);
		return j40__u(st, n) + (1 << n);
	} else {
		return 0;
	}
}

// equivalent to u(ceil(log2(max + 1))), decodes [0, max] with the minimal number of bits
J40_INLINE int32_t j40__at_most(j40__st *st, int32_t max) {
	int32_t v = max > 0 ? j40__u(st, j40__ceil_lg32((uint32_t) max + 1)) : 0;
	if (v > max) return J40__ERR("rnge"), 0;
	return v;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// prefix code

J40_STATIC J40_RETURNS_ERR j40__init_prefix_code(
	j40__st *st, int32_t l2size, int32_t *out_fast_len, int32_t *out_max_len, int32_t **out_table
);
J40_INLINE int32_t j40__prefix_code(j40__st *st, int32_t fast_len, int32_t max_len, const int32_t *table);

#ifdef J40_IMPLEMENTATION

static const uint8_t J40__REV5[32] = {
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

enum { J40__MAX_TYPICAL_FAST_LEN = 7 }; // limit fast_len for typical cases
enum { J40__MAX_TABLE_GROWTH = 2 }; // we can afford 2x the table size if beneficial though

// read a prefix code tree, as specified in RFC 7932 section 3
J40_STATIC J40_RETURNS_ERR j40__init_prefix_code(
	j40__st *st, int32_t l2size, int32_t *out_fast_len, int32_t *out_max_len, int32_t **out_table
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

	J40__ASSERT(l2size > 0 && l2size <= 0x8000);
	if (l2size == 1) { // SPEC missing this case
		*out_fast_len = *out_max_len = 0;
		J40__SHOULD(*out_table = malloc(sizeof(int32_t)), "!mem");
		(*out_table)[0] = 0;
		return 0;
	}

	hskip = j40__u(st, 2);
	if (hskip == 1) { // simple prefix codes (section 3.4)
		static const struct { int8_t maxlen, sortfrom, sortto, len[8], symref[8]; } TEMPLATES[5] = {
			{ 3, 2, 4, {1,2,1,3,1,2,1,3}, {0,1,0,2,0,1,0,3} }, // NSYM=4 tree-select 1 (1233)
			{ 0, 0, 0, {0}, {0} },                             // NSYM=1 (0)
			{ 1, 0, 2, {1,1}, {0,1} },                         // NSYM=2 (11)
			{ 2, 1, 3, {1,2,1,2}, {0,1,0,2} },                 // NSYM=3 (122)
			{ 2, 0, 4, {2,2,2,2}, {0,1,2,3} },                 // NSYM=4 tree-select 0 (2222)
		};
		int32_t nsym = j40__u(st, 2) + 1, syms[4], tmp;
		for (i = 0; i < nsym; ++i) {
			syms[i] = j40__at_most(st, l2size - 1);
			for (j = 0; j < i; ++j) J40__SHOULD(syms[i] != syms[j], "hufd");
		}
		if (nsym == 4 && j40__u(st, 1)) nsym = 0; // tree-select
		J40__RAISE_DELAYED();

		// symbols of the equal length have to be sorted
		for (i = TEMPLATES[nsym].sortfrom + 1; i < TEMPLATES[nsym].sortto; ++i) {
			for (j = i; j > TEMPLATES[nsym].sortfrom && syms[j - 1] > syms[j]; --j) {
				tmp = syms[j - 1];
				syms[j - 1] = syms[j];
				syms[j] = tmp;
			}
		}

		*out_fast_len = *out_max_len = TEMPLATES[nsym].maxlen;
		J40__SHOULD(*out_table = malloc(sizeof(int32_t) << *out_max_len), "!mem");
		for (i = 0; i < (1 << *out_max_len); ++i) {
			(*out_table)[i] = (syms[TEMPLATES[nsym].symref[i]] << 16) | (int32_t) TEMPLATES[nsym].len[i];
		}
		return 0;
	}

	// complex prefix codes (section 3.5): read layer 1 code lengths using the layer 0 code
	total = 0;
	for (i = l1counts[0] = hskip; i < L1SIZE && total < L1CODESUM; ++i) {
		l1lengths[L1ZIGZAG[i]] = code = j40__prefix_code(st, L0MAXLEN, L0MAXLEN, L0TABLE);
		++l1counts[code];
		if (code) total += L1CODESUM >> code;
	}
	J40__SHOULD(total == L1CODESUM && l1counts[0] != i, "hufd");

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
			for (code = (int32_t) J40__REV5[*start]; code < L1CODESUM; code += 1 << n) {
				l1table[code] = (i << 16) | n;
			}
			*start += L1CODESUM >> n;
		}
	}

	{ // read layer 2 code lengths using the layer 1 code
		int32_t prev = 8, rep, prev_rep = 0; // prev_rep: prev repeat count of 16(pos)/17(neg) so far
		J40__SHOULD(l2lengths = calloc((size_t) l2size, sizeof(int32_t)), "!mem");
		for (i = total = 0; i < l2size && total < L2CODESUM; ) {
			code = j40__prefix_code(st, L1MAXLEN, L1MAXLEN, l1table);
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
				rep = (prev_rep > 0 ? 4 * prev_rep - 5 : 3) + j40__u(st, 2);
				total += (L2CODESUM * (rep - prev_rep)) >> prev;
				l2counts[prev] += rep - prev_rep;
				for (; prev_rep < rep; ++prev_rep) l2lengths[i++] = prev;
			} else { // code == 17: repeat zero 3+u(3) times
				if (prev_rep > 0) prev_rep = 0;
				rep = (prev_rep < 0 ? 8 * prev_rep + 13 : -3) - j40__u(st, 3);
				for (; prev_rep > rep; --prev_rep) l2lengths[i++] = 0;
			}
			J40__RAISE_DELAYED();
		}
		J40__SHOULD(total == L2CODESUM, "hufd");
	}

	// determine the layer 2 lookup table size
	l2starts[1] = 0;
	*out_max_len = 1;
	for (i = 2; i <= L2MAXLEN; ++i) {
		l2starts[i] = l2starts[i - 1] + (l2counts[i - 1] << (L2MAXLEN - (i - 1)));
		if (l2counts[i]) *out_max_len = i;
	}
	if (*out_max_len <= J40__MAX_TYPICAL_FAST_LEN) {
		fast_len = *out_max_len;
		J40__SHOULD(l2table = malloc(sizeof(int32_t) << fast_len), "!mem");
	} else {
		// if the distribution is flat enough the max fast_len might be slow
		// because most LUT entries will be overflow refs so we will hit slow paths for most cases.
		// we therefore calculate the table size with the max fast_len,
		// then find the largest fast_len within the specified table growth factor.
		int32_t size, size_limit, size_used;
		fast_len = J40__MAX_TYPICAL_FAST_LEN;
		size = 1 << fast_len;
		for (i = fast_len + 1; i <= *out_max_len; ++i) size += l2counts[i];
		size_used = size;
		size_limit = size * J40__MAX_TABLE_GROWTH;
		for (i = fast_len + 1; i <= *out_max_len; ++i) {
			size = size + (1 << i) - l2counts[i];
			if (size <= size_limit) {
				size_used = size;
				fast_len = i;
			}
		}
		l2overflows[fast_len + 1] = 1 << fast_len;
		for (i = fast_len + 2; i <= *out_max_len; ++i) l2overflows[i] = l2overflows[i - 1] + l2counts[i - 1];
		J40__SHOULD(l2table = malloc(sizeof(int32_t) * (size_t) (size_used + 1)), "!mem");
		// this entry should be unreachable, but should work as a stopper if there happens to be a logic bug
		l2table[size_used] = 0;
	}

	// fill the layer 2 table
	for (i = 0; i < l2size; ++i) {
		int32_t n = l2lengths[i], *start = &l2starts[n];
		if (n == 0) continue;
		code = (int32_t) J40__REV5[*start & 31] << 10 |
			(int32_t) J40__REV5[*start >> 5 & 31] << 5 |
			(int32_t) J40__REV5[*start >> 10];
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

J40__ON_ERROR:
	free(l2lengths);
	free(l2table);
	return st->err;
}

J40_STATIC int32_t j40__match_overflow(j40__st *st, int32_t fast_len, const int32_t *table) {
	int32_t entry, code, code_len;
	st->nbits -= fast_len;
	st->bits >>= fast_len;
	do {
		entry = *table++;
		code = (entry >> 4) & 0xfff;
		code_len = entry & 15;
	} while (code != (int32_t) (st->bits & ((1u << code_len) - 1)));
	return entry;
}

J40_INLINE int32_t j40__prefix_code(j40__st *st, int32_t fast_len, int32_t max_len, const int32_t *table) {
	int32_t entry, code_len;
	if (st->nbits < max_len && j40__always_refill(st, 0)) return 0;
	entry = table[st->bits & ((1u << fast_len) - 1)];
	if (entry < 0 && fast_len < max_len) entry = j40__match_overflow(st, fast_len, table - entry);
	code_len = entry & 15;
	st->nbits -= code_len;
	st->bits >>= code_len;
	return entry >> 16;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// hybrid integer encoding

// token < 2^split_exp is interpreted as is.
// otherwise (token - 2^split_exp) is split into NNHHHLLL where config determines H/L lengths.
// then MMMMM = u(NN + split_exp - H/L lengths) is read; the decoded value is 1HHHMMMMMLLL.
typedef struct { int8_t split_exp, msb_in_token, lsb_in_token; } j40__hybrid_int_config_t;

J40_STATIC J40_RETURNS_ERR j40__hybrid_int_config(j40__st *st, int32_t log_alpha_size, j40__hybrid_int_config_t *out);
J40_INLINE int32_t j40__hybrid_int(j40__st *st, int32_t token, j40__hybrid_int_config_t config);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__hybrid_int_config(j40__st *st, int32_t log_alpha_size, j40__hybrid_int_config_t *out) {
	out->split_exp = (int8_t) j40__at_most(st, log_alpha_size);
	if (out->split_exp != log_alpha_size) {
		out->msb_in_token = (int8_t) j40__at_most(st, out->split_exp);
		out->lsb_in_token = (int8_t) j40__at_most(st, out->split_exp - out->msb_in_token);
	} else {
		out->msb_in_token = out->lsb_in_token = 0;
	}
	return st->err;
}

J40_INLINE int32_t j40__hybrid_int(j40__st *st, int32_t token, j40__hybrid_int_config_t config) {
	int32_t midbits, lo, mid, hi, top, bits_in_token, split = 1 << config.split_exp;
	if (token < split) return token;
	bits_in_token = config.msb_in_token + config.lsb_in_token;
	midbits = config.split_exp - bits_in_token + ((token - split) >> bits_in_token);
	// TODO midbits can overflow!
	mid = j40__u(st, midbits);
	top = 1 << config.msb_in_token;
	lo = token & ((1 << config.lsb_in_token) - 1);
	hi = (token >> config.lsb_in_token) & (top - 1);
	return ((top | hi) << (midbits + config.lsb_in_token)) | ((mid << config.lsb_in_token) | lo);
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// rANS alias table

enum {
	J40__DIST_BITS = 12,
	J40__ANS_INIT_STATE = 0x130000
};

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
typedef struct { int16_t cutoff, offset_or_next, symbol; } j40__alias_bucket;

J40_STATIC J40_RETURNS_ERR j40__init_alias_map(
	j40__st *st, const int16_t *D, int32_t log_alpha_size, j40__alias_bucket **out
);
J40_STATIC int32_t j40__ans_code(
	j40__st *st, uint32_t *state, int32_t log_bucket_size,
	const int16_t *D, const j40__alias_bucket *aliases
);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__init_alias_map(
	j40__st *st, const int16_t *D, int32_t log_alpha_size, j40__alias_bucket **out
) {
	int16_t log_bucket_size = (int16_t) (J40__DIST_BITS - log_alpha_size);
	int16_t bucket_size = (int16_t) (1 << log_bucket_size);
	int16_t table_size = (int16_t) (1 << log_alpha_size);
	j40__alias_bucket *buckets = NULL;
	// the underfull and overfull stacks are implicit linked lists; u/o resp. is the top index,
	// buckets[u/o].next is the second-to-top index and so on. an index -1 indicates the bottom.
	int16_t u = -1, o = -1, i, j;

	J40__ASSERT(5 <= log_alpha_size && log_alpha_size <= 8);
	J40__SHOULD(buckets = malloc(sizeof(j40__alias_bucket) << log_alpha_size), "!mem");

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
		J40__ASSERT(u >= 0);
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

	J40__ASSERT(u < 0);
	*out = buckets;
	return 0;

J40__ON_ERROR:
	free(buckets);
	return st->err;
}

J40_STATIC int32_t j40__ans_code(
	j40__st *st, uint32_t *state, int32_t log_bucket_size,
	const int16_t *D, const j40__alias_bucket *aliases
) {
	if (*state == 0) {
		*state = (uint32_t) j40__u(st, 16);
		*state |= (uint32_t) j40__u(st, 16) << 16;
	}
	{
		int32_t index = (int32_t) (*state & 0xfff);
		int32_t i = index >> log_bucket_size;
		int32_t pos = index & ((1 << log_bucket_size) - 1);
		const j40__alias_bucket *bucket = &aliases[i];
		int32_t symbol = pos < bucket->cutoff ? i : bucket->symbol;
		int32_t offset = pos < bucket->cutoff ? 0 : bucket->offset_or_next /*offset*/;
		J40__ASSERT(D[symbol] != 0);
		*state = (uint32_t) D[symbol] * (*state >> 12) + (uint32_t) offset + (uint32_t) pos;
		if (*state < (1u << 16)) *state = (*state << 16) | (uint32_t) j40__u(st, 16);
		return symbol;
	}
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// entropy code

typedef union {
	j40__hybrid_int_config_t config;
	struct {
		j40__hybrid_int_config_t config;
		int32_t count;
	} init; // only used during the initialization
	struct {
		j40__hybrid_int_config_t config;
		int16_t *D;
		j40__alias_bucket *aliases;
	} ans; // if parent use_prefix_code is false
	struct {
		j40__hybrid_int_config_t config;
		int16_t fast_len, max_len;
		int32_t *table;
	} prefix; // if parent use_prefix_code is true
} j40__code_cluster_t;

typedef struct {
	int32_t num_dist;
	int lz77_enabled, use_prefix_code;
	int32_t min_symbol, min_length;
	int32_t log_alpha_size; // only used when use_prefix_code is false
	int32_t num_clusters; // in [1, min(num_dist, 256)]
	uint8_t *cluster_map; // each in [0, num_clusters)
	j40__hybrid_int_config_t lz_len_config;
	j40__code_cluster_t *clusters;
} j40__code_spec_t;

typedef struct {
	const j40__code_spec_t *spec;
	// LZ77 states
	int32_t num_to_copy, copy_pos, num_decoded;
	int32_t window_cap, *window;
	// ANS state (SPEC there is a single such state throughout the whole ANS stream)
	uint32_t ans_state; // 0 if uninitialized
} j40__code_t;

J40_STATIC J40_RETURNS_ERR j40__cluster_map(
	j40__st *st, int32_t num_dist, int32_t max_allowed, int32_t *num_clusters, uint8_t *map
);
J40_STATIC J40_RETURNS_ERR j40__code_spec(j40__st *st, int32_t num_dist, j40__code_spec_t *spec);
J40_STATIC int32_t j40__entropy_code_cluster(
	j40__st *st, int use_prefix_code, int32_t log_alpha_size,
	j40__code_cluster_t *cluster, uint32_t *ans_state
);
J40_STATIC int32_t j40__code(j40__st *st, int32_t ctx, int32_t dist_mult, j40__code_t *code);
J40_STATIC void j40__free_code(j40__code_t *code);
J40_STATIC J40_RETURNS_ERR j40__finish_and_free_code(j40__st *st, j40__code_t *code);
J40_STATIC void j40__free_code_spec(j40__code_spec_t *spec);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__cluster_map(
	j40__st *st, int32_t num_dist, int32_t max_allowed, int32_t *num_clusters, uint8_t *map
) {
	j40__code_spec_t codespec = {0}; // cluster map might be recursively coded
	j40__code_t code = { .spec = &codespec };
	uint32_t seen[8] = {0};
	int32_t i, j;

	J40__ASSERT(max_allowed >= 1 && max_allowed <= 256);
	if (max_allowed > num_dist) max_allowed = num_dist;

	if (num_dist == 1) { // SPEC impossible in Brotli but possible (and unspecified) in JPEG XL
		*num_clusters = 1;
		map[0] = 0;
		return 0;
	}

	if (j40__u(st, 1)) { // is_simple (# clusters < 8)
		int32_t nbits = j40__u(st, 2);
		for (i = 0; i < num_dist; ++i) {
			map[i] = (uint8_t) j40__u(st, nbits);
			J40__SHOULD((int32_t) map[i] < max_allowed, "clst");
		}
	} else {
		int use_mtf = j40__u(st, 1);

		// num_dist=1 prevents further recursion
		J40__TRY(j40__code_spec(st, 1, &codespec));
		for (i = 0; i < num_dist; ++i) {
			int32_t index = j40__code(st, 0, 0, &code); // SPEC context (always 0) is missing
			J40__SHOULD(index < max_allowed, "clst");
			map[i] = (uint8_t) index;
		}
		J40__TRY(j40__finish_and_free_code(st, &code));
		j40__free_code_spec(&codespec);

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
	J40__ASSERT(i > 0);
	*num_clusters = i; // the first unset position or 256 if none
	for (; i < 256 && !(seen[i >> 5] >> (i & 31) & 1); ++i);
	J40__SHOULD(i == 256, "clst"); // no more set position beyond num_clusters

	return 0;

J40__ON_ERROR:
	j40__free_code(&code);
	j40__free_code_spec(&codespec);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__code_spec(j40__st *st, int32_t num_dist, j40__code_spec_t *spec) {
	int32_t i, j;

	spec->cluster_map = NULL;
	spec->clusters = NULL;

	// LZ77Params
	spec->lz77_enabled = j40__u(st, 1);
	if (spec->lz77_enabled) {
		spec->min_symbol = j40__u32(st, 224, 0, 512, 0, 4096, 0, 8, 15);
		spec->min_length = j40__u32(st, 3, 0, 4, 0, 5, 2, 9, 8);
		J40__TRY(j40__hybrid_int_config(st, 8, &spec->lz_len_config));
		++num_dist; // num_dist - 1 is a synthesized LZ77 length distribution
	} else {
		spec->min_symbol = spec->min_length = 0x7fffffff;
	}

	// cluster_map: a mapping from context IDs to actual distributions
	J40__SHOULD(spec->cluster_map = malloc(sizeof(uint8_t) * (size_t) num_dist), "!mem");
	J40__TRY(j40__cluster_map(st, num_dist, 256, &spec->num_clusters, spec->cluster_map));

	J40__SHOULD(spec->clusters = calloc((size_t) spec->num_clusters, sizeof(j40__code_cluster_t)), "!mem");

	spec->use_prefix_code = j40__u(st, 1);
	if (spec->use_prefix_code) {
		for (i = 0; i < spec->num_clusters; ++i) { // SPEC the count is off by one
			J40__TRY(j40__hybrid_int_config(st, 15, &spec->clusters[i].config));
		}

		for (i = 0; i < spec->num_clusters; ++i) {
			if (j40__u(st, 1)) {
				int32_t n = j40__u(st, 4);
				spec->clusters[i].init.count = 1 + (1 << n) + j40__u(st, n);
				J40__SHOULD(spec->clusters[i].init.count <= (1 << 15), "hufd");
			} else {
				spec->clusters[i].init.count = 1;
			}
		}

		// SPEC this should happen after reading *all* count[i]
		for (i = 0; i < spec->num_clusters; ++i) {
			j40__code_cluster_t *c = &spec->clusters[i];
			int32_t fast_len, max_len;
			J40__TRY(j40__init_prefix_code(st, c->init.count, &fast_len, &max_len, &c->prefix.table));
			c->prefix.fast_len = (int16_t) fast_len;
			c->prefix.max_len = (int16_t) max_len;
		}
	} else {
		enum { DISTBITS = J40__DIST_BITS, DISTSUM = 1 << DISTBITS };

		spec->log_alpha_size = 5 + j40__u(st, 2);
		for (i = 0; i < spec->num_clusters; ++i) { // SPEC the count is off by one
			J40__TRY(j40__hybrid_int_config(st, spec->log_alpha_size, &spec->clusters[i].config));
		}

		for (i = 0; i < spec->num_clusters; ++i) {
			int32_t table_size = 1 << spec->log_alpha_size;
			int16_t *D;
			J40__SHOULD(D = malloc(sizeof(int16_t) * (size_t) table_size), "!mem");
			spec->clusters[i].ans.D = D;

			switch (j40__u(st, 2)) {
			case 1: // one entry
				memset(D, 0, sizeof(int16_t) * (size_t) table_size);
				D[j40__u8(st)] = DISTSUM;
				break;

			case 3: { // two entries
				int32_t v1 = j40__u8(st);
				int32_t v2 = j40__u8(st);
				J40__SHOULD(v1 != v2 && v1 < table_size && v2 < table_size, "ansd");
				memset(D, 0, sizeof(int16_t) * (size_t) table_size);
				D[v1] = (int16_t) j40__u(st, DISTBITS);
				D[v2] = (int16_t) (DISTSUM - D[v1]);
				break;
			}

			case 2: { // evenly distribute to first `alpha_size` entries (false -> true)
				int32_t alpha_size = j40__u8(st) + 1;
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

				len = j40__u(st, 1) ? j40__u(st, 1) ? j40__u(st, 1) ? 3 : 2 : 1 : 0;
				shift = j40__u(st, len) + (1 << len) - 1;
				J40__SHOULD(shift <= 13, "ansd");
				alpha_size = j40__u8(st) + 3;

				omit_log = -1; // there should be at least one non-RLE code
				for (j = ncodes = 0; j < alpha_size; ) {
					static const int32_t TABLE[] = { // reinterpretation of kLogCountLut
						0xa0003,     -16, 0x70003, 0x30004, 0x60003, 0x80003, 0x90003, 0x50004,
						0xa0003, 0x40004, 0x70003, 0x10004, 0x60003, 0x80003, 0x90003, 0x20004,
						0x00011, 0xb0022, 0xc0003, 0xd0043, // overflow for ...0001
					};
					code = j40__prefix_code(st, 4, 7, TABLE);
					if (code < 13) {
						++j;
						codes[ncodes++] = code;
						if (omit_log < code) omit_log = code;
					} else {
						j += code = j40__u8(st) + 4;
						codes[ncodes++] = -code;
					}
				}
				J40__SHOULD(j == alpha_size && omit_log >= 0, "ansd");

				omit_pos = -1;
				for (j = n = total = 0; j < ncodes; ++j) {
					code = codes[j];
					if (code < 0) { // repeat
						int16_t prev = n > 0 ? D[n - 1] : 0;
						J40__SHOULD(prev >= 0, "ansd"); // implicit D[n] followed by RLE
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
						bitcount = j40__min32(j40__max32(0, shift - ((DISTBITS - code) >> 1)), code);
						code = (1 << code) + (j40__u(st, bitcount) << (code - bitcount));
						total += code;
						D[n++] = (int16_t) code;
					}
				}
				for (; n < table_size; ++n) D[n] = 0;
				J40__ASSERT(omit_pos >= 0);
				J40__SHOULD(total <= DISTSUM, "ansd");
				D[omit_pos] = (int16_t) (DISTSUM - total);
				break;
			}

			default: J40__UNREACHABLE();
			}

			J40__TRY(j40__init_alias_map(st, D, spec->log_alpha_size, &spec->clusters[i].ans.aliases));
		}
	}

	spec->num_dist = num_dist;
	return 0;

J40__ON_ERROR:
	j40__free_code_spec(spec);
	return st->err;
}

J40_STATIC int32_t j40__entropy_code_cluster(
	j40__st *st, int use_prefix_code, int32_t log_alpha_size,
	j40__code_cluster_t *cluster, uint32_t *ans_state
) {
	if (use_prefix_code) {
		return j40__prefix_code(st, cluster->prefix.fast_len, cluster->prefix.max_len, cluster->prefix.table);
	} else {
		return j40__ans_code(st, ans_state, J40__DIST_BITS - log_alpha_size, cluster->ans.D, cluster->ans.aliases);
	}
}

// aka DecodeHybridVarLenUint
J40_STATIC int32_t j40__code(j40__st *st, int32_t ctx, int32_t dist_mult, j40__code_t *code) {
	const j40__code_spec_t *spec = code->spec;
	int32_t token, distance, log_alpha_size;
	j40__code_cluster_t *cluster;
	int use_prefix_code;

	if (code->num_to_copy > 0) {
continue_lz77:
		--code->num_to_copy;
		return code->window[code->num_decoded++ & 0xfffff] = code->window[code->copy_pos++ & 0xfffff];
	}

	J40__ASSERT(ctx < spec->num_dist);
	use_prefix_code = spec->use_prefix_code;
	log_alpha_size = spec->log_alpha_size;
	cluster = &spec->clusters[spec->cluster_map[ctx]];
	token = j40__entropy_code_cluster(st, use_prefix_code, log_alpha_size, cluster, &code->ans_state);
	if (token >= spec->min_symbol) { // this is large enough if lz77_enabled is false
		j40__code_cluster_t *lz_cluster = &spec->clusters[spec->cluster_map[spec->num_dist - 1]];
		code->num_to_copy = j40__hybrid_int(st, token - spec->min_symbol, spec->lz_len_config) + spec->min_length;
		token = j40__entropy_code_cluster(st, use_prefix_code, log_alpha_size, lz_cluster, &code->ans_state);
		distance = j40__hybrid_int(st, token, lz_cluster->config);
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

	token = j40__hybrid_int(st, token, cluster->config);
	if (st->err) return 0;
	if (spec->lz77_enabled) {
		if (!code->window) { // XXX should be dynamically resized
			code->window = malloc(sizeof(int32_t) << 20);
			if (!code->window) return J40__ERR("!mem"), 0;
		}
		code->window[code->num_decoded++ & 0xfffff] = token;
	}
	return token;
}

J40_STATIC void j40__free_code(j40__code_t *code) {
	free(code->window);
	code->window = NULL;
	code->window_cap = 0;
}

J40_STATIC J40_RETURNS_ERR j40__finish_and_free_code(j40__st *st, j40__code_t *code) {
	if (!code->spec->use_prefix_code) {
		if (code->ans_state) {
			J40__SHOULD(code->ans_state == J40__ANS_INIT_STATE, "ans?");
		} else { // edge case: if no symbols have been read the state has to be read at this point
			J40__SHOULD(j40__u(st, 16) == (J40__ANS_INIT_STATE & 0xffff), "ans?");
			J40__SHOULD(j40__u(st, 16) == (J40__ANS_INIT_STATE >> 16), "ans?");
		}
	}
	// it's explicitly allowed that num_to_copy can be > 0 at the end of stream
J40__ON_ERROR:
	j40__free_code(code);
	return st->err;
}

J40_STATIC void j40__free_code_spec(j40__code_spec_t *spec) {
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

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// image header & metadata

enum {
	J40__CHROMA_WHITE = 0, J40__CHROMA_RED = 1,
	J40__CHROMA_GREEN = 2, J40__CHROMA_BLUE = 3,
};

typedef struct {
	enum {
		J40__EC_ALPHA = 0, J40__EC_DEPTH = 1, J40__EC_SPOT_COLOUR = 2,
		J40__EC_SELECTION_MASK = 3, J40__EC_BLACK = 4, J40__EC_CFA = 5,
		J40__EC_THERMAL = 6, J40__EC_NON_OPTIONAL = 15, J40__EC_OPTIONAL = 16,
	} type;
	int32_t bpp, exp_bits, dim_shift, name_len;
	char *name;
	union {
		int alpha_associated;
		struct { float red, green, blue, solidity; } spot;
		int32_t cfa_channel;
	} data;
} j40__ec_info;

typedef struct j40__image_st {
	int32_t width, height;
	enum {
		J40__ORIENT_TL = 1, J40__ORIENT_TR = 2, J40__ORIENT_BR = 3, J40__ORIENT_BL = 4,
		J40__ORIENT_LT = 5, J40__ORIENT_RT = 6, J40__ORIENT_RB = 7, J40__ORIENT_LB = 8,
	} orientation;
	int32_t intr_width, intr_height; // 0 if not specified
	int bpp, exp_bits;

	int32_t anim_tps_num, anim_tps_denom; // num=denom=0 if not animated
	int32_t anim_nloops; // 0 if infinity
	int anim_have_timecodes;

	char *icc;
	size_t iccsize;
	enum { J40__CS_CHROMA = 'c', J40__CS_GREY = 'g', J40__CS_XYB = 'x' } cspace;
	float cpoints[4 /*J40__CHROMA_xxx*/][2 /*x=0, y=1*/]; // only for J40__CS_CHROMA
	enum {
		J40__TF_709 = -1, J40__TF_UNKNOWN = -2, J40__TF_LINEAR = -8, J40__TF_SRGB = -13,
		J40__TF_PQ = -16, J40__TF_DCI = -17, J40__TF_HLG = -18,
		J40__GAMMA_MAX = 10000000,
	} gamma_or_tf; // gamma if > 0, transfer function if <= 0
	enum { J40__INTENT_PERC = 0, J40__INTENT_REL = 1, J40__INTENT_SAT = 2, J40__INTENT_ABS = 3 } render_intent;
	float intensity_target, min_nits; // 0 < min_nits <= intensity_target
	float linear_below; // absolute (nits) if >= 0; a negated ratio of max display brightness if [-1,0]

	int modular_16bit_buffers;
	int num_extra_channels;
	j40__ec_info *ec_info;
	int xyb_encoded;
	float opsin_inv_mat[3][3], opsin_bias[3], quant_bias[3 /*xyb*/], quant_bias_num;
	int want_icc;
} j40__image_st;

J40_STATIC J40_RETURNS_ERR j40__size_header(j40__st *st, int32_t *outw, int32_t *outh);
J40_STATIC J40_RETURNS_ERR j40__bit_depth(j40__st *st, int32_t *outbpp, int32_t *outexpbits);
J40_STATIC J40_RETURNS_ERR j40__name(j40__st *st, int32_t *outlen, char **outbuf);
J40_STATIC J40_RETURNS_ERR j40__customxy(j40__st *st, float xy[2]);
J40_STATIC J40_RETURNS_ERR j40__extensions(j40__st *st);
J40_STATIC J40_RETURNS_ERR j40__image_metadata(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__size_header(j40__st *st, int32_t *outw, int32_t *outh) {
	int32_t div8 = j40__u(st, 1);
	*outh = div8 ? (j40__u(st, 5) + 1) * 8 : j40__u32(st, 1, 9, 1, 13, 1, 18, 1, 30);
	switch (j40__u(st, 3)) { // ratio
	case 0: *outw = div8 ? (j40__u(st, 5) + 1) * 8 : j40__u32(st, 1, 9, 1, 13, 1, 18, 1, 30); break;
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
		J40__SHOULD(*outh < 0x40000000, "bigg");
		*outw = *outh * 2;
		break;
	default: J40__UNREACHABLE();
	}
J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__bit_depth(j40__st *st, int32_t *outbpp, int32_t *outexpbits) {
	if (j40__u(st, 1)) { // float_sample
		int32_t mantissa_bits;
		*outbpp = j40__u32(st, 32, 0, 16, 0, 24, 0, 1, 6);
		*outexpbits = j40__u(st, 4) + 1;
		mantissa_bits = *outbpp - *outexpbits - 1;
		J40__SHOULD(2 <= mantissa_bits && mantissa_bits <= 23, "bpp?");
		J40__SHOULD(2 <= *outexpbits && *outexpbits <= 8, "exp?"); // implies bpp in [5,32] when combined
	} else {
		*outbpp = j40__u32(st, 8, 0, 10, 0, 12, 0, 1, 6);
		*outexpbits = 0;
		J40__SHOULD(1 <= *outbpp && *outbpp <= 31, "bpp?");
	}
J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__name(j40__st *st, int32_t *outlen, char **outbuf) {
	char *buf = NULL;
	int32_t i, c, cc, len;
	len = j40__u32(st, 0, 0, 0, 4, 16, 5, 48, 10);
	if (len > 0) {
		J40__SHOULD(buf = malloc((size_t) len + 1), "!mem");
		for (i = 0; i < len; ++i) {
			buf[i] = (char) j40__u(st, 8);
			J40__RAISE_DELAYED();
		}
		buf[len] = 0;
		for (i = 0; i < len; ) { // UTF-8 verification
			c = (uint8_t) buf[i++];
			cc = (uint8_t) buf[i]; // always accessible thanks to null-termination
			c = c < 0x80 ? 0 : c < 0xc2 ? -1 : c < 0xe0 ? 1 :
				c < 0xf0 ? (c == 0xe0 ? cc >= 0xa0 : c == 0xed ? cc < 0xa0 : 1) ? 2 : -1 :
				c < 0xf5 ? (c == 0xf0 ? cc >= 0x90 : c == 0xf4 ? cc < 0x90 : 1) ? 3 : -1 : -1;
			J40__SHOULD(c >= 0 && i + c < len, "name");
			while (c-- > 0) J40__SHOULD((buf[i++] & 0xc0) == 0x80, "name");
		}
		*outbuf = buf;
	} else {
		J40__RAISE_DELAYED();
		*outbuf = NULL;
	}
	*outlen = len;
	return 0;
J40__ON_ERROR:
	free(buf);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__customxy(j40__st *st, float xy[2]) {
	xy[0] = (float)j40__unpack_signed(j40__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21)) / 100000.0f;
	xy[1] = (float)j40__unpack_signed(j40__u32(st, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21)) / 100000.0f;
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__extensions(j40__st *st) {
	uint64_t extensions = j40__u64(st);
	uint64_t nbits = 0;
	int32_t i;
	for (i = 0; i < 64; ++i) {
		if (extensions >> i & 1) {
			uint64_t n = j40__u64(st);
			J40__RAISE_DELAYED();
			if (nbits > UINT64_MAX - n) J40__RAISE("over");
			nbits += n;
		}
	}
	return j40__skip(st, nbits);
J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__image_metadata(j40__st *st) {
	static const float SRGB_CHROMA[4][2] = { // default chromacity (kD65, kSRGB)
		{0.3127f, 0.3290f}, {0.639998686f, 0.330010138f},
		{0.300003784f, 0.600003357f}, {0.150002046f, 0.059997204f},
	};

	j40__image_st *im = st->image;
	int32_t i, j;

	im->orientation = J40__ORIENT_TL;
	im->intr_width = 0;
	im->intr_height = 0;
	im->bpp = 8;
	im->exp_bits = 0;
	im->anim_tps_num = 0;
	im->anim_tps_denom = 0;
	im->anim_nloops = 0;
	im->anim_have_timecodes = 0;
	im->icc = NULL;
	im->iccsize = 0;
	im->cspace = J40__CS_CHROMA;
	memcpy(im->cpoints, SRGB_CHROMA, sizeof SRGB_CHROMA);
	im->gamma_or_tf = J40__TF_SRGB;
	im->render_intent = J40__INTENT_REL;
	im->intensity_target = 255.0f;
	im->min_nits = 0.0f;
	im->linear_below = 0.0f;

	if (!j40__u(st, 1)) { // !all_default
		int32_t extra_fields = j40__u(st, 1);
		if (extra_fields) {
			im->orientation = j40__u(st, 3) + 1;
			if (j40__u(st, 1)) { // have_intr_size
				J40__TRY(j40__size_header(st, &im->intr_width, &im->intr_height));
			}
			if (j40__u(st, 1)) { // have_preview
				J40__RAISE("TODO: preview");
			}
			if (j40__u(st, 1)) { // have_animation
				im->anim_tps_num = j40__u32(st, 100, 0, 1000, 0, 1, 10, 1, 30);
				im->anim_tps_denom = j40__u32(st, 1, 0, 1001, 0, 1, 8, 1, 10);
				im->anim_nloops = j40__u32(st, 0, 0, 0, 3, 0, 16, 0, 32);
				im->anim_have_timecodes = j40__u(st, 1);
			}
		}
		J40__TRY(j40__bit_depth(st, &im->bpp, &im->exp_bits));
		im->modular_16bit_buffers = j40__u(st, 1);
		im->num_extra_channels = j40__u32(st, 0, 0, 1, 0, 2, 4, 1, 12);
		J40__SHOULD(im->ec_info = calloc((size_t) im->num_extra_channels, sizeof(j40__ec_info)), "!mem");
		for (i = 0; i < im->num_extra_channels; ++i) {
			j40__ec_info *ec = &im->ec_info[i];
			if (j40__u(st, 1)) { // d_alpha
				ec->type = J40__EC_ALPHA;
				ec->bpp = 8;
				ec->exp_bits = ec->dim_shift = ec->name_len = 0;
				ec->name = NULL;
				ec->data.alpha_associated = 0;
			} else {
				ec->type = j40__enum(st);
				J40__TRY(j40__bit_depth(st, &ec->bpp, &ec->exp_bits));
				ec->dim_shift = j40__u32(st, 0, 0, 3, 0, 4, 0, 1, 3);
				J40__TRY(j40__name(st, &ec->name_len, &ec->name));
				switch (ec->type) {
				case J40__EC_ALPHA:
					ec->data.alpha_associated = j40__u(st, 1);
					break;
				case J40__EC_SPOT_COLOUR:
					ec->data.spot.red = j40__f16(st);
					ec->data.spot.green = j40__f16(st);
					ec->data.spot.blue = j40__f16(st);
					ec->data.spot.solidity = j40__f16(st);
					break;
				case J40__EC_CFA:
					ec->data.cfa_channel = j40__u32(st, 1, 0, 0, 2, 3, 4, 19, 8);
					break;
				case J40__EC_DEPTH: case J40__EC_SELECTION_MASK: case J40__EC_BLACK:
				case J40__EC_THERMAL: case J40__EC_NON_OPTIONAL: case J40__EC_OPTIONAL:
					break;
				default: J40__RAISE("ect?");
				}
			}
			J40__RAISE_DELAYED();
		}
		im->xyb_encoded = j40__u(st, 1);
		if (!j40__u(st, 1)) { // ColourEncoding.all_default
			enum { CS_RGB = 0, CS_GREY = 1, CS_XYB = 2, CS_UNKNOWN = 3 } cspace;
			enum { WP_D65 = 1, WP_CUSTOM = 2, WP_E = 10, WP_DCI = 11 };
			enum { PR_SRGB = 1, PR_CUSTOM = 2, PR_2100 = 9, PR_P3 = 11 };
			im->want_icc = j40__u(st, 1);
			cspace = j40__enum(st);
			switch (cspace) {
			case CS_RGB: case CS_UNKNOWN: im->cspace = J40__CS_CHROMA; break;
			case CS_GREY: im->cspace = J40__CS_GREY; break;
			case CS_XYB: im->cspace = J40__CS_XYB; break;
			default: J40__RAISE("csp?");
			}
			// TODO: should verify cspace grayness with ICC grayness
			if (!im->want_icc) {
				if (cspace != CS_XYB) {
					static const float E[2] = {1/3.f, 1/3.f}, DCI[2] = {0.314f, 0.351f},
						BT2100[3][2] = {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}},
						P3[3][2] = {{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}};
					switch (j40__enum(st)) {
					case WP_D65: break; // default
					case WP_CUSTOM: J40__TRY(j40__customxy(st, im->cpoints[J40__CHROMA_WHITE])); break;
					case WP_E: memcpy(im->cpoints + J40__CHROMA_WHITE, E, sizeof E); break;
					case WP_DCI: memcpy(im->cpoints + J40__CHROMA_WHITE, DCI, sizeof DCI); break;
					default: J40__RAISE("wpt?");
					}
					if (cspace != CS_GREY) {
						switch (j40__enum(st)) {
						case PR_SRGB: break; // default
						case PR_CUSTOM:
							J40__TRY(j40__customxy(st, im->cpoints[J40__CHROMA_RED]));
							J40__TRY(j40__customxy(st, im->cpoints[J40__CHROMA_GREEN]));
							J40__TRY(j40__customxy(st, im->cpoints[J40__CHROMA_BLUE]));
							break;
						case PR_2100: memcpy(im->cpoints + J40__CHROMA_RED, BT2100, sizeof BT2100); break;
						case PR_P3: memcpy(im->cpoints + J40__CHROMA_RED, P3, sizeof P3); break;
						default: J40__RAISE("prm?");
						}
					}
				}
				if (j40__u(st, 1)) { // have_gamma
					im->gamma_or_tf = j40__u(st, 24);
					J40__SHOULD(im->gamma_or_tf > 0 && im->gamma_or_tf <= J40__GAMMA_MAX, "gama");
					if (cspace == CS_XYB) J40__SHOULD(im->gamma_or_tf == 3333333, "gama");
				} else {
					im->gamma_or_tf = -j40__enum(st);
					J40__SHOULD((
						1 << -J40__TF_709 | 1 << -J40__TF_UNKNOWN | 1 << -J40__TF_LINEAR |
						1 << -J40__TF_SRGB | 1 << -J40__TF_PQ | 1 << -J40__TF_DCI |
						1 << -J40__TF_HLG
					) >> -im->gamma_or_tf & 1, "tfn?");
				}
				im->render_intent = j40__enum(st);
				J40__SHOULD((
					1 << J40__INTENT_PERC | 1 << J40__INTENT_REL |
					1 << J40__INTENT_SAT | 1 << J40__INTENT_ABS
				) >> im->render_intent & 1, "itt?");
			}
		}
		if (extra_fields) {
			if (!j40__u(st, 1)) { // ToneMapping.all_default
				int relative_to_max_display;
				im->intensity_target = j40__f16(st);
				J40__SHOULD(im->intensity_target > 0, "tone");
				im->min_nits = j40__f16(st);
				J40__SHOULD(0 < im->min_nits && im->min_nits <= im->intensity_target, "tone");
				relative_to_max_display = j40__u(st, 1);
				im->linear_below = j40__f16(st);
				if (relative_to_max_display) {
					J40__SHOULD(0 <= im->linear_below && im->linear_below <= 1, "tone");
					im->linear_below *= -1.0f;
				} else {
					J40__SHOULD(0 <= im->linear_below, "tone");
				}
			}
		}
		J40__TRY(j40__extensions(st));
	}
	if (!j40__u(st, 1)) { // !default_m
		int32_t cw_mask;
		if (im->xyb_encoded) {
			for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) im->opsin_inv_mat[i][j] = j40__f16(st);
			for (i = 0; i < 3; ++i) im->opsin_bias[i] = j40__f16(st);
			for (i = 0; i < 3; ++i) im->quant_bias[i] = j40__f16(st);
			im->quant_bias_num = j40__f16(st);
		}
		cw_mask = j40__u(st, 3);
		if (cw_mask & 1) {
			J40__RAISE("TODO: up2_weight");
		}
		if (cw_mask & 2) {
			J40__RAISE("TODO: up4_weight");
		}
		if (cw_mask & 4) {
			J40__RAISE("TODO: up8_weight");
		}
	}
	J40__RAISE_DELAYED();
	return 0;

J40__ON_ERROR:
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// ICC

J40_STATIC J40_RETURNS_ERR j40__icc(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__icc(j40__st *st) {
	size_t enc_size, index;
	j40__code_spec_t codespec = {0};
	j40__code_t code = { .spec = &codespec };
	int32_t byte = 0, prev = 0, pprev = 0, ctx;

	enc_size = j40__u64(st);
	J40__TRY(j40__code_spec(st, 41, &codespec));

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
		byte = j40__code(st, ctx, 0, &code);
		//printf("%zd/%zd: %zd ctx=%d byte=%#x %c\n", index, enc_size, j40__bits_read(st), ctx, (int)byte, 0x20 <= byte && byte < 0x7f ? byte : ' '); fflush(stdout);
		J40__RAISE_DELAYED();
		// TODO actually interpret them
	}
	J40__TRY(j40__finish_and_free_code(st, &code));
	j40__free_code_spec(&codespec);

	//size_t output_size = j40__varint(st);
	//size_t commands_size = j40__varint(st);

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

J40__ON_ERROR:
	j40__free_code(&code);
	j40__free_code_spec(&codespec);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// MA tree

enum { J40__NUM_PRED = 14 };

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
} j40__tree_t;

J40_STATIC J40_RETURNS_ERR j40__tree(j40__st *st, j40__tree_t **tree, j40__code_spec_t *codespec);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__tree(j40__st *st, j40__tree_t **tree, j40__code_spec_t *codespec) {
	j40__code_t code = { .spec = codespec };
	j40__tree_t *t = NULL;
	int32_t tree_idx = 0, tree_cap = 8;
	int32_t ctx_id = 0, nodes_left = 1;

	J40__TRY(j40__code_spec(st, 6, codespec));
	J40__SHOULD(t = malloc(sizeof(j40__tree_t) * (size_t) tree_cap), "!mem");
	while (nodes_left-- > 0) { // depth-first, left-to-right ordering
		j40__tree_t *n;
		int32_t prop = j40__code(st, 1, 0, &code), val, shift;
		J40__TRY_REALLOC(&t, sizeof(j40__tree_t), tree_idx + 1, &tree_cap);
		n = &t[tree_idx++];
		if (prop > 0) {
			n->branch.prop = -prop;
			n->branch.value = j40__unpack_signed(j40__code(st, 0, 0, &code));
			n->branch.leftoff = ++nodes_left;
			n->branch.rightoff = ++nodes_left;
		} else {
			n->leaf.ctx = ctx_id++;
			n->leaf.predictor = j40__code(st, 2, 0, &code);
			n->leaf.offset = j40__unpack_signed(j40__code(st, 3, 0, &code));
			shift = j40__code(st, 4, 0, &code);
			J40__SHOULD(shift < 31, "tree");
			val = j40__code(st, 5, 0, &code);
			J40__SHOULD(val < (1 << (31 - shift)) - 1, "tree");
			n->leaf.multiplier = (val + 1) << shift;
		}
		J40__SHOULD(tree_idx + nodes_left <= (1 << 26), "tree");
	}
	J40__TRY(j40__finish_and_free_code(st, &code));
	j40__free_code_spec(codespec);
	memset(codespec, 0, sizeof(*codespec)); // XXX is it required?
	J40__TRY(j40__code_spec(st, ctx_id, codespec));
	*tree = t;
	return 0;

J40__ON_ERROR:
	free(t);
	j40__free_code(&code);
	j40__free_code_spec(codespec);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// modular header

typedef union {
	enum j40__transform_id {
		J40__TR_RCT = 0, J40__TR_PALETTE = 1, J40__TR_SQUEEZE = 2
	} tr;
	struct {
		enum j40__transform_id tr; // = J40__TR_RCT
		int32_t begin_c, type;
	} rct;
	struct {
		enum j40__transform_id tr; // = J40__TR_PALETTE
		int32_t begin_c, num_c, nb_colours, nb_deltas, d_pred;
	} pal;
	// this is nested in the bitstream, but flattened here.
	// nb_transforms get updated accordingly, but should be enough (the maximum is 80808)
	struct {
		enum j40__transform_id tr; // = J40__TR_SQUEEZE
		int implicit; // if true, no explicit parameters given in the bitstream
		int horizontal, in_place;
		int32_t begin_c, num_c;
	} sq;
} j40__transform_t;

typedef struct { int8_t p1, p2, p3[5], w[4]; } j40__wp_params;

typedef struct {
	int use_global_tree;
	j40__wp_params wp;
	int32_t nb_transforms;
	j40__transform_t *transform;
	j40__tree_t *tree; // owned only if use_global_tree is false
	j40__code_spec_t codespec;
	j40__code_t code;
	int32_t num_channels, nb_meta_channels;
	j40__plane *channel;
	int32_t max_width; // aka dist_multiplier, excludes meta channels
} j40__modular_t;

J40_STATIC void j40__init_modular_common(j40__modular_t *m);
J40_STATIC J40_RETURNS_ERR j40__init_modular(
	j40__st *st, int32_t num_channels, const int32_t *w, const int32_t *h, j40__modular_t *m
);
J40_STATIC J40_RETURNS_ERR j40__init_modular_for_global(
	j40__st *st, int frame_is_modular, int frame_do_ycbcr,
	int32_t frame_log_upsampling, const int32_t *frame_ec_log_upsampling,
	int32_t frame_width, int32_t frame_height, j40__modular_t *m
);
J40_STATIC J40_RETURNS_ERR j40__init_modular_for_pass_group(
	j40__st *st, int32_t num_gm_channels, int32_t gw, int32_t gh,
	int32_t minshift, int32_t maxshift, const j40__modular_t *gm, j40__modular_t *m
);
J40_STATIC void j40__combine_modular_from_pass_group(
	j40__st *st, int32_t num_gm_channels, int32_t gy, int32_t gx,
	int32_t minshift, int32_t maxshift, const j40__modular_t *gm, j40__modular_t *m
);
J40_STATIC J40_RETURNS_ERR j40__modular_header(
	j40__st *st, j40__tree_t *global_tree, const j40__code_spec_t *global_codespec,
	j40__modular_t *m
);
J40_STATIC J40_RETURNS_ERR j40__allocate_modular(j40__st *st, j40__modular_t *m);
J40_STATIC void j40__free_modular(j40__modular_t *m);

#ifdef J40_IMPLEMENTATION

J40_STATIC void j40__init_modular_common(j40__modular_t *m) {
	m->transform = NULL;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(j40__code_spec_t));
	memset(&m->code, 0, sizeof(j40__code_t));
	m->code.spec = &m->codespec;
	m->channel = NULL;
}

J40_STATIC J40_RETURNS_ERR j40__init_modular(
	j40__st *st, int32_t num_channels, const int32_t *w, const int32_t *h, j40__modular_t *m
) {
	int32_t i;

	j40__init_modular_common(m);
	m->num_channels = num_channels;
	J40__ASSERT(num_channels > 0);
	J40__SHOULD(m->channel = calloc((size_t) num_channels, sizeof(j40__plane)), "!mem");
	for (i = 0; i < num_channels; ++i) {
		m->channel[i].width = w[i];
		m->channel[i].height = h[i];
		m->channel[i].hshift = m->channel[i].vshift = 0;
	}
J40__ON_ERROR:
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__init_modular_for_global(
	j40__st *st, int frame_is_modular, int frame_do_ycbcr,
	int32_t frame_log_upsampling, const int32_t *frame_ec_log_upsampling,
	int32_t frame_width, int32_t frame_height, j40__modular_t *m
) {
	j40__image_st *im = st->image;
	int32_t i;

	j40__init_modular_common(m);
	m->num_channels = im->num_extra_channels;
	if (frame_is_modular) { // SPEC the condition is negated
		m->num_channels += (!frame_do_ycbcr && !im->xyb_encoded && im->cspace == J40__CS_GREY ? 1 : 3);
	}
	if (m->num_channels == 0) return 0;

	J40__SHOULD(m->channel = calloc((size_t) m->num_channels, sizeof(j40__plane)), "!mem");
	for (i = 0; i < im->num_extra_channels; ++i) {
		int32_t log_upsampling = (frame_ec_log_upsampling ? frame_ec_log_upsampling[i] : 0) + im->ec_info[i].dim_shift;
		J40__SHOULD(log_upsampling >= frame_log_upsampling, "usmp");
		J40__SHOULD(log_upsampling == 0, "TODO: upsampling is not yet supported");
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

J40__ON_ERROR:
	free(m->channel);
	m->channel = NULL;
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__init_modular_for_pass_group(
	j40__st *st, int32_t num_gm_channels, int32_t gw, int32_t gh,
	int32_t minshift, int32_t maxshift, const j40__modular_t *gm, j40__modular_t *m
) {
	int32_t i, max_channels;

	j40__init_modular_common(m);
	m->num_channels = 0;
	max_channels = gm->num_channels - num_gm_channels;
	J40__ASSERT(max_channels >= 0);
	J40__SHOULD(m->channel = calloc((size_t) max_channels, sizeof(j40__plane)), "!mem");
	for (i = num_gm_channels; i < gm->num_channels; ++i) {
		j40__plane *gc = &gm->channel[i], *c = &m->channel[m->num_channels];
		if (gc->hshift < 3 || gc->vshift < 3) {
			J40__ASSERT(gc->hshift >= 0 && gc->vshift >= 0);
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
J40__ON_ERROR:
	return st->err;
}

J40_STATIC void j40__combine_modular_from_pass_group(
	j40__st *st, int32_t num_gm_channels, int32_t gy, int32_t gx,
	int32_t minshift, int32_t maxshift, const j40__modular_t *gm, j40__modular_t *m
) {
	size_t pixel_size = st->image->modular_16bit_buffers ? sizeof(int16_t) : sizeof(int32_t);
	int32_t gcidx, cidx, y, gx0, gy0;
	for (gcidx = num_gm_channels, cidx = 0; gcidx < gm->num_channels; ++gcidx) {
		j40__plane *gc = &gm->channel[gcidx], *c = &m->channel[cidx];
		if (gc->hshift < 3 || gc->vshift < 3) {
			(void) minshift; (void) maxshift;
			// TODO check minshift/maxshift!!!
			J40__ASSERT(gc->hshift == c->hshift && gc->vshift == c->vshift);
			gx0 = gx >> gc->hshift;
			gy0 = gy >> gc->vshift;
			J40__ASSERT(gx0 + c->width <= gc->width && gy0 + c->height <= gc->height);
			for (y = 0; y < c->height; ++y) {
				memcpy(
					(char*) gc->pixels + pixel_size * (size_t) ((gy0 + y) * gc->width + gx0),
					(char*) c->pixels + pixel_size * (size_t) (y * c->width),
					pixel_size * (size_t) c->width);
			}
			printf("combined channel %d with w=%d h=%d to channel %d with w=%d h=%d gx0=%d gy0=%d\n", cidx, c->width, c->height, gcidx, gc->width, gc->height, gx0, gy0); fflush(stdout);
			++cidx;
		}
	}
	J40__ASSERT(cidx == m->num_channels);
}

J40_STATIC J40_RETURNS_ERR j40__modular_header(
	j40__st *st, j40__tree_t *global_tree, const j40__code_spec_t *global_codespec,
	j40__modular_t *m
) {
	j40__plane *channel = m->channel;
	int32_t num_channels = m->num_channels, nb_meta_channels = 0;
	// note: channel_cap is the upper bound of # channels during inverse transform, and since
	// we don't shrink the channel list we don't ever need reallocation in j40__inverse_transform!
	int32_t channel_cap = m->num_channels, transform_cap;
	int32_t i, j;

	J40__ASSERT(num_channels > 0);

	m->use_global_tree = j40__u(st, 1);
	J40__SHOULD(!m->use_global_tree || global_tree, "mtre");

	{ // WPHeader
		int default_wp = j40__u(st, 1);
		m->wp.p1 = default_wp ? 16 : (int8_t) j40__u(st, 5);
		m->wp.p2 = default_wp ? 10 : (int8_t) j40__u(st, 5);
		for (i = 0; i < 5; ++i) m->wp.p3[i] = default_wp ? 7 * (i < 3) : (int8_t) j40__u(st, 5);
		for (i = 0; i < 4; ++i) m->wp.w[i] = default_wp ? 12 + (i < 1) : (int8_t) j40__u(st, 4);
	}

	transform_cap = m->nb_transforms = j40__u32(st, 0, 0, 1, 0, 2, 4, 18, 8);
	J40__SHOULD(m->transform = malloc(sizeof(j40__transform_t) * (size_t) transform_cap), "!mem");
	for (i = 0; i < m->nb_transforms; ++i) {
		j40__transform_t *tr = &m->transform[i];
		int32_t num_sq;

		tr->tr = (enum j40__transform_id) j40__u(st, 2);
		switch (tr->tr) {
		// RCT: [begin_c, begin_c+3) -> [begin_c, begin_c+3)
		case J40__TR_RCT: {
			int32_t begin_c = tr->rct.begin_c = j40__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
			int32_t type = tr->rct.type = j40__u32(st, 6, 0, 0, 2, 2, 4, 10, 6);
			J40__SHOULD(type < 42, "rctt");
			J40__SHOULD(begin_c + 3 <= num_channels, "rctc");
			J40__SHOULD(begin_c >= nb_meta_channels || begin_c + 3 <= nb_meta_channels, "rctc");
			J40__SHOULD(j40__plane_all_equal_sized(channel + begin_c, channel + begin_c + 3), "rtcd");
			printf("transform %d: rct type %d [%d,%d)\n", i, type, begin_c, begin_c + 3); fflush(stdout);
			break;
		}

		// Palette: [begin_c, end_c) -> palette 0 (meta, nb_colours by num_c) + index begin_c+1
		case J40__TR_PALETTE: {
			j40__plane input;
			int32_t begin_c = tr->pal.begin_c = j40__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
			int32_t num_c = tr->pal.num_c = j40__u32(st, 1, 0, 3, 0, 4, 0, 1, 13);
			int32_t end_c = begin_c + num_c;
			int32_t nb_colours = tr->pal.nb_colours = j40__u32(st, 0, 8, 256, 10, 1280, 12, 5376, 16);
			tr->pal.nb_deltas = j40__u32(st, 0, 0, 1, 8, 257, 10, 1281, 16);
			tr->pal.d_pred = j40__u(st, 4);
			J40__SHOULD(tr->pal.d_pred < J40__NUM_PRED, "palp");
			J40__SHOULD(end_c <= num_channels, "palc");
			if (begin_c < nb_meta_channels) { // num_c meta channels -> 2 meta channels (palette + index)
				J40__SHOULD(end_c <= nb_meta_channels, "palc");
				nb_meta_channels += 2 - num_c;
			} else { // num_c color channels -> 1 meta channel (palette) + 1 color channel (index)
				nb_meta_channels += 1;
			}
			J40__SHOULD(j40__plane_all_equal_sized(channel + begin_c, channel + end_c), "pald");
			// inverse palette transform always requires one more channel slot
			J40__TRY_REALLOC(&channel, sizeof(*channel), num_channels + 1, &channel_cap);
			input = channel[begin_c];
			memmove(channel + 1, channel, sizeof(*channel) * (size_t) begin_c);
			memmove(channel + begin_c + 2, channel + end_c, sizeof(*channel) * (size_t) (num_channels - end_c));
			channel[0].width = nb_colours;
			channel[0].height = num_c;
			channel[0].hshift = 0; // SPEC missing
			channel[0].vshift = -1;
			channel[begin_c + 1] = input;
			num_channels += 2 - num_c;
			printf("transform %d: palette [%d,%d) c%d d%d p%d\n", i, begin_c, end_c, nb_colours, tr->pal.nb_deltas, tr->pal.d_pred); fflush(stdout);
			break;
		}

		// Squeeze: 
		case J40__TR_SQUEEZE: {
			num_sq = j40__u32(st, 0, 0, 1, 4, 9, 6, 41, 8);
			if (num_sq == 0) {
				tr->sq.implicit = 1;
			} else {
				J40__TRY_REALLOC(&m->transform, sizeof(j40__transform_t),
					m->nb_transforms + num_sq - 1, &transform_cap);
				for (j = 0; j < num_sq; ++j) {
					tr = &m->transform[i + j];
					tr->sq.tr = J40__TR_SQUEEZE;
					tr->sq.implicit = 0;
					tr->sq.horizontal = j40__u(st, 1);
					tr->sq.in_place = j40__u(st, 1);
					tr->sq.begin_c = j40__u32(st, 0, 3, 8, 6, 72, 10, 1096, 13);
					tr->sq.num_c = j40__u32(st, 1, 0, 2, 0, 3, 0, 4, 4);
				}
				i += num_sq - 1;
				m->nb_transforms += num_sq - 1;
			}
			J40__RAISE("TODO: squeeze channel effects");
			break;
		}

		default: J40__RAISE("xfm?");
		}
		J40__RAISE_DELAYED();
	}

	if (m->use_global_tree) {
		m->tree = global_tree;
		memcpy(&m->codespec, global_codespec, sizeof(j40__code_spec_t));
	} else {
		J40__TRY(j40__tree(st, &m->tree, &m->codespec));
	}

	m->channel = channel;
	m->num_channels = num_channels;
	m->nb_meta_channels = nb_meta_channels;
	m->max_width = 0;
	for (i = nb_meta_channels; i < num_channels; ++i) {
		m->max_width = j40__max32(m->max_width, channel[i].width);
	}
	return 0;

J40__ON_ERROR:
	free(channel);
	free(m->transform);
	if (!m->use_global_tree) {
		free(m->tree);
		j40__free_code_spec(&m->codespec);
	}
	m->num_channels = 0;
	m->channel = NULL;
	m->transform = NULL;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(j40__code_spec_t));
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__allocate_modular(j40__st *st, j40__modular_t *m) {
	uint8_t pixel_type = st->image->modular_16bit_buffers ? J40__PLANE_I16 : J40__PLANE_I32;
	int32_t i;
	for (i = 0; i < m->num_channels; ++i) {
		j40__plane *c = &m->channel[i];
		J40__TRY(j40__init_plane(st, pixel_type, c->width, c->height, c));
	}
J40__ON_ERROR:
	return st->err;
}

J40_STATIC void j40__free_modular(j40__modular_t *m) {
	int32_t i;
	j40__free_code(&m->code);
	if (!m->use_global_tree) {
		free(m->tree);
		j40__free_code_spec(&m->codespec);
	}
	for (i = 0; i < m->num_channels; ++i) j40__free_plane(&m->channel[i]);
	free(m->transform);
	free(m->channel);
	m->use_global_tree = 0;
	m->tree = NULL;
	memset(&m->codespec, 0, sizeof(j40__code_spec_t));
	m->transform = NULL;
	m->num_channels = 0;
	m->channel = NULL;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// modular prediction

J40_STATIC J40_RETURNS_ERR j40__modular_channel(j40__st *st, j40__modular_t *m, int32_t cidx, int32_t sidx);

#ifdef J40_IMPLEMENTATION
static const int32_t J40__24DIVP1[64] = { // [i] = floor(2^24 / (i+1))
	0x1000000, 0x800000, 0x555555, 0x400000, 0x333333, 0x2aaaaa, 0x249249, 0x200000,
	0x1c71c7, 0x199999, 0x1745d1, 0x155555, 0x13b13b, 0x124924, 0x111111, 0x100000,
	0xf0f0f, 0xe38e3, 0xd7943, 0xccccc, 0xc30c3, 0xba2e8, 0xb2164, 0xaaaaa,
	0xa3d70, 0x9d89d, 0x97b42, 0x92492, 0x8d3dc, 0x88888, 0x84210, 0x80000,
	0x7c1f0, 0x78787, 0x75075, 0x71c71, 0x6eb3e, 0x6bca1, 0x69069, 0x66666,
	0x63e70, 0x61861, 0x5f417, 0x5d174, 0x5b05b, 0x590b2, 0x57262, 0x55555,
	0x53978, 0x51eb8, 0x50505, 0x4ec4e, 0x4d487, 0x4bda1, 0x4a790, 0x49249,
	0x47dc1, 0x469ee, 0x456c7, 0x44444, 0x4325c, 0x42108, 0x41041, 0x40000,
};
#endif

// ----------------------------------------
// recursion for modular buffer sizes (16/32)
#undef J40__RECURSING
#define J40__RECURSING 200
#define J40__P 16
#define J40__2P 32
#include J40_FILENAME
#define J40__P 32
#define J40__2P 64
#include J40_FILENAME
#undef J40__RECURSING
#define J40__RECURSING (-1)

#endif // J40__RECURSING < 0
#if J40__RECURSING == 200
	#define j40__intP_t J40__CONCAT3(int, J40__P, _t)
	#define j40__int2P_t J40__CONCAT3(int, J40__2P, _t)
	#define j40__uint2P_t J40__CONCAT3(uint, J40__2P, _t)
	#define J40__PIXELS J40__CONCAT3(J40__I, J40__P, _PIXELS)
// ----------------------------------------

typedef struct {
	int32_t width;
	j40__wp_params params;
	j40__int2P_t (*errors)[5], pred[5]; // [0..3] = sub-predictions, [4] = final prediction
	j40__int2P_t trueerrw, trueerrn, trueerrnw, trueerrne;
} j40__(wp,2P);

typedef struct { j40__intP_t w, n, nw, ne, nn, nee, ww, nww; } j40__(neighbors_t,P);
J40_ALWAYS_INLINE j40__(neighbors_t,P) j40__(neighbors,P)(const j40__plane *plane, int32_t x, int32_t y);

J40_INLINE j40__int2P_t j40__(gradient,2P)(j40__int2P_t w, j40__int2P_t n, j40__int2P_t nw);
J40_STATIC J40_RETURNS_ERR j40__(init_wp,2P)(j40__st *st, j40__wp_params params, int32_t width, j40__(wp,2P) *wp);
J40_STATIC void j40__(wp_before_predict_internal,2P)(
	j40__(wp,2P) *wp, int32_t x, int32_t y,
	j40__intP_t pw, j40__intP_t pn, j40__intP_t pnw, j40__intP_t pne, j40__intP_t pnn
);
J40_INLINE void j40__(wp_before_predict,2P)(j40__(wp,2P) *wp, int32_t x, int32_t y, j40__(neighbors_t,P) *p);
J40_INLINE j40__int2P_t j40__(predict,2P)(
	j40__st *st, int32_t pred, const j40__(wp,2P) *wp, const j40__(neighbors_t,P) *p
);
J40_INLINE void j40__(wp_after_predict,2P)(j40__(wp,2P) *wp, int32_t x, int32_t y, j40__int2P_t val);
J40_STATIC void j40__(reset_wp,2P)(j40__(wp,2P) *wp);
J40_STATIC void j40__(free_wp,2P)(j40__(wp,2P) *wp);
J40_STATIC J40_RETURNS_ERR j40__(modular_channel,P)(j40__st *st, j40__modular_t *m, int32_t cidx, int32_t sidx);

#ifdef J40_IMPLEMENTATION

J40_ALWAYS_INLINE j40__(neighbors_t,P) j40__(neighbors,P)(const j40__plane *plane, int32_t x, int32_t y) {
	j40__(neighbors_t,P) p;
	const j40__intP_t *pixels = J40__PIXELS(plane, y);
	int32_t width = plane->width, stride = J40__PLANE_STRIDE(plane);

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
	p.w = x > 0 ? pixels[x - 1] : y > 0 ? pixels[x - stride] : 0;
	p.n = y > 0 ? pixels[x - stride] : p.w;
	p.nw = x > 0 && y > 0 ? pixels[(x - 1) - stride] : p.w;
	p.ne = x + 1 < width && y > 0 ? pixels[(x + 1) - stride] : p.n;
	p.nn = y > 1 ? pixels[x - 2 * stride] : p.n;
	p.nee = x + 2 < width && y > 0 ? pixels[(x + 2) - stride] : p.ne;
	p.ww = x > 1 ? pixels[x - 2] : p.w;
	p.nww = x > 1 && y > 0 ? pixels[(x - 2) - stride] : p.ww;
	return p;
}

J40_INLINE j40__int2P_t j40__(gradient,2P)(j40__int2P_t w, j40__int2P_t n, j40__int2P_t nw) {
	j40__int2P_t lo = j40__(min,2P)(w, n), hi = j40__(max,2P)(w, n);
	return j40__(min,2P)(j40__(max,2P)(lo, w + n - nw), hi);
}

J40_STATIC J40_RETURNS_ERR j40__(init_wp,2P)(j40__st *st, j40__wp_params params, int32_t width, j40__(wp,2P) *wp) {
	int32_t i;
	J40__ASSERT(width > 0);
	wp->width = width;
	wp->params = params;
	J40__SHOULD(wp->errors = calloc((size_t) width * 2, sizeof(j40__int2P_t[5])), "!mem");
	for (i = 0; i < 5; ++i) wp->pred[i] = 0;
	wp->trueerrw = wp->trueerrn = wp->trueerrnw = wp->trueerrne = 0;
J40__ON_ERROR:
	return st->err;
}

// also works when wp is zero-initialized (in which case does nothing)
J40_STATIC void j40__(wp_before_predict_internal,2P)(
	j40__(wp,2P) *wp, int32_t x, int32_t y,
	j40__intP_t pw, j40__intP_t pn, j40__intP_t pnw, j40__intP_t pne, j40__intP_t pnn
) {
	typedef j40__int2P_t int2P_t;
	typedef j40__uint2P_t uint2P_t;

	static const int2P_t ZERO[4] = {0, 0, 0, 0};

	int2P_t (*err)[5], (*nerr)[5];
	int2P_t w[4], wsum, sum;
	int32_t logw, i;
	const int2P_t *errw, *errn, *errnw, *errne, *errww, *errw2;

	if (!wp->errors) return;

	err = wp->errors + (y & 1 ? wp->width : 0);
	nerr = wp->errors + (y & 1 ? 0 : wp->width);

	// SPEC edge cases are handled differently from the spec, in particular some pixels are
	// added twice to err_sum and requires a special care (errw2 below)
	errw = x > 0 ? err[x - 1] : ZERO;
	errn = y > 0 ? nerr[x] : ZERO;
	errnw = x > 0 && y > 0 ? nerr[x - 1] : errn;
	errne = x + 1 < wp->width && y > 0 ? nerr[x + 1] : errn;
	errww = x > 1 ? err[x - 2] : ZERO;
	errw2 = x + 1 < wp->width ? ZERO : errw;

	// SPEC again, edge cases are handled differently
	wp->trueerrw = x > 0 ? err[x - 1][4] : 0;
	wp->trueerrn = y > 0 ? nerr[x][4] : 0;
	wp->trueerrnw = x > 0 && y > 0 ? nerr[x - 1][4] : wp->trueerrn;
	wp->trueerrne = x + 1 < wp->width && y > 0 ? nerr[x + 1][4] : wp->trueerrn;

	wp->pred[0] = (pw + pne - pn) << 3;
	wp->pred[1] = (pn << 3) - (((wp->trueerrw + wp->trueerrn + wp->trueerrne) * wp->params.p1) >> 5);
	wp->pred[2] = (pw << 3) - (((wp->trueerrw + wp->trueerrn + wp->trueerrnw) * wp->params.p2) >> 5);
	wp->pred[3] = (pn << 3) - // SPEC negated (was `+`)
		((wp->trueerrnw * wp->params.p3[0] + wp->trueerrn * wp->params.p3[1] +
		  wp->trueerrne * wp->params.p3[2] + ((pnn - pn) << 3) * wp->params.p3[3] +
		  ((pnw - pw) << 3) * wp->params.p3[4]) >> 5);
	for (i = 0; i < 4; ++i) {
		int2P_t errsum = errn[i] + errw[i] + errnw[i] + errww[i] + errne[i] + errw2[i];
		int32_t shift = j40__max32(j40__(floor_lg,2P)((uint2P_t) errsum + 1) - 5, 0);
		// SPEC missing the final `>> shift`
		w[i] = (int2P_t) (4 + ((int64_t) wp->params.w[i] * J40__24DIVP1[errsum >> shift] >> shift));
	}
	logw = j40__(floor_lg,2P)((uint2P_t) (w[0] + w[1] + w[2] + w[3])) - 4;
	wsum = sum = 0;
	for (i = 0; i < 4; ++i) {
		wsum += w[i] >>= logw;
		sum += wp->pred[i] * w[i];
	}
	// SPEC missing `- 1` before scaling
	wp->pred[4] = (int2P_t) (((int64_t) sum + (wsum >> 1) - 1) * J40__24DIVP1[wsum - 1] >> 24);
	if (((wp->trueerrn ^ wp->trueerrw) | (wp->trueerrn ^ wp->trueerrnw)) <= 0) {
		int2P_t lo = j40__(min,2P)(pw, j40__(min,2P)(pn, pne)) << 3; // SPEC missing shifts
		int2P_t hi = j40__(max,2P)(pw, j40__(max,2P)(pn, pne)) << 3;
		wp->pred[4] = j40__(min,2P)(j40__(max,2P)(lo, wp->pred[4]), hi);
	}
}

J40_INLINE void j40__(wp_before_predict,2P)(
	j40__(wp,2P) *wp, int32_t x, int32_t y, j40__(neighbors_t,P) *p
) {
	j40__(wp_before_predict_internal,2P)(wp, x, y, p->w, p->n, p->nw, p->ne, p->nn);
}

J40_INLINE j40__int2P_t j40__(predict,2P)(
	j40__st *st, int32_t pred, const j40__(wp,2P) *wp, const j40__(neighbors_t,P) *p
) {
	switch (pred) {
	case 0: return 0;
	case 1: return p->w;
	case 2: return p->n;
	case 3: return (p->w + p->n) / 2;
	case 4: return j40__(abs,2P)(p->n - p->nw) < j40__(abs,2P)(p->w - p->nw) ? p->w : p->n;
	case 5: return j40__(gradient,2P)(p->w, p->n, p->nw);
	case 6: return (wp->pred[4] + 3) >> 3;
	case 7: return p->ne;
	case 8: return p->nw;
	case 9: return p->ww;
	case 10: return (p->w + p->nw) / 2;
	case 11: return (p->n + p->nw) / 2;
	case 12: return (p->n + p->ne) / 2;
	case 13: return (6 * p->n - 2 * p->nn + 7 * p->w + p->ww + p->nee + 3 * p->ne + 8) / 16;
	default: return J40__ERR("pred"), 0;
	}
}

// also works when wp is zero-initialized (in which case does nothing)
J40_INLINE void j40__(wp_after_predict,2P)(j40__(wp,2P) *wp, int32_t x, int32_t y, j40__int2P_t val) {
	if (wp->errors) {
		j40__int2P_t *err = wp->errors[(y & 1 ? wp->width : 0) + x];
		int32_t i;
		// SPEC approximated differently from the spec
		for (i = 0; i < 4; ++i) err[i] = (j40__(abs,2P)(wp->pred[i] - (val << 3)) + 3) >> 3;
		err[4] = wp->pred[4] - (val << 3); // SPEC this is a *signed* difference
	}
}

// also works when wp is zero-initialized (in which case does nothing)
J40_STATIC void j40__(reset_wp,2P)(j40__(wp,2P) *wp) {
	int32_t i;
	if (wp->errors) memset(wp->errors, 0, (size_t) wp->width * 2 * sizeof(j40__int2P_t[5]));
	for (i = 0; i < 5; ++i) wp->pred[i] = 0;
	wp->trueerrw = wp->trueerrn = wp->trueerrnw = wp->trueerrne = 0;
}

J40_STATIC void j40__(free_wp,2P)(j40__(wp,2P) *wp) {
	free(wp->errors);
	wp->errors = NULL;
	wp->width = 0;
}

J40_STATIC J40_RETURNS_ERR j40__(modular_channel,P)(
	j40__st *st, j40__modular_t *m, int32_t cidx, int32_t sidx
) {
	typedef j40__intP_t intP_t;
	typedef j40__int2P_t int2P_t;

	j40__plane *c = &m->channel[cidx];
	int32_t width = c->width, height = c->height;
	int32_t y, x, i;
	int32_t nrefcmap, *refcmap = NULL; // refcmap[i] is a channel index for properties (16..19)+4*i
	j40__(wp,2P) wp = {0};

	J40__ASSERT(m->tree); // caller should set this to the global tree if not given

	{ // determine whether to use weighted predictor (expensive)
		int32_t lasttree = 0, use_wp = 0;
		for (i = 0; i <= lasttree && !use_wp; ++i) {
			if (m->tree[i].branch.prop < 0) {
				use_wp |= ~m->tree[i].branch.prop == 15;
				lasttree = j40__max32(lasttree,
					i + j40__max32(m->tree[i].branch.leftoff, m->tree[i].branch.rightoff));
			} else {
				use_wp |= m->tree[i].leaf.predictor == 6;
			}
		}
		if (use_wp) J40__TRY(j40__(init_wp,2P)(st, m->wp, width, &wp));
	}

	// compute indices for additional "previous channel" properties
	// SPEC incompatible channels are skipped and never result in unusable but numbered properties
	J40__SHOULD(refcmap = malloc(sizeof(int32_t) * (size_t) cidx), "!mem");
	nrefcmap = 0;
	for (i = cidx - 1; i >= 0; --i) {
		j40__plane *refc = &m->channel[i];
		if (c->width != refc->width || c->height != refc->height) continue;
		if (c->hshift != refc->hshift || c->vshift != refc->vshift) continue;
		refcmap[nrefcmap++] = i;
	}

	for (y = 0; y < height; ++y) {
		intP_t *outpixels = J40__PIXELS(c, y);
		for (x = 0; x < width; ++x) {
			j40__tree_t *n = m->tree;
			j40__(neighbors_t,P) p = j40__(neighbors,P)(c, x, y);
			int2P_t val;

			// wp should be calculated before any property testing due to max_error (property 15)
			j40__(wp_before_predict,2P)(&wp, x, y, &p);

			while (n->branch.prop < 0) {
				int32_t refcidx;
				j40__plane *refc;

				switch (~n->branch.prop) {
				case 0: val = cidx; break;
				case 1: val = sidx; break;
				case 2: val = y; break;
				case 3: val = x; break;
				case 4: val = j40__(abs,2P)(p.n); break;
				case 5: val = j40__(abs,2P)(p.w); break;
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
					if (j40__(abs,2P)(val) < j40__(abs,2P)(wp.trueerrn)) val = wp.trueerrn;
					if (j40__(abs,2P)(val) < j40__(abs,2P)(wp.trueerrnw)) val = wp.trueerrnw;
					if (j40__(abs,2P)(val) < j40__(abs,2P)(wp.trueerrne)) val = wp.trueerrne;
					break;
				default:
					refcidx = (~n->branch.prop - 16) / 4;
					J40__SHOULD(refcidx < nrefcmap, "trec");
					refc = &m->channel[refcmap[refcidx]];
					J40__ASSERT(c->width == refc->width && c->height == refc->height);
					val = J40__PIXELS(refc, y)[x]; // rC
					if (~n->branch.prop & 2) {
						int2P_t rw = x > 0 ? J40__PIXELS(refc, y)[x - 1] : 0;
						int2P_t rn = y > 0 ? J40__PIXELS(refc, y - 1)[x] : rw;
						int2P_t rnw = x > 0 && y > 0 ? J40__PIXELS(refc, y - 1)[x - 1] : rw;
						val -= j40__(gradient,2P)(rw, rn, rnw);
					}
					if (~n->branch.prop & 1) val = j40__(abs,2P)(val);
					break;
				}
				n += val > n->branch.value ? n->branch.leftoff : n->branch.rightoff;
			}

			val = j40__code(st, n->leaf.ctx, m->max_width, &m->code);
			//printf("%d ", val);
			val = j40__unpack_signed((int32_t) val) * n->leaf.multiplier + n->leaf.offset;
			val += j40__(predict,2P)(st, n->leaf.predictor, &wp, &p);
			J40__SHOULD(INT16_MIN <= val && val <= INT16_MAX, "povf");
			outpixels[x] = (intP_t) val;
			j40__(wp_after_predict,2P)(&wp, x, y, val);
		}
			//printf("\n");
	}
			//printf("--\n"); fflush(stdout);

	j40__(free_wp,2P)(&wp);
	free(refcmap);
	return 0;

J40__ON_ERROR:
	j40__(free_wp,2P)(&wp);
	free(refcmap);
	j40__free_plane(c);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

// ----------------------------------------
// end of recursion
	#undef j40__intP_t
	#undef j40__int2P_t
	#undef j40__uint2P_t
	#undef J40__PIXELS
	#undef J40__P
	#undef J40__2P
#endif // J40__RECURSING == 200
#if J40__RECURSING < 0
// ----------------------------------------

#ifdef J40_IMPLEMENTATION
J40_STATIC J40_RETURNS_ERR j40__modular_channel(j40__st *st, j40__modular_t *m, int32_t cidx, int32_t sidx) {
	if (st->image->modular_16bit_buffers) {
		return j40__modular_channel16(st, m, cidx, sidx);
	} else {
		return j40__modular_channel32(st, m, cidx, sidx);
	}
}
#endif

////////////////////////////////////////////////////////////////////////////////
// modular (inverse) transform

J40_STATIC J40_RETURNS_ERR j40__inverse_transform(j40__st *st, j40__modular_t *m);

#ifdef J40_IMPLEMENTATION
#define J40__X(x,y,z) {x,y,z}, {-(x),-(y),-(z)}
#define J40__XX(a,b,c,d,e,f) J40__X a, J40__X b, J40__X c, J40__X d, J40__X e, J40__X f
static const int16_t J40__PALETTE_DELTAS[144][3] = { // the first entry is a duplicate and skipped
	J40__XX((0, 0, 0), (4, 4, 4), (11, 0, 0), (0, 0, -13), (0, -12, 0), (-10, -10, -10)),
	J40__XX((-18, -18, -18), (-27, -27, -27), (-18, -18, 0), (0, 0, -32), (-32, 0, 0), (-37, -37, -37)),
	J40__XX((0, -32, -32), (24, 24, 45), (50, 50, 50), (-45, -24, -24), (-24, -45, -45), (0, -24, -24)),
	J40__XX((-34, -34, 0), (-24, 0, -24), (-45, -45, -24), (64, 64, 64), (-32, 0, -32), (0, -32, 0)),
	J40__XX((-32, 0, 32), (-24, -45, -24), (45, 24, 45), (24, -24, -45), (-45, -24, 24), (80, 80, 80)),
	J40__XX((64, 0, 0), (0, 0, -64), (0, -64, -64), (-24, -24, 45), (96, 96, 96), (64, 64, 0)),
	J40__XX((45, -24, -24), (34, -34, 0), (112, 112, 112), (24, -45, -45), (45, 45, -24), (0, -32, 32)),
	J40__XX((24, -24, 45), (0, 96, 96), (45, -24, 24), (24, -45, -24), (-24, -45, 24), (0, -64, 0)),
	J40__XX((96, 0, 0), (128, 128, 128), (64, 0, 64), (144, 144, 144), (96, 96, 0), (-36, -36, 36)),
	J40__XX((45, -24, -45), (45, -45, -24), (0, 0, -96), (0, 128, 128), (0, 96, 0), (45, 24, -45)),
	J40__XX((-128, 0, 0), (24, -45, 24), (-45, 24, -45), (64, 0, -64), (64, -64, -64), (96, 0, 96)),
	J40__XX((45, -45, 24), (24, 45, -45), (64, 64, -64), (128, 128, 0), (0, 0, -128), (-24, 45, -45)),
};
#undef J40__X
#undef J40__XX
#endif // defined J40_IMPLEMENTATION

// ----------------------------------------
// recursion for modular inverse transform
#undef J40__RECURSING
#define J40__RECURSING 300
#define J40__P 16
#define J40__2P 32
#include J40_FILENAME
#define J40__P 32
#define J40__2P 64
#include J40_FILENAME
#undef J40__RECURSING
#define J40__RECURSING (-1)

#endif // J40__RECURSING < 0
#if J40__RECURSING == 300
	#define j40__intP_t J40__CONCAT3(int, J40__P, _t)
	#define j40__int2P_t J40__CONCAT3(int, J40__2P, _t)
	#define J40__PIXELS J40__CONCAT3(J40__I, J40__P, _PIXELS)
// ----------------------------------------

J40_STATIC void j40__(inverse_rct,P)(j40__modular_t *m, const j40__transform_t *tr);
J40_STATIC J40_RETURNS_ERR j40__(inverse_palette,P)(j40__st *st, j40__modular_t *m, const j40__transform_t *tr);

#ifdef J40_IMPLEMENTATION

J40_STATIC void j40__(inverse_rct,P)(j40__modular_t *m, const j40__transform_t *tr) {
	typedef j40__intP_t intP_t;
	typedef j40__int2P_t int2P_t;

	// SPEC permutation psuedocode is missing parentheses; better done with a LUT anyway
	static const uint8_t PERMUTATIONS[6][3] = {{0,1,2},{1,2,0},{2,0,1},{0,2,1},{1,0,2},{2,1,0}};

	j40__plane c[3];
	int32_t x, y, i;

	J40__ASSERT(tr->tr == J40__TR_RCT);
	for (i = 0; i < 3; ++i) c[i] = m->channel[tr->rct.begin_c + i];
	J40__ASSERT(j40__plane_all_equal_sized(c, c + 3));

	// TODO detect overflow
	switch (tr->rct.type % 7) {
	case 0: break;
	case 1:
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) pp2[x] = (intP_t) (pp2[x] + pp0[x]);
		}
		break;
	case 2:
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp1 = J40__PIXELS(&c[1], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) pp2[x] = (intP_t) (pp1[x] + pp0[x]);
		}
		break;
	case 3:
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp1 = J40__PIXELS(&c[1], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) {
				pp1[x] = (intP_t) (pp1[x] + pp0[x]);
				pp2[x] = (intP_t) (pp2[x] + pp0[x]);
			}
		}
		break;
	case 4:
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp1 = J40__PIXELS(&c[1], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) pp1[x] = (intP_t) (pp1[x] + j40__(floor_avg,P)(pp0[x], pp2[x]));
		}
		break;
	case 5:
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp1 = J40__PIXELS(&c[1], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) {
				// TODO avoid int2P_t if possible
				pp1[x] = (intP_t) ((int2P_t) pp1[x] + pp0[x] + (pp2[x] >> 1));
				pp2[x] = (intP_t) (pp2[x] + pp0[x]);
			}
		}
		break;
	case 6: // YCgCo
		for (y = 0; y < c->height; ++y) {
			intP_t *pp0 = J40__PIXELS(&c[0], y), *pp1 = J40__PIXELS(&c[1], y), *pp2 = J40__PIXELS(&c[2], y);
			for (x = 0; x < c->width; ++x) {
				// TODO avoid int2P_t if possible
				int2P_t tmp = (int2P_t) pp0[x] - ((int2P_t) pp2[x] >> 1);
				int2P_t p1 = (int2P_t) pp2[x] + tmp;
				int2P_t p2 = tmp - ((int2P_t) pp1[x] >> 1);
				pp0[x] = (intP_t) (p2 + pp1[x]);
				pp1[x] = (intP_t) p1;
				pp2[x] = (intP_t) p2;
			}
		}
		break;
	default: J40__UNREACHABLE();
	}

	for (i = 0; i < 3; ++i) {
		m->channel[tr->rct.begin_c + PERMUTATIONS[tr->rct.type / 7][i]] = c[i];
	}
}

J40_STATIC J40_RETURNS_ERR j40__(inverse_palette,P)(
	j40__st *st, j40__modular_t *m, const j40__transform_t *tr
) {
	typedef j40__intP_t intP_t;
	typedef j40__int2P_t int2P_t;

	// `first` is the index channel index; restored color channels will be at indices [first,last],
	// where the original index channel is relocated to the index `last` and then repurposed.
	// the palette meta channel 0 will be removed at the very end.
	int32_t first = tr->pal.begin_c + 1, last = tr->pal.begin_c + tr->pal.num_c, bpp = st->image->bpp;
	int32_t i, j, y, x;
	j40__plane *idxc = &m->channel[last];
	int32_t width = m->channel[first].width, height = m->channel[first].height;
	int use_pred = tr->pal.nb_deltas > 0, use_wp = use_pred && tr->pal.d_pred == 6;
	j40__(wp,2P) wp = {0};

	J40__ASSERT(tr->tr == J40__TR_PALETTE);

	// since we never shrink m->channel, we know there is enough capacity for intermediate transform
	memmove(m->channel + last, m->channel + first, sizeof(j40__plane) * (size_t) (m->num_channels - first));
	m->num_channels += last - first;

	for (i = first; i < last; ++i) m->channel[i].type = 0;
	for (i = first; i < last; ++i) {
		J40__TRY(j40__init_plane(st, J40__(PLANE_I,P), width, height, &m->channel[i]));
	}

	if (use_wp) J40__TRY(j40__(init_wp,2P)(st, m->wp, width, &wp));

	for (i = 0; i < tr->pal.num_c; ++i) {
		intP_t *palp = J40__PIXELS(&m->channel[0], i);
		j40__plane *c = &m->channel[first + i];
		for (y = 0; y < height; ++y) {
			// SPEC pseudocode accidentally overwrites the index channel
			intP_t *idxline = J40__PIXELS(idxc, y);
			intP_t *line = J40__PIXELS(c, y);
			for (x = 0; x < width; ++x) {
				intP_t idx = idxline[x], val;
				int is_delta = idx < tr->pal.nb_deltas;
				if (idx < 0) { // hard-coded delta for first 3 channels, otherwise 0
					if (i < 3) {
						idx = (intP_t) (~idx % 143); // say no to 1's complement
						val = J40__PALETTE_DELTAS[idx + 1][i];
						if (bpp > 8) val = (intP_t) (val << (j40__min32(bpp, 24) - 8));
					} else {
						val = 0;
					}
				} else if (idx < tr->pal.nb_colours) {
					val = palp[idx];
				} else { // synthesized from (idx - nb_colours)
					idx = (intP_t) (idx - tr->pal.nb_colours);
					if (idx < 64) { // idx == ..YX in base 4 -> {(X+0.5)/4, (Y+0.5)/4, ...}
						val = (intP_t) ((i < 3 ? idx >> (2 * i) : 0) * (((int2P_t) 1 << bpp) - 1) / 4 +
							(1 << j40__max32(0, bpp - 3)));
					} else { // idx + 64 == ..ZYX in base 5 -> {X/4, Y/4, Z/4, ...}
						val = (intP_t) (idx - 64);
						for (j = 0; j < i; ++j) val /= 5;
						val = (intP_t) ((val % 5) * ((1 << bpp) - 1) / 4);
					}
				}
				if (use_pred) {
					j40__(neighbors_t,P) p = j40__(neighbors,P)(c, x, y);
					j40__(wp_before_predict,2P)(&wp, x, y, &p);
					// TODO handle overflow
					if (is_delta) val = (intP_t) (val + j40__(predict,2P)(st, tr->pal.d_pred, &wp, &p));
					j40__(wp_after_predict,2P)(&wp, x, y, val);
				}
				line[x] = val;
			}
		}
		j40__(reset_wp,2P)(&wp);
	}

	j40__(free_wp,2P)(&wp);
	j40__free_plane(&m->channel[0]);
	memmove(m->channel, m->channel + 1, sizeof(j40__plane) * (size_t) --m->num_channels);
	return 0;

J40__ON_ERROR:
	j40__(free_wp,2P)(&wp);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

// ----------------------------------------
// end of recursion
	#undef j40__intP_t
	#undef j40__int2P_t
	#undef J40__PIXELS
	#undef J40__P
	#undef J40__2P
#endif // J40__RECURSING == 300
#if J40__RECURSING < 0
// ----------------------------------------

#ifdef J40_IMPLEMENTATION
J40_STATIC J40_RETURNS_ERR j40__inverse_transform(j40__st *st, j40__modular_t *m) {
	int32_t i;

	if (st->image->modular_16bit_buffers) {
		for (i = m->nb_transforms - 1; i >= 0; --i) {
			const j40__transform_t *tr = &m->transform[i];
			switch (tr->tr) {
			case J40__TR_RCT: j40__inverse_rct16(m, tr); break;
			case J40__TR_PALETTE: J40__TRY(j40__inverse_palette16(st, m, tr)); break;
			case J40__TR_SQUEEZE: J40__RAISE("TODO: squeeze inverse transformation"); break;
			default: J40__UNREACHABLE();
			}
		}
	} else {
		for (i = m->nb_transforms - 1; i >= 0; --i) {
			const j40__transform_t *tr = &m->transform[i];
			switch (tr->tr) {
			case J40__TR_RCT: j40__inverse_rct32(m, tr); break;
			case J40__TR_PALETTE: J40__TRY(j40__inverse_palette32(st, m, tr)); break;
			case J40__TR_SQUEEZE: J40__RAISE("TODO: squeeze inverse transformation"); break;
			default: J40__UNREACHABLE();
			}
		}
	}

J40__ON_ERROR:
	return st->err;
}
#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// dequantization matrix and coefficient orders

enum {
	J40__NUM_DCT_SELECT = 27, // the number of all possible varblock types (DctSelect)
	J40__NUM_DCT_PARAMS = 17, // the number of parameters, some shared by multiple DctSelects
	J40__NUM_ORDERS = 13, // the number of distinct varblock dimensions & orders, after transposition
};

typedef struct {
	enum { // the number of params per channel follows:
		J40__DQ_ENC_LIBRARY = 0, // 0
		J40__DQ_ENC_HORNUSS = 1, // 3 (params)
		J40__DQ_ENC_DCT2 = 2, // 6 (params)
		J40__DQ_ENC_DCT4 = 3, // 2 (params) + n (dct_params)
		// TODO spec issue: DCT4x8 uses an undefined name "parameters" (should be "params")
		J40__DQ_ENC_DCT4X8 = 4, // 1 (params) + n (dct_params)
		J40__DQ_ENC_AFV = 5, // 9 (params) + n (dct_params) + m (dct4x4_params)
		J40__DQ_ENC_DCT = 6, // n (params)
		// all other modes eventually decode to:
		J40__DQ_ENC_RAW = 7, // n rows * m columns, with the top-left 1/8 by 1/8 unused
	} mode;
	int16_t n, m;
	float (*params)[4]; // the last element per each row is unused
} j40__dq_matrix_t;

J40_STATIC J40_RETURNS_ERR j40__dq_matrix(
	j40__st *st, int32_t rows, int32_t columns, int32_t raw_sidx, j40__dq_matrix_t *dqmat
);
J40_INLINE float j40__interpolate(float pos, int32_t c, const float (*bands)[4], int32_t len);
J40_STATIC J40_RETURNS_ERR j40__interpolation_bands(
	j40__st *st, const float (*params)[4], int32_t nparams, float (*out)[4]
);
J40_STATIC void j40__dct_quant_weights(
	int32_t rows, int32_t columns, const float (*bands)[4], int32_t len, float (*out)[4]
);
J40_STATIC J40_RETURNS_ERR j40__load_dq_matrix(j40__st *st, int32_t idx, j40__dq_matrix_t *dqmat);
J40_STATIC J40_RETURNS_ERR j40__natural_order(j40__st *st, int32_t log_rows, int32_t log_columns, int32_t **out);

#ifdef J40_IMPLEMENTATION

typedef struct { int8_t log_rows, log_columns, param_idx, order_idx; } j40__dct_select;
static const j40__dct_select J40__DCT_SELECT[J40__NUM_DCT_SELECT] = {
	// hereafter DCTnm refers to DCT(2^n)x(2^m) in the spec
	/*DCT33*/ {3, 3, 0, 0}, /*Hornuss*/ {3, 3, 1, 1}, /*DCT11*/ {3, 3, 2, 1}, /*DCT22*/ {3, 3, 3, 1},
	/*DCT44*/ {4, 4, 4, 2}, /*DCT55*/ {5, 5, 5, 3}, /*DCT43*/ {4, 3, 6, 4}, /*DCT34*/ {3, 4, 6, 4},
	/*DCT53*/ {5, 3, 7, 5}, /*DCT35*/ {3, 5, 7, 5}, /*DCT54*/ {5, 4, 8, 6}, /*DCT45*/ {4, 5, 8, 6},
	/*DCT23*/ {3, 3, 9, 1}, /*DCT32*/ {3, 3, 9, 1}, /*AFV0*/ {3, 3, 10, 1}, /*AFV1*/ {3, 3, 10, 1},
	/*AFV2*/ {3, 3, 10, 1}, /*AFV3*/ {3, 3, 10, 1}, /*DCT66*/ {6, 6, 11, 7}, /*DCT65*/ {6, 5, 12, 8},
	/*DCT56*/ {5, 6, 12, 8}, /*DCT77*/ {7, 7, 13, 9}, /*DCT76*/ {7, 6, 14, 10}, /*DCT67*/ {6, 7, 14, 10},
	/*DCT88*/ {8, 8, 15, 11}, /*DCT87*/ {8, 7, 16, 12}, /*DCT78*/ {7, 8, 16, 12},
};

static const struct j40__dct_params {
	int8_t log_rows, log_columns, def_offset, def_mode, def_n, def_m;
} J40__DCT_PARAMS[J40__NUM_DCT_PARAMS] = {
	/*DCT33*/ {3, 3, 0, J40__DQ_ENC_DCT, 6, 0}, /*Hornuss*/ {3, 3, 6, J40__DQ_ENC_HORNUSS, 0, 0},
	/*DCT11*/ {3, 3, 9, J40__DQ_ENC_DCT2, 0, 0}, /*DCT22*/ {3, 3, 15, J40__DQ_ENC_DCT4, 4, 0},
	/*DCT44*/ {4, 4, 21, J40__DQ_ENC_DCT, 7, 0}, /*DCT55*/ {5, 5, 28, J40__DQ_ENC_DCT, 8, 0},
	/*DCT34*/ {3, 4, 36, J40__DQ_ENC_DCT, 7, 0}, /*DCT35*/ {3, 5, 43, J40__DQ_ENC_DCT, 8, 0},
	/*DCT45*/ {4, 5, 51, J40__DQ_ENC_DCT, 8, 0}, /*DCT23*/ {3, 3, 59, J40__DQ_ENC_DCT4X8, 4, 0},
	/*AFV*/ {3, 3, 64, J40__DQ_ENC_AFV, 4, 4}, /*DCT66*/ {6, 6, 81, J40__DQ_ENC_DCT, 8, 0},
	/*DCT56*/ {5, 6, 89, J40__DQ_ENC_DCT, 8, 0}, /*DCT77*/ {7, 7, 97, J40__DQ_ENC_DCT, 8, 0},
	/*DCT67*/ {6, 7, 105, J40__DQ_ENC_DCT, 8, 0}, /*DCT88*/ {8, 8, 113, J40__DQ_ENC_DCT, 8, 0},
	/*DCT78*/ {7, 8, 121, J40__DQ_ENC_DCT, 8, 0},
};

#define J40__DCT4X4_DCT_PARAMS \
	{2200.0f, 392.0f, 112.0f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.5f} // (4)
#define J40__DCT4X8_DCT_PARAMS \
	{2198.050556016380522f, 764.3655248643528689f, 527.107573587542228f}, \
	{-0.96269623020744692f, -0.92630200888366945f, -1.4594385811273854f}, \
	{-0.76194253026666783f, -0.9675229603596517f, -1.450082094097871593f}, \
	{-0.6551140670773547f, -0.27845290869168118f, -1.5843722511996204f} // (4)
#define J40__LARGE_DCT_PARAMS(mult) \
	/* it turns out that the first sets of parameters for larger DCTs have the same ratios */ \
	{mult * 23629.073922049845f, mult * 8611.3238710010046f, mult * 4492.2486445538634f}, \
	{-1.025f, -0.3041958212306401f, -1.2f}, {-0.78f, 0.3633036457487539f, -1.2f}, \
	{-0.65012f, -0.35660379990111464f, -0.8f}, {-0.19041574084286472f, -0.3443074455424403f, -0.7f}, \
	{-0.20819395464f, -0.33699592683512467f, -0.7f}, {-0.421064f, -0.30180866526242109f, -0.4f}, \
	{-0.32733845535848671f, -0.27321683125358037f, -0.5f} // (8)
static const float J40__LIBRARY_DCT_PARAMS[129][4] = {
	// DCT33 dct_params (n=6) (SPEC some values are incorrect)
	{3150.0f, 560.0f, 512.0f}, {0.0f, 0.0f, -2.0f}, {-0.4f, -0.3f, -1.0f},
	{-0.4f, -0.3f, 0.0f}, {-0.4f, -0.3f, -1.0f}, {-2.0f, -0.3f, -2.0f},
	// Hornuss params (3)
	{280.0f, 60.0f, 18.0f}, {3160.0f, 864.0f, 200.0f}, {3160.0f, 864.0f, 200.0f},
	// DCT11 params (6)
	{3840.0f, 960.0f, 640.0f}, {2560.0f, 640.0f, 320.0f}, {1280.0f, 320.0f, 128.0f},
	{640.0f, 180.0f, 64.0f}, {480.0f, 140.0f, 32.0f}, {300.0f, 120.0f, 16.0f},
	// DCT22 params (2) + dct_params (n=4) (TODO spec bug: some values are incorrect)
	{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, J40__DCT4X4_DCT_PARAMS,
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
	{1.0f, 1.0f, 1.0f}, J40__DCT4X8_DCT_PARAMS,
	// AFV params (9) + dct_params (n=4) + dct4x4_params (m=4)
	// (SPEC params & dct_params are swapped; TODO spec bug: dct4x4_params are also incorrect)
	{3072.0f, 1024.0f, 384.0f}, {3072.0f, 1024.0f, 384.0f}, {256.0f, 50.0f, 12.0f},
	{256.0f, 50.0f, 12.0f}, {256.0f, 50.0f, 12.0f}, {414.0f, 58.0f, 22.0f},
	{0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f},
	J40__DCT4X8_DCT_PARAMS, J40__DCT4X4_DCT_PARAMS,

	J40__LARGE_DCT_PARAMS(0.9f), // DCT66 dct_params (n=8)
	J40__LARGE_DCT_PARAMS(0.65f), // DCT56 dct_params (n=8)
	J40__LARGE_DCT_PARAMS(1.8f), // DCT77 dct_params (n=8)
	J40__LARGE_DCT_PARAMS(1.3f), // DCT67 dct_params (n=8)
	J40__LARGE_DCT_PARAMS(3.6f), // DCT88 dct_params (n=8)
	J40__LARGE_DCT_PARAMS(2.6f), // DCT78 dct_params (n=8)
};

static const int8_t J40__LOG_ORDER_SIZE[J40__NUM_ORDERS][2] = {
	{3,3}, {3,3}, {4,4}, {5,5}, {3,4}, {3,5}, {4,5}, {6,6}, {5,6}, {7,7}, {6,7}, {8,8}, {7,8},
};

J40_STATIC J40_RETURNS_ERR j40__dq_matrix(
	j40__st *st, int32_t rows, int32_t columns, int32_t raw_sidx, j40__dq_matrix_t *dqmat
) {
	int32_t c, i, j;

	dqmat->mode = j40__u(st, 3);
	dqmat->params = NULL;
	if (dqmat->mode == J40__DQ_ENC_RAW) { // read as a modular image
		float denom = j40__f16(st);
		J40__TRY(j40__zero_pad_to_byte(st));
		(void) raw_sidx;
		J40__RAISE("TODO: RAW dequant matrix");
	} else {
		// interpreted as 0xABCD: A is 1 if 8x8 matrix is required, B is the fixed params size,
		// C indicates that params[0..C-1] are to be scaled, D is the number of calls to ReadDctParams.
		static const int16_t HOW[7] = {0x0000, 0x1330, 0x1660, 0x1221, 0x1101, 0x1962, 0x1001};
		int32_t how = HOW[dqmat->mode];
		int32_t nparams = how >> 8 & 15, nscaled = how >> 4 & 15, ndctparams = how & 15;
		if (how >> 12) J40__SHOULD(rows == 8 && columns == 8, "dqm?");
		J40__SHOULD(dqmat->params = malloc(sizeof(float[3]) * (size_t) (nparams + ndctparams * 16)), "!mem");
		for (c = 0; c < 3; ++c) for (j = 0; j < nparams; ++j) {
			dqmat->params[j][c] = j40__f16(st) * (j < nscaled ? 64.0f : 1.0f);
		}
		for (i = 0; i < ndctparams; ++i) { // ReadDctParams
			int32_t n = *(i == 0 ? &dqmat->n : &dqmat->m) = (int16_t) (j40__u(st, 4) + 1);
			for (c = 0; c < 3; ++c) for (j = 0; j < n; ++j) {
				dqmat->params[nparams + j][c] = j40__f16(st) * (j == 0 ? 64.0f : 1.0f);
			}
			nparams += n;
		}
		J40__RAISE_DELAYED();
	}
	return 0;

J40__ON_ERROR:
	free(dqmat->params);
	dqmat->params = NULL;
	return st->err;
}

// piecewise exponential interpolation where pos is in [0,1], mapping pos = k/(len-1) to bands[k]
J40_INLINE float j40__interpolate(float pos, int32_t c, const float (*bands)[4], int32_t len) {
	float scaled_pos, frac_idx, a, b;
	int32_t scaled_idx;
	if (len == 1) return bands[0][c];
	scaled_pos = pos * (float) (len - 1);
	scaled_idx = (int32_t) scaled_pos;
	frac_idx = scaled_pos - (float) scaled_idx;
	a = bands[scaled_idx][c];
	b = bands[scaled_idx + 1][c];
	return a * powf(b / a, frac_idx);
}

J40_STATIC J40_RETURNS_ERR j40__interpolation_bands(
	j40__st *st, const float (*params)[4], int32_t nparams, float (*out)[4]
) {
	int32_t i, c;
	for (c = 0; c < 3; ++c) {
		// TODO spec bug: loops for x & y are independent of the loop for i (bands)
		// TODO spec bug: `bands(i)` for i >= 0 (not i > 0) should be larger (not no less) than 0
		out[0][c] = params[0][c];
		J40__SHOULD(out[0][c] > 0, "band");
		for (i = 1; i < nparams; ++i) {
			float v = params[i][c];
			out[i][c] = v > 0 ? out[i - 1][c] * (1.0f + v) : out[i - 1][c] / (1.0f - v);
			J40__SHOULD(out[i][c] > 0, "band");
		}
	}
J40__ON_ERROR:
	return st->err;
}

J40_STATIC void j40__dct_quant_weights(
	int32_t rows, int32_t columns, const float (*bands)[4], int32_t len, float (*out)[4]
) {
	float inv_rows_m1 = 1.0f / (float) (rows - 1), inv_columns_m1 = 1.0f / (float) (columns - 1);
	int32_t x, y, c;
	for (c = 0; c < 3; ++c) {
		for (y = 0; y < rows; ++y) for (x = 0; x < columns; ++x) {
			static const float INV_SQRT2 = 1.0f / 1.414214562373095f; // 1/(sqrt(2) + 1e-6)
			float d = hypotf((float) x * inv_columns_m1, (float) y * inv_rows_m1);
			// TODO spec issue: num_bands doesn't exist (probably len)
			out[y * columns + x][c] = j40__interpolate(d * INV_SQRT2, c, bands, len);
		}
	}
}

// TODO spec issue: VarDCT uses the (row, column) notation, not the (x, y) notation; explicitly note this
// TODO spec improvement: spec can provide computed matrices for default parameters to aid verification
J40_STATIC J40_RETURNS_ERR j40__load_dq_matrix(j40__st *st, int32_t idx, j40__dq_matrix_t *dqmat) {
	enum { MAX_BANDS = 15 };
	const struct j40__dct_params dct = J40__DCT_PARAMS[idx];
	int32_t rows, columns, mode, n, m;
	const float (*params)[4];
	float (*raw)[4] = NULL, bands[MAX_BANDS][4], scratch[64][4];
	int32_t x, y, i, c;

	mode = dqmat->mode;
	if (mode == J40__DQ_ENC_RAW) {
		return 0; // nothing to do
	} else if (mode == J40__DQ_ENC_LIBRARY) {
		mode = dct.def_mode;
		n = dct.def_n;
		m = dct.def_m;
		params = J40__LIBRARY_DCT_PARAMS + dct.def_offset;
	} else {
		n = dqmat->n;
		m = dqmat->m;
		params = dqmat->params;
	}

	rows = 1 << dct.log_rows;
	columns = 1 << dct.log_columns;
	J40__SHOULD(raw = malloc(sizeof(float[4]) * (size_t) (rows * columns)), "!mem");

	switch (mode) {
	case J40__DQ_ENC_DCT:
		J40__TRY(j40__interpolation_bands(st, params, n, bands));
		j40__dct_quant_weights(rows, columns, bands, n, raw);
		break;

	case J40__DQ_ENC_DCT4:
		J40__ASSERT(rows == 8 && columns == 8);
		J40__ASSERT(n <= MAX_BANDS);
		J40__TRY(j40__interpolation_bands(st, params + 2, n, bands));
		j40__dct_quant_weights(4, 4, bands, n, scratch);
		for (c = 0; c < 3; ++c) {
			for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
				raw[y * 8 + x][c] = scratch[(y / 2) * 4 + (x / 2)][c];
			}
			raw[001][c] /= params[0][c];
			raw[010][c] /= params[0][c];
			raw[011][c] /= params[1][c];
		}
		break;

	case J40__DQ_ENC_DCT2:
		J40__ASSERT(rows == 8 && columns == 8);
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

	case J40__DQ_ENC_HORNUSS:
		J40__ASSERT(rows == 8 && columns == 8);
		for (c = 0; c < 3; ++c) {
			for (i = 0; i < 64; ++i) raw[i][c] = params[0][c];
			raw[000][c] = 1.0f;
			raw[001][c] = raw[010][c] = params[1][c];
			raw[011][c] = params[2][c];
		}
		break;

	case J40__DQ_ENC_DCT4X8:
		J40__ASSERT(rows == 8 && columns == 8);
		J40__ASSERT(n <= MAX_BANDS);
		J40__TRY(j40__interpolation_bands(st, params + 1, n, bands));
		// TODO spec bug: 4 rows by 8 columns, not 8 rows by 4 columns (compare with AFV weights4x8)
		// the position (x, y Idiv 2) is also confusing, since it's using the (x, y) notation
		j40__dct_quant_weights(4, 8, bands, n, scratch);
		for (c = 0; c < 3; ++c) {
			for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
				raw[y * 8 + x][c] = scratch[(y / 2) * 8 + x][c];
			}
			raw[001][c] /= params[0][c];
		}
		break;

	case J40__DQ_ENC_AFV:
		J40__ASSERT(rows == 8 && columns == 8);
		J40__ASSERT(n <= MAX_BANDS && m <= MAX_BANDS);
		J40__TRY(j40__interpolation_bands(st, params + 9, n, bands));
		j40__dct_quant_weights(4, 8, bands, n, scratch);
		J40__TRY(j40__interpolation_bands(st, params + 9 + n, m, bands));
		j40__dct_quant_weights(4, 4, bands, m, scratch + 32);
		J40__TRY(j40__interpolation_bands(st, params + 5, 4, bands));
		for (c = 0; c < 3; ++c) {
			// TODO spec bug: this value can never be 1 because it will result in an out-of-bound
			// access in j40__interpolate; libjxl avoids this by adding 1e-6 to the denominator
			static const float FREQS[12] = { // precomputed values of (freqs[i] - lo) / (hi - lo + 1e-6)
				0.000000000f, 0.373436417f, 0.320380100f, 0.379332596f, 0.066671353f, 0.259756761f,
				0.530035651f, 0.789731061f, 0.149436598f, 0.559318823f, 0.669198646f, 0.999999917f,
			};
			scratch[0][c] = params[0][c]; // replaces the top-left corner of weights4x8
			scratch[32][c] = params[1][c]; // replaces the top-left corner of weights4x4
			for (i = 0; i < 12; ++i) scratch[i + 48][c] = j40__interpolate(FREQS[i], c, bands, 4);
			scratch[60][c] = 1.0f;
			for (i = 0; i < 3; ++i) scratch[i + 61][c] = params[i + 2][c];
		}
		for (c = 0; c < 3; ++c) {
			// TODO spec bug: `weight(...)` uses multiple conflicting notations
			static const int8_t MAP[64] = {
				// 1..31 from weights4x8, 33..47 from weights4x4, 48..59 interpolated,
				// 0/32/61..63 directly from parameters, 60 fixed to 1.0
				60, 32, 62, 33, 48, 34, 49, 35,
				 0,  1,  2,  3,  4,  5,  6,  7,
				61, 36, 63, 37, 50, 38, 51, 39,
				 8,  9, 10, 11, 12, 13, 14, 15,
				52, 40, 53, 41, 54, 42, 55, 43,
				16, 17, 18, 19, 20, 21, 22, 23,
				56, 44, 57, 45, 58, 46, 59, 47,
				24, 25, 26, 27, 28, 29, 30, 31,
			};
			for (i = 0; i < 64; ++i) raw[i][c] = scratch[MAP[i]][c];
		}
		break;

	default: J40__UNREACHABLE();
	}

	free(dqmat->params);
	dqmat->mode = J40__DQ_ENC_RAW;
	dqmat->n = (int16_t) rows;
	dqmat->m = (int16_t) columns;
	dqmat->params = raw;
	return 0;

J40__ON_ERROR:
	free(raw);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__natural_order(j40__st *st, int32_t log_rows, int32_t log_columns, int32_t **out) {
	int32_t size = 1 << (log_rows + log_columns), log_slope = log_columns - log_rows;
	int32_t rows8 = 1 << (log_rows - 3), columns8 = 1 << (log_columns - 3);
	int32_t *order = NULL;
	int32_t y, x, key1, o;

	J40__ASSERT(8 >= log_columns && log_columns >= log_rows && log_rows >= 3);

	J40__SHOULD(order = malloc(sizeof(int32_t) * (size_t) size), "!mem");

	o = 0;
	for (y = 0; y < rows8; ++y) for (x = 0; x < columns8; ++x) {
		order[o++] = y << log_columns | x;
	}

	//            d e..
	// +---------/-/-  each diagonal is identified by an integer
	// |       |/ / /    key1 = scaled_x + scaled_y = x + y * 2^log_slope,
	// |_a_b_c_| / /   and covers at least one cell when:
	// |/ / / / / / /    2^(log_columns - 3) <= key1 < 2^(log_columns + 1) - 2^log_slope.
	for (key1 = 1 << (log_columns - 3); o < size; ++key1) {
		// place initial endpoints to leftmost and topmost edges, then fix out-of-bounds later
		int32_t x0 = key1 & ((1 << log_slope) - 1), y0 = key1 >> log_slope, x1 = key1, y1 = 0;
		if (x1 >= (1 << log_columns)) {
			int32_t excess = j40__ceil_div32(x1 - ((1 << log_columns) - 1), 1 << log_slope);
			x1 -= excess << log_slope;
			y1 += excess;
			J40__ASSERT(x1 >= 0 && y1 < (1 << log_rows));
		}
		if (y0 >= (1 << log_rows)) {
			int32_t excess = y0 - ((1 << log_rows) - 1);
			x0 += excess << log_slope;
			y0 -= excess;
			J40__ASSERT(x0 < (1 << log_columns) && y0 >= 0);
		}
		J40__ASSERT(o + (y0 - y1 + 1) <= size);
		if (key1 & 1) {
			for (x = x1, y = y1; x >= x0; x -= 1 << log_slope, ++y) {
				// skip the already covered top-left LLF region
				if (y >= rows8 || x >= columns8) order[o++] = y << log_columns | x;
			}
		} else {
			for (x = x0, y = y0; x <= x1; x += 1 << log_slope, --y) {
				if (y >= rows8 || x >= columns8) order[o++] = y << log_columns | x;
			}
		}
	}
	J40__ASSERT(o == size);

	*out = order;
	return 0;

J40__ON_ERROR:
	free(order);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// frame header & TOC

enum { J40__MAX_PASSES = 11 };

enum {
	J40__BLEND_REPLACE = 0, // new
	J40__BLEND_ADD = 1,     // old + new
	J40__BLEND_BLEND = 2,   // new + old * (1 - new alpha) or equivalent, optionally clamped
	J40__BLEND_MUL_ADD = 3, // old + new * alpha or equivalent, optionally clamped
	J40__BLEND_MUL = 4,     // old * new, optionally clamped
};
typedef struct { int8_t mode, alpha_chan, clamp, src_ref_frame; } j40__blend_info;

typedef struct j40__frame_st {
	int is_last;
	enum { J40__FRAME_REGULAR = 0, J40__FRAME_LF = 1, J40__FRAME_REFONLY = 2, J40__FRAME_REGULAR_SKIPPROG = 3 } type;
	int is_modular; // VarDCT if false
	int has_noise, has_patches, has_splines, use_lf_frame, skip_adapt_lf_smooth;
	int do_ycbcr;
	int32_t jpeg_upsampling; // [0] | [1] << 2 | [2] << 4
	int32_t log_upsampling, *ec_log_upsampling;
	int32_t group_size_shift;
	int32_t x_qm_scale, b_qm_scale;
	int32_t num_passes;
	int8_t shift[J40__MAX_PASSES];
	int8_t log_ds[J40__MAX_PASSES + 1]; // pass i shift range is [log_ds[i+1], log_ds[i])
	int32_t lf_level;
	int32_t x0, y0, width, height;
	int32_t num_groups, num_lf_groups, num_lf_groups_per_row;
	int32_t duration, timecode;
	j40__blend_info blend_info, *ec_blend_info;
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
	// TODO spec bug: m_*_lf_unscaled are wildly incorrect, both in default values and scaling
	float m_lf_scaled[3 /*xyb*/];
	j40__tree_t *global_tree;
	j40__code_spec_t global_codespec;

	// modular only, available after LfGlobal (local groups are always pasted into gmodular)
	j40__modular_t gmodular;
	int32_t num_gm_channels; // <= gmodular.num_channels

	// vardct only, available after LfGlobal
	int32_t global_scale, quant_lf;
	int32_t lf_thr[3 /*xyb*/][15], qf_thr[15];
	int32_t nb_lf_thr[3 /*xyb*/], nb_qf_thr;
	uint8_t *block_ctx_map;
	int32_t block_ctx_size, nb_block_ctx;
	float inv_colour_factor;
	int32_t x_factor_lf, b_factor_lf;
	float base_corr_x, base_corr_b;

	// vardct only, available after HfGlobal/HfPass
	int32_t dct_select_used, order_used; // bitset for DctSelect and order, respectively
	j40__dq_matrix_t dq_matrix[J40__NUM_DCT_PARAMS];
	int32_t num_hf_presets;
	// Lehmer code + sentinel (-1) before actual coefficient decoding,
	// either properly computed or discarded due to non-use later (can be NULL in that case)
	int32_t *orders[J40__MAX_PASSES][J40__NUM_ORDERS][3 /*xyb*/];
	j40__code_spec_t coeff_codespec[J40__MAX_PASSES];
} j40__frame_st;

J40_STATIC J40_RETURNS_ERR j40__frame_header(j40__st *st);
J40_STATIC J40_RETURNS_ERR j40__permutation(
	j40__st *st, j40__code_t *code, int32_t size, int32_t skip, int32_t **out
);
J40_INLINE void j40__apply_permutation(void *targetbuf, void *temp, size_t elemsize, const int32_t *lehmer);
J40_STATIC J40_RETURNS_ERR j40__toc(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__frame_header(j40__st *st) {
	j40__image_st *im = st->image;
	j40__frame_st *f = st->frame;
	int32_t i, j;

	f->is_last = 1;
	f->type = J40__FRAME_REGULAR;
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
	f->width = im->width;
	f->height = im->height;
	f->duration = f->timecode = 0;
	f->blend_info.mode = J40__BLEND_REPLACE;
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
	// TODO spec bug: default values for m_*_lf_unscaled should be reciprocals of the listed values
	f->m_lf_scaled[0] = 1.0f / 4096.0f;
	f->m_lf_scaled[1] = 1.0f / 512.0f;
	f->m_lf_scaled[2] = 1.0f / 256.0f;
	f->global_tree = NULL;
	memset(&f->global_codespec, 0, sizeof(j40__code_spec_t));
	memset(&f->gmodular, 0, sizeof(j40__modular_t));
	f->block_ctx_map = NULL;
	f->inv_colour_factor = 1 / 84.0f;
	f->x_factor_lf = 0;
	f->b_factor_lf = 0;
	f->base_corr_x = 0.0f;
	f->base_corr_b = 1.0f;
	f->dct_select_used = f->order_used = 0;
	memset(f->dq_matrix, 0, sizeof(f->dq_matrix));
	memset(f->orders, 0, sizeof(f->orders));
	memset(f->coeff_codespec, 0, sizeof(f->coeff_codespec));

	J40__TRY(j40__zero_pad_to_byte(st));
	printf("frame starts at %d\n", (int32_t) j40__bits_read(st)); fflush(stdout);

	if (!j40__u(st, 1)) { // !all_default
		int full_frame = 1;
		uint64_t flags;
		f->type = j40__u(st, 2);
		f->is_modular = j40__u(st, 1);
		flags = j40__u64(st);
		f->has_noise = (int) (flags & 1);
		f->has_patches = (int) (flags >> 1 & 1);
		f->has_splines = (int) (flags >> 4 & 1);
		f->use_lf_frame = (int) (flags >> 5 & 1);
		f->skip_adapt_lf_smooth = (int) (flags >> 7 & 1);
		if (!im->xyb_encoded) f->do_ycbcr = j40__u(st, 1);
		if (!f->use_lf_frame) {
			if (f->do_ycbcr) f->jpeg_upsampling = j40__u(st, 6); // yes, we are lazy
			f->log_upsampling = j40__u(st, 2);
			J40__SHOULD(f->log_upsampling == 0, "TODO: upsampling is not yet implemented");
			J40__SHOULD(f->ec_log_upsampling = malloc(sizeof(int32_t) * (size_t) im->num_extra_channels), "!mem");
			for (i = 0; i < im->num_extra_channels; ++i) {
				f->ec_log_upsampling[i] = j40__u(st, 2);
				J40__SHOULD(f->ec_log_upsampling[i] == 0, "TODO: upsampling is not yet implemented");
			}
		}
		if (f->is_modular) {
			f->group_size_shift = 7 + j40__u(st, 2);
		} else if (im->xyb_encoded) {
			f->x_qm_scale = j40__u(st, 3);
			f->b_qm_scale = j40__u(st, 3);
		}
		if (f->type != J40__FRAME_REFONLY) {
			f->num_passes = j40__u32(st, 1, 0, 2, 0, 3, 0, 4, 3);
			if (f->num_passes > 1) {
				// SPEC this part is especially flaky and the spec and libjxl don't agree to each other.
				// we do the most sensible thing that is still compatible to libjxl:
				// - downsample should be decreasing (or stay same)
				// - last_pass should be strictly increasing and last_pass[0] (if any) should be 0
				// see also https://github.com/libjxl/libjxl/issues/1401
				int8_t log_ds[4];
				int32_t ppass = 0, num_ds = j40__u32(st, 0, 0, 1, 0, 2, 0, 3, 1);
				J40__SHOULD(num_ds < f->num_passes, "pass");
				for (i = 0; i < f->num_passes - 1; ++i) f->shift[i] = (int8_t) j40__u(st, 2);
				f->shift[f->num_passes - 1] = 0;
				for (i = 0; i < num_ds; ++i) {
					log_ds[i] = (int8_t) j40__u(st, 2);
					if (i > 0) J40__SHOULD(log_ds[i - 1] >= log_ds[i], "pass");
				}
				for (i = 0; i < num_ds; ++i) {
					int32_t pass = j40__u32(st, 0, 0, 1, 0, 2, 0, 0, 3);
					J40__SHOULD(i > 0 ? ppass < pass && pass < f->num_passes : pass == 0, "pass");
					while (ppass < pass) f->log_ds[++ppass] = i > 0 ? log_ds[i - 1] : 3;
				}
				while (ppass < f->num_passes) f->log_ds[++ppass] = i > 0 ? log_ds[num_ds - 1] : 3;
			}
		}
		if (f->type == J40__FRAME_LF) {
			f->lf_level = j40__u(st, 2) + 1;
		} else if (j40__u(st, 1)) { // have_crop
			if (f->type != J40__FRAME_REFONLY) { // SPEC missing UnpackSigned
				f->x0 = j40__unpack_signed(j40__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30));
				f->y0 = j40__unpack_signed(j40__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30));
			}
			f->width = j40__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30);
			f->height = j40__u32(st, 0, 8, 256, 11, 2304, 14, 18688, 30);
			full_frame = f->x0 <= 0 && f->y0 <= 0 &&
				f->width + f->x0 >= im->width && f->height + f->y0 >= im->height;
		}
		if (f->type == J40__FRAME_REGULAR || f->type == J40__FRAME_REGULAR_SKIPPROG) {
			J40__SHOULD(f->ec_blend_info = malloc(sizeof(j40__blend_info) * (size_t) im->num_extra_channels), "!mem");
			for (i = -1; i < im->num_extra_channels; ++i) {
				j40__blend_info *blend = i < 0 ? &f->blend_info : &f->ec_blend_info[i];
				blend->mode = (int8_t) j40__u32(st, 0, 0, 1, 0, 2, 0, 3, 2);
				if (im->num_extra_channels > 0) {
					if (blend->mode == J40__BLEND_BLEND || blend->mode == J40__BLEND_MUL_ADD) {
						blend->alpha_chan = (int8_t) j40__u32(st, 0, 0, 1, 0, 2, 0, 3, 3);
						blend->clamp = (int8_t) j40__u(st, 1);
					} else if (blend->mode == J40__BLEND_MUL) {
						blend->clamp = (int8_t) j40__u(st, 1);
					}
				}
				if (!full_frame || blend->mode != J40__BLEND_REPLACE) {
					blend->src_ref_frame = (int8_t) j40__u(st, 2);
				}
			}
			if (im->anim_tps_denom) { // have_animation stored implicitly
				f->duration = j40__u32(st, 0, 0, 1, 0, 0, 8, 0, 32); // TODO uh, u32?
				if (im->anim_have_timecodes) {
					f->timecode = j40__u(st, 32); // TODO uh, u32??
				}
			}
			f->is_last = j40__u(st, 1);
		} else {
			f->is_last = 0;
		}
		if (f->type != J40__FRAME_LF && !f->is_last) f->save_as_ref = j40__u(st, 2);
		// SPEC this condition is essentially swapped with the default value in the spec
		if (f->type == J40__FRAME_REFONLY || (
			full_frame &&
			(f->type == J40__FRAME_REGULAR || f->type == J40__FRAME_REGULAR_SKIPPROG) &&
			f->blend_info.mode == J40__BLEND_REPLACE &&
			(f->duration == 0 || f->save_as_ref != 0) &&
			!f->is_last
		)) {
			f->save_before_ct = j40__u(st, 1);
		} else {
			f->save_before_ct = (f->type == J40__FRAME_LF);
		}
		J40__TRY(j40__name(st, &f->name_len, &f->name));
		{ // RestorationFilter
			int restoration_all_default = j40__u(st, 1);
			f->gab.enabled = restoration_all_default ? 1 : j40__u(st, 1);
			if (f->gab.enabled) {
				if (j40__u(st, 1)) { // gab_custom
					for (i = 0; i < 3; ++i) for (j = 0; j < 2; ++j) f->gab.weights[i][j] = j40__f16(st);
				}
			}
			f->epf.iters = restoration_all_default ? 2 : j40__u(st, 2);
			if (f->epf.iters) {
				if (!f->is_modular && j40__u(st, 1)) { // epf_sharp_custom
					for (i = 0; i < 8; ++i) f->epf.sharp_lut[i] = j40__f16(st);
				}
				if (j40__u(st, 1)) { // epf_weight_custom
					for (i = 0; i < 3; ++i) f->epf.channel_scale[i] = j40__f16(st);
					J40__TRY(j40__skip(st, 32)); // ignored
				}
				if (j40__u(st, 1)) { // epf_sigma_custom
					if (!f->is_modular) f->epf.quant_mul = j40__f16(st);
					f->epf.pass0_sigma_circle = j40__f16(st);
					f->epf.pass2_sigma_circle = j40__f16(st);
					f->epf.border_sad_mul = j40__f16(st);
				}
				if (f->epf.iters && f->is_modular) f->epf.sigma_for_modular = j40__f16(st);
			}
			if (!restoration_all_default) J40__TRY(j40__extensions(st));
		}
		J40__TRY(j40__extensions(st));
	}
	J40__RAISE_DELAYED();

	if (im->xyb_encoded && im->want_icc) f->save_before_ct = 1; // ignores the decoded bit
	f->num_groups = j40__ceil_div32(f->width, 1 << f->group_size_shift) *
		j40__ceil_div32(f->height, 1 << f->group_size_shift);
	f->num_lf_groups_per_row = j40__ceil_div32(f->width, 8 << f->group_size_shift);
	f->num_lf_groups = f->num_lf_groups_per_row * j40__ceil_div32(f->height, 8 << f->group_size_shift);
	return 0;

J40__ON_ERROR:
	free(f->ec_log_upsampling);
	free(f->ec_blend_info);
	free(f->name);
	f->ec_log_upsampling = NULL;
	f->ec_blend_info = NULL;
	f->name = NULL;
	return st->err;
}

// also used in j40__hf_global; out is terminated by a sentinel (-1) or NULL if empty
// TODO permutation may have to handle more than 2^31 entries
J40_STATIC J40_RETURNS_ERR j40__permutation(
	j40__st *st, j40__code_t *code, int32_t size, int32_t skip, int32_t **out
) {
	int32_t *arr = NULL;
	int32_t i, prev, end;

	J40__ASSERT(code->spec->num_dist == 8 + !!code->spec->lz77_enabled);

	// SPEC this is the number of integers to read, not the last offset to read (can differ when skip > 0)
	end = j40__code(st, j40__min32(7, j40__ceil_lg32((uint32_t) size + 1)), 0, code);
	J40__SHOULD(end <= size - skip, "perm"); // SPEC missing
	if (end == 0) {
		*out = NULL;
		return 0;
	}

	J40__SHOULD(arr = malloc(sizeof(int32_t) * (size_t) (end + 1)), "!mem");
	prev = 0;
	for (i = 0; i < end; ++i) {
		prev = arr[i] = j40__code(st, j40__min32(7, j40__ceil_lg32((uint32_t) prev + 1)), 0, code);
		J40__SHOULD(prev < size - (skip + i), "perm"); // SPEC missing
	}
	arr[end] = -1; // sentinel
	*out = arr;
J40__ON_ERROR:
	return st->err;
}

// target is pre-shifted by skip
J40_INLINE void j40__apply_permutation(
	void *targetbuf, void *temp, size_t elemsize, const int32_t *lehmer
) {
	char *target = targetbuf;
	if (!lehmer) return;
	while (*lehmer >= 0) {
		size_t x = (size_t) *lehmer++;
		memcpy(temp, target + elemsize * x, elemsize);
		memmove(target + elemsize, target, elemsize * x);
		memcpy(target, temp, elemsize);
		target += elemsize;
	}
}

J40_STATIC J40_RETURNS_ERR j40__toc(j40__st *st) {
	j40__frame_st *f = st->frame;
	typedef struct { int32_t lo, hi; } toc_t;
	int32_t size = f->num_passes == 1 && f->num_groups == 1 ? 1 :
		1 /*lf_global*/ + f->num_lf_groups /*lf_group*/ +
		1 /*hf_global + hf_pass*/ + f->num_passes * f->num_groups /*group_pass*/;
	toc_t *toc, temp;
	int32_t *lehmer = NULL;
	j40__code_spec_t codespec = {0};
	j40__code_t code = { .spec = &codespec };
	int32_t base, i;

	J40__SHOULD(toc = malloc(sizeof(toc_t) * (size_t) size), "!mem");

	if (j40__u(st, 1)) { // permuted
		J40__TRY(j40__code_spec(st, 8, &codespec));
		J40__TRY(j40__permutation(st, &code, size, 0, &lehmer));
		J40__TRY(j40__finish_and_free_code(st, &code));
		j40__free_code_spec(&codespec);
		J40__RAISE("TODO: should reorder groups in this case");
	}
	J40__TRY(j40__zero_pad_to_byte(st));

	for (i = 0; i < size; ++i) {
		toc[i].lo = i > 0 ? toc[i - 1].hi : 0;
		toc[i].hi = toc[i].lo + j40__u32(st, 0, 10, 1024, 14, 17408, 22, 4211712, 30);
	}
	J40__TRY(j40__zero_pad_to_byte(st));
	base = 0; // TODO (int32_t) (st->bits_read / 8);
	for (i = 0; i < size; ++i) toc[i].lo += base, toc[i].hi += base;

	if (lehmer) j40__apply_permutation(toc, &temp, sizeof(toc_t), lehmer);

	printf("TOC: lf_global %d-%d\n", toc[0].lo, toc[0].hi);
	if (size > 1) {
		int32_t j, k;
		printf("     lf_group");
		for (i = 1, j = 0; j < f->num_lf_groups; ++i, ++j) printf(" %d:%d-%d", j, toc[i].lo, toc[i].hi);
		printf("\n     hf_global/hf_pass %d-%d\n", toc[i].lo, toc[i].hi); ++i;
		for (j = 0; j < f->num_passes; ++j) {
			printf("     pass[%d]", j);
			for (k = 0; k < f->num_groups; ++i, ++k) printf(" %d:%d-%d", k, toc[i].lo, toc[i].hi);
			printf("\n");
		}
	}
	fflush(stdout);

	free(lehmer);
	free(toc); // TODO use toc somehow (especially required for permuted one)
	return 0;

J40__ON_ERROR:
	free(lehmer);
	free(toc);
	j40__free_code(&code);
	j40__free_code_spec(&codespec);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// DCT

// both use `in` as a scratch space as well, so `in` will be altered after return
J40_STATIC void j40__forward_dct_unscaled(
	float *J40_RESTRICT out, float *J40_RESTRICT in, int32_t t, int32_t rep
);
J40_STATIC void j40__inverse_dct(
	float *J40_RESTRICT out, float *J40_RESTRICT in, int32_t t, int32_t rep
);

J40_STATIC void j40__forward_dct2d_scaled_for_llf(
	float *J40_RESTRICT buf, float *J40_RESTRICT scratch, int32_t log_rows, int32_t log_columns
);
J40_STATIC void j40__inverse_dct2d(
	float *J40_RESTRICT buf, float *J40_RESTRICT scratch, int32_t log_rows, int32_t log_columns
);

J40_STATIC void j40__inverse_dct11(float *buf);
J40_STATIC void j40__inverse_dct22(float *buf);
J40_STATIC void j40__inverse_hornuss(float *buf);
J40_STATIC void j40__inverse_dct32(float *buf);
J40_STATIC void j40__inverse_dct23(float *buf);
J40_STATIC void j40__inverse_afv22(float *J40_RESTRICT out, float *J40_RESTRICT in);
J40_STATIC void j40__inverse_afv(float *buf, int flipx, int flipy);

#ifdef J40_IMPLEMENTATION

// this is more or less a direct translation of mcos2/3 algorithms described in:
// Perera, S. M., & Liu, J. (2018). Lowest Complexity Self-Recursive Radix-2 DCT II/III Algorithms.
// SIAM Journal on Matrix Analysis and Applications, 39(2), 664--682.

// [(1<<n) + k] = 1/(2 cos((k+0.5)/2^(n+1) pi)) for n >= 1 and 0 <= k < 2^n
J40_STATIC const float J40__HALF_SECANTS[256] = {
	0, 0, // unused
	0.54119610f, 1.30656296f, // n=1 for DCT-4
	0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f, // n=2 for DCT-8
	// n=3 for DCT-16
	0.50241929f, 0.52249861f, 0.56694403f, 0.64682178f, 0.78815462f, 1.06067769f, 1.72244710f, 5.10114862f,
	// n=4 for DCT-32
	0.50060300f, 0.50547096f, 0.51544731f, 0.53104259f, 0.55310390f, 0.58293497f, 0.62250412f, 0.67480834f,
	0.74453627f, 0.83934965f, 0.97256824f, 1.16943993f, 1.48416462f, 2.05778101f, 3.40760842f, 10.1900081f,
	// n=5 for DCT-64
	0.50015064f, 0.50135845f, 0.50378873f, 0.50747117f, 0.51245148f, 0.51879271f, 0.52657732f, 0.53590982f,
	0.54692044f, 0.55976981f, 0.57465518f, 0.59181854f, 0.61155735f, 0.63423894f, 0.66031981f, 0.69037213f,
	0.72512052f, 0.76549416f, 0.81270209f, 0.86834472f, 0.93458360f, 1.01440826f, 1.11207162f, 1.23383274f,
	1.38929396f, 1.59397228f, 1.87467598f, 2.28205007f, 2.92462843f, 4.08461108f, 6.79675071f, 20.3738782f,
	// n=6 for DCT-128
	0.50003765f, 0.50033904f, 0.50094272f, 0.50185052f, 0.50306519f, 0.50459044f, 0.50643095f, 0.50859242f,
	0.51108159f, 0.51390633f, 0.51707566f, 0.52059987f, 0.52449054f, 0.52876071f, 0.53342493f, 0.53849944f,
	0.54400225f, 0.54995337f, 0.55637499f, 0.56329167f, 0.57073059f, 0.57872189f, 0.58729894f, 0.59649876f,
	0.60636246f, 0.61693573f, 0.62826943f, 0.64042034f, 0.65345190f, 0.66743520f, 0.68245013f, 0.69858665f,
	0.71594645f, 0.73464482f, 0.75481294f, 0.77660066f, 0.80017990f, 0.82574877f, 0.85353675f, 0.88381100f,
	0.91688445f, 0.95312587f, 0.99297296f, 1.03694904f, 1.08568506f, 1.13994868f, 1.20068326f, 1.26906117f,
	1.34655763f, 1.43505509f, 1.53699410f, 1.65559652f, 1.79520522f, 1.96181785f, 2.16395782f, 2.41416000f,
	2.73164503f, 3.14746219f, 3.71524274f, 4.53629094f, 5.82768838f, 8.15384860f, 13.5842903f, 40.7446881f,
	// n=7 for DCT-256
	0.50000941f, 0.50008472f, 0.50023540f, 0.50046156f, 0.50076337f, 0.50114106f, 0.50159492f, 0.50212529f,
	0.50273257f, 0.50341722f, 0.50417977f, 0.50502081f, 0.50594098f, 0.50694099f, 0.50802161f, 0.50918370f,
	0.51042817f, 0.51175599f, 0.51316821f, 0.51466598f, 0.51625048f, 0.51792302f, 0.51968494f, 0.52153769f,
	0.52348283f, 0.52552196f, 0.52765682f, 0.52988922f, 0.53222108f, 0.53465442f, 0.53719139f, 0.53983424f,
	0.54258533f, 0.54544717f, 0.54842239f, 0.55151375f, 0.55472418f, 0.55805673f, 0.56151465f, 0.56510131f,
	0.56882030f, 0.57267538f, 0.57667051f, 0.58080985f, 0.58509780f, 0.58953898f, 0.59413825f, 0.59890075f,
	0.60383188f, 0.60893736f, 0.61422320f, 0.61969575f, 0.62536172f, 0.63122819f, 0.63730265f, 0.64359303f,
	0.65010770f, 0.65685553f, 0.66384594f, 0.67108889f, 0.67859495f, 0.68637535f, 0.69444203f, 0.70280766f,
	0.71148577f, 0.72049072f, 0.72983786f, 0.73954355f, 0.74962527f, 0.76010172f, 0.77099290f, 0.78232026f,
	0.79410679f, 0.80637720f, 0.81915807f, 0.83247799f, 0.84636782f, 0.86086085f, 0.87599311f, 0.89180358f,
	0.90833456f, 0.92563200f, 0.94374590f, 0.96273078f, 0.98264619f, 1.00355728f, 1.02553551f, 1.04865941f,
	1.07301549f, 1.09869926f, 1.12581641f, 1.15448427f, 1.18483336f, 1.21700940f, 1.25117548f, 1.28751481f,
	1.32623388f, 1.36756626f, 1.41177723f, 1.45916930f, 1.51008903f, 1.56493528f, 1.62416951f, 1.68832855f,
	1.75804061f, 1.83404561f, 1.91722116f, 2.00861611f, 2.10949453f, 2.22139378f, 2.34620266f, 2.48626791f,
	2.64454188f, 2.82479140f, 3.03189945f, 3.27231159f, 3.55471533f, 3.89110779f, 4.29853753f, 4.80207601f,
	5.44016622f, 6.27490841f, 7.41356676f, 9.05875145f, 11.6446273f, 16.3000231f, 27.1639777f, 81.4878422f,
};

// TODO spec bug: ScaleF doesn't match with the current libjxl! it turns out that this is actually
// a set of factors for the Arai, Agui, Nakajima DCT & IDCT algorithm, which was only used in
// older versions of libjxl (both the current libjxl and J40 currently uses Perera-Liu) and
// not even a resampling algorithm to begin with.
//
// [(1<<N) + k] = 1 / (cos(k/2^(4+N) pi) * cos(k/2^(3+N) pi) * cos(k/2^(2+N) pi) * 2^N)
//                for N >= 1 and 0 <= k < 2^N
J40_STATIC const float J40__LF2LLF_SCALES[64] = {
	0, // unused
	1.00000000f, // N=1, n=8
	0.50000000f, 0.55446868f, // N=2, n=16
	0.25000000f, 0.25644002f, 0.27723434f, 0.31763984f, // N=4, n=32
	// N=8, n=64
	0.12500000f, 0.12579419f, 0.12822001f, 0.13241272f, 0.13861717f, 0.14722207f, 0.15881992f, 0.17431123f,
	// N=16, n=128
	0.06250000f, 0.06259894f, 0.06289709f, 0.06339849f, 0.06411001f, 0.06504154f, 0.06620636f, 0.06762155f,
	0.06930858f, 0.07129412f, 0.07361103f, 0.07629973f, 0.07940996f, 0.08300316f, 0.08715562f, 0.09196277f,
	// N=32, n=256
	0.03125000f, 0.03126236f, 0.03129947f, 0.03136146f, 0.03144855f, 0.03156101f, 0.03169925f, 0.03186372f,
	0.03205500f, 0.03227376f, 0.03252077f, 0.03279691f, 0.03310318f, 0.03344071f, 0.03381077f, 0.03421478f,
	0.03465429f, 0.03513107f, 0.03564706f, 0.03620441f, 0.03680552f, 0.03745302f, 0.03814986f, 0.03889931f,
	0.03970498f, 0.04057091f, 0.04150158f, 0.04250201f, 0.04357781f, 0.04473525f, 0.04598138f, 0.04732417f,
};

#define J40__SQRT2 1.4142135623730951f

#define J40__DCT_ARGS float *J40_RESTRICT out, float *J40_RESTRICT in, int32_t t
#define J40__REPEAT1() for (r1 = 0; r1 < rep1 * rep2; r1 += rep2)
#define J40__REPEAT2() for (r2 = 0; r2 < rep2; ++r2)
#define J40__IN(i) in[(i) * stride + r1 + r2]
#define J40__OUT(i) out[(i) * stride + r1 + r2]

J40_ALWAYS_INLINE void j40__forward_dct_core(
	J40__DCT_ARGS, int32_t rep1, int32_t rep2,
	void (*half_forward_dct)(J40__DCT_ARGS, int32_t rep1, int32_t rep2)
) {
	int32_t r1, r2, i, N = 1 << t, stride = rep1 * rep2;

	// out[0..N) = W^c_N H_N in[0..N)
	J40__REPEAT1() {
		for (i = 0; i < N / 2; ++i) {
			float mult = J40__HALF_SECANTS[N / 2 + i];
			J40__REPEAT2() {
				float x = J40__IN(i), y = J40__IN(N - i - 1);
				J40__OUT(i) = x + y;
				J40__OUT(N / 2 + i) = (x - y) * mult;
			}
		}
	}

	// in[0..N/2) = mcos2(out[0..N/2), N/2)
	// in[N/2..N) = mcos2(out[N/2..N), N/2)
	half_forward_dct(in, out, t - 1, rep1, rep2);
	half_forward_dct(in + N / 2 * stride, out + N / 2 * stride, t - 1, rep1, rep2);

	// out[0,2..N) = in[0..N/2)
	J40__REPEAT1() for (i = 0; i < N / 2; ++i) J40__REPEAT2() {
		J40__OUT(i * 2) = J40__IN(i);
	}

	// out[1,3..N) = B_(N/2) in[N/2..N)
	J40__REPEAT1() {
		J40__REPEAT2() J40__OUT(1) = J40__SQRT2 * J40__IN(N / 2) + J40__IN(N / 2 + 1);
		for (i = 1; i < N / 2 - 1; ++i) {
			J40__REPEAT2() J40__OUT(i * 2 + 1) = J40__IN(N / 2 + i) + J40__IN(N / 2 + i + 1);
		}
		J40__REPEAT2() J40__OUT(N - 1) = J40__IN(N - 1);
	}
}

J40_ALWAYS_INLINE void j40__inverse_dct_core(
	J40__DCT_ARGS, int32_t rep1, int32_t rep2,
	void (*half_inverse_dct)(J40__DCT_ARGS, int32_t rep1, int32_t rep2)
) {
	int32_t r1, r2, i, N = 1 << t, stride = rep1 * rep2;

	// out[0..N/2) = in[0,2..N)
	J40__REPEAT1() {
		for (i = 0; i < N / 2; ++i) {
			J40__REPEAT2() J40__OUT(i) = J40__IN(i * 2);
		}
	}

	// out[N/2..N) = (B_(N/2))^T in[1,3..N)
	J40__REPEAT1() {
		J40__REPEAT2() J40__OUT(N / 2) = J40__SQRT2 * J40__IN(1);
		for (i = 1; i < N / 2; ++i) {
			J40__REPEAT2() J40__OUT(N / 2 + i) = J40__IN(i * 2 - 1) + J40__IN(i * 2 + 1);
		}
	}

	// in[0..N/2) = mcos3(out[0..N/2), N/2)
	// in[N/2..N) = mcos3(out[N/2..N), N/2)
	half_inverse_dct(in, out, t - 1, rep1, rep2);
	half_inverse_dct(in + N / 2 * stride, out + N / 2 * stride, t - 1, rep1, rep2);

	// out[0..N) = (H_N)^T W^c_N in[0..N)
	J40__REPEAT1() {
		for (i = 0; i < N / 2; ++i) {
			float mult = J40__HALF_SECANTS[N / 2 + i];
			J40__REPEAT2() {
				float x = J40__IN(i), y = J40__IN(N / 2 + i);
				// this might look wasteful, but modern compilers can optimize them into FMA
				// which can be actually faster than a single multiplication (TODO verify this)
				J40__OUT(i) = x + y * mult;
				J40__OUT(N - i - 1) = x - y * mult;
			}
		}
	}
}

J40_ALWAYS_INLINE void j40__dct2(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	int32_t r1, r2, stride = rep1 * rep2;
	J40__ASSERT(t == 1); (void) t;
	J40__REPEAT1() J40__REPEAT2() {
		float x = J40__IN(0), y = J40__IN(1);
		J40__OUT(0) = x + y;
		J40__OUT(1) = x - y;
	}
}

J40_ALWAYS_INLINE void j40__forward_dct4(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	J40__ASSERT(t == 2); (void) t;
	j40__forward_dct_core(out, in, 2, rep1, rep2, j40__dct2);
}

J40_STATIC void j40__forward_dct_recur(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	if (t < 4) {
		J40__ASSERT(t == 3);
		j40__forward_dct_core(out, in, 3, rep1, rep2, j40__forward_dct4);
	} else {
		j40__forward_dct_core(out, in, t, rep1, rep2, j40__forward_dct_recur);
	}
}

J40_STATIC void j40__forward_dct_recur_x8(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	J40__ASSERT(rep2 == 8); (void) rep2;
	if (t < 4) {
		J40__ASSERT(t == 3);
		j40__forward_dct_core(out, in, 3, rep1, 8, j40__forward_dct4);
	} else {
		j40__forward_dct_core(out, in, t, rep1, 8, j40__forward_dct_recur_x8);
	}
}

// this omits the final division by (1 << t)!
J40_STATIC void j40__forward_dct_unscaled(J40__DCT_ARGS, int32_t rep) {
	if (t <= 0) {
		memcpy(out, in, sizeof(float) * (size_t) rep);
	} else if (rep % 8 == 0) {
		if (t == 1) return j40__dct2(out, in, 1, rep / 8, 8);
		if (t == 2) return j40__forward_dct4(out, in, 2, rep / 8, 8);
		j40__forward_dct_recur_x8(out, in, t, rep / 8, 8);
	} else {
		if (t == 1) return j40__dct2(out, in, 1, rep, 1);
		if (t == 2) return j40__forward_dct4(out, in, 2, rep, 1);
		j40__forward_dct_recur(out, in, t, rep, 1);
	}
}

J40_ALWAYS_INLINE void j40__forward_dct_unscaled_view(j40__view_f32 *outv, j40__view_f32 *inv) {
	j40__adapt_view_f32(outv, inv->logw, inv->logh);
	j40__forward_dct_unscaled(outv->ptr, inv->ptr, inv->logh, 1 << inv->logw);
}

J40_ALWAYS_INLINE void j40__inverse_dct4(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	J40__ASSERT(t == 2); (void) t;
	j40__inverse_dct_core(out, in, 2, rep1, rep2, j40__dct2);
}

J40_STATIC void j40__inverse_dct_recur(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	if (t < 4) {
		J40__ASSERT(t == 3);
		j40__inverse_dct_core(out, in, 3, rep1, rep2, j40__inverse_dct4);
	} else {
		j40__inverse_dct_core(out, in, t, rep1, rep2, j40__inverse_dct_recur);
	}
}

J40_STATIC void j40__inverse_dct_recur_x8(J40__DCT_ARGS, int32_t rep1, int32_t rep2) {
	J40__ASSERT(rep2 == 8); (void) rep2;
	if (t < 4) {
		J40__ASSERT(t == 3);
		j40__inverse_dct_core(out, in, 3, rep1, 8, j40__inverse_dct4);
	} else {
		j40__inverse_dct_core(out, in, t, rep1, 8, j40__inverse_dct_recur_x8);
	}
}

J40_STATIC void j40__inverse_dct(J40__DCT_ARGS, int32_t rep) {
	if (t <= 0) {
		memcpy(out, in, sizeof(float) * (size_t) rep);
	} else if (rep % 8 == 0) {
		if (t == 1) return j40__dct2(out, in, 1, rep / 8, 8);
		if (t == 2) return j40__inverse_dct4(out, in, 2, rep / 8, 8);
		return j40__inverse_dct_recur_x8(out, in, t, rep / 8, 8);
	} else {
		if (t == 1) return j40__dct2(out, in, 1, rep, 1);
		if (t == 2) return j40__inverse_dct4(out, in, 2, rep, 1);
		return j40__inverse_dct_recur(out, in, t, rep, 1);
	}
}

J40_ALWAYS_INLINE void j40__inverse_dct_view(j40__view_f32 *outv, j40__view_f32 *inv) {
	j40__adapt_view_f32(outv, inv->logw, inv->logh);
	j40__inverse_dct(outv->ptr, inv->ptr, inv->logh, 1 << inv->logw);
}

#undef J40__DCT_ARGS
#undef J40__IN
#undef J40__OUT

J40_STATIC void j40__forward_dct2d_scaled_for_llf(
	float *J40_RESTRICT buf, float *J40_RESTRICT scratch, int32_t log_rows, int32_t log_columns
) {
	j40__view_f32 bufv = j40__make_view_f32(log_columns, log_rows, buf);
	j40__view_f32 scratchv = j40__make_view_f32(log_columns, log_rows, scratch);
	float *p;
	int32_t x, y;

	j40__forward_dct_unscaled_view(&scratchv, &bufv);
	j40__transpose_view_f32(&bufv, scratchv);
	j40__forward_dct_unscaled_view(&scratchv, &bufv);
	// TODO spec bug (I.6.5): the pseudocode only works correctly when C > R;
	// the condition itself can be eliminated by inlining DCT_2D though
	J40__VIEW_FOREACH(scratchv, y, x, p) {
		// hopefully compiler will factor the second multiplication out of the inner loop (TODO verify this)
		*p *= J40__LF2LLF_SCALES[(1 << scratchv.logw) + x] * J40__LF2LLF_SCALES[(1 << scratchv.logh) + y];
	}
	// TODO spec improvement (I.6.3 note): given the pseudocode, it might be better to
	// state that the DCT result *always* has C <= R, transposing as necessary.
	if (log_columns > log_rows) {
		j40__transpose_view_f32(&bufv, scratchv);
	} else {
		j40__copy_view_f32(&bufv, scratchv);
	}
	J40__ASSERT(bufv.logw == j40__max32(log_columns, log_rows));
	J40__ASSERT(bufv.logh == j40__min32(log_columns, log_rows));
}

J40_STATIC void j40__inverse_dct2d(
	float *J40_RESTRICT buf, float *J40_RESTRICT scratch, int32_t log_rows, int32_t log_columns
) {
	j40__view_f32 bufv;
	j40__view_f32 scratchv = j40__make_view_f32(log_columns, log_rows, scratch);

	if (log_columns > log_rows) {
		// TODO spec improvement: coefficients start being transposed, note this as well
		bufv = j40__make_view_f32(log_columns, log_rows, buf);
		j40__transpose_view_f32(&scratchv, bufv);
	} else {
		bufv = j40__make_view_f32(log_rows, log_columns, buf);
		j40__copy_view_f32(&scratchv, bufv);
	}
	j40__inverse_dct_view(&bufv, &scratchv);
	j40__transpose_view_f32(&scratchv, bufv);
	j40__inverse_dct_view(&bufv, &scratchv);
	J40__ASSERT(bufv.logw == log_columns && bufv.logh == log_rows);
}

// a single iteration of AuxIDCT2x2
J40_ALWAYS_INLINE void j40__aux_inverse_dct11(float *out, float *in, int32_t x, int32_t y, int32_t S2) {
	int32_t p = y * 8 + x, q = (y * 2) * 8 + (x * 2);
	float c00 = in[p], c01 = in[p + S2], c10 = in[p + S2 * 8], c11 = in[p + S2 * 9];
	out[q + 000] = c00 + c01 + c10 + c11; // r00
	out[q + 001] = c00 + c01 - c10 - c11; // r01
	out[q + 010] = c00 - c01 + c10 - c11; // r10
	out[q + 011] = c00 - c01 - c10 + c11; // r11
}

J40_STATIC void j40__inverse_dct11(float *buf) {
	float scratch[64];
	int32_t x, y;

	// TODO spec issue: only the "top-left" SxS cells, not "top"
	j40__aux_inverse_dct11(buf, buf, 0, 0, 1); // updates buf[(0..1)*8+(0..1)]
	// updates scratch[(0..3)*8+(0..3)], copying other elements from buf in verbatim
	memcpy(scratch, buf, sizeof(float) * 64);
	for (y = 0; y < 2; ++y) for (x = 0; x < 2; ++x) j40__aux_inverse_dct11(scratch, buf, x, y, 2);
	// updates the entire buf
	for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x) j40__aux_inverse_dct11(buf, scratch, x, y, 4);
}

J40_STATIC void j40__inverse_dct22(float *buf) {
	float scratch[64];
	int32_t x, y;

	j40__aux_inverse_dct11(buf, buf, 0, 0, 1);
	// after the top-left inverse DCT2x2, four 4x4 submatrices are formed and IDCTed individually.
	// IDCT itself requires transposition and the final matrices are stitched in a different way,
	// but it turns out that IDCT can be done in place, only requiring the final stitching.
	//
	// input                        after transposition          output
	// a1 a2 b1 b2 c1 c2 d1 d2      a1 a3 e1 e3 i1 i3 m1 m3      a1 e1 i1 m1 a2 e2 i2 m2
	// a3 a4 b3 b4 c3 c4 d3 d4      a2 a4 e2 e4 i2 i4 m2 m4      b1 f1 j1 n1 b2 f2 j2 n2
	// e1 e2 f1 f2 g1 g2 h1 h2      b1 b3 f1 f3 j1 j3 n1 n3      c1 g1 k1 o1 c2 g2 k2 o2
	// e3 e4 f3 f4 g3 g4 h3 h4 ---> b2 b4 f2 f4 j2 j4 n2 n4 ---> d1 k1 l1 p1 d2 k2 l2 p2
	// i1 i2 j1 j2 k1 k2 l1 l2      c1 c3 g1 g3 k1 k3 o1 o3      a3 e3 i3 m3 a4 e4 i4 m4
	// i3 i4 j3 j4 k3 k4 l3 l4      c2 c4 g2 g4 k2 k4 o2 o4      b3 f3 j3 n3 b4 f4 j4 n4
	// m1 m2 n1 n2 o1 o2 p1 p2      d1 d3 h1 h3 l1 l3 p1 p3      c3 g3 k3 o3 c4 g4 k4 o4
	// m3 m4 n3 n4 o3 o4 p3 p4      d2 d4 h2 h4 l2 l4 p2 p4      d3 k3 l3 p3 d4 k4 l4 p4
	//
	// TODO spec issue: notationally `sample` is a *4-dimensional* array, which is not very clear
	j40__inverse_dct(scratch, buf, 2, 16); // columnar IDCT for a#-m#, b#-n#, c#-o# and d#-p#
	for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) buf[x * 8 + y] = scratch[y * 8 + x];
	j40__inverse_dct(scratch, buf, 2, 16); // columnar IDCT for a#-d#, e#-h#, i#-l# and m#-p#
	for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x) {
		buf[y * 8 + x] = scratch[(y * 2) * 8 + (x * 2)];
		buf[y * 8 + (x + 4)] = scratch[(y * 2 + 1) * 8 + (x * 2)];
		buf[(y + 4) * 8 + x] = scratch[(y * 2) * 8 + (x * 2 + 1)];
		buf[(y + 4) * 8 + (x + 4)] = scratch[(y * 2 + 1) * 8 + (x * 2 + 1)];
	}
}

J40_STATIC void j40__inverse_hornuss(float *buf) {
	float scratch[64];
	int32_t x, y, ix, iy;
	memcpy(scratch, buf, sizeof(float) * 64);
	j40__aux_inverse_dct11(scratch, buf, 0, 0, 1); // updates scratch[(0..1)*8+(0..1)]
	for (y = 0; y < 2; ++y) for (x = 0; x < 2; ++x) {
		int32_t pos00 = y * 8 + x, pos11 = (y + 2) * 8 + (x + 2);
		float rsum[4] = {}, sample11;
		for (iy = 0; iy < 4; ++iy) for (ix = 0; ix < 4; ++ix) {
			rsum[ix] += scratch[(y + iy * 2) * 8 + (x + ix * 2)];
		}
		// conceptually (SUM rsum[i]) = residual_sum + coefficients(x, y) in the spec
		sample11 = scratch[pos00] - (rsum[0] + rsum[1] + rsum[2] + rsum[3] - scratch[pos00]) * 0.0625f;
		scratch[pos00] = scratch[pos11];
		scratch[pos11] = 0.0f;
		for (iy = 0; iy < 4; ++iy) for (ix = 0; ix < 4; ++ix) {
			buf[(4 * y + iy) * 8 + (4 * x + ix)] = scratch[(y + iy * 2) * 8 + (x + ix * 2)] + sample11;
		}
	}
}

J40_STATIC void j40__inverse_dct32(float *buf) {
	float scratch[64], tmp;
	j40__view_f32 bufv = j40__make_view_f32(3, 3, buf);
	j40__view_f32 scratchv = j40__make_view_f32(3, 3, scratch);

	// coefficients form two 4 rows x 8 columns matrices from even and odd rows;
	// note that this is NOT 8 rows x 4 columns, because of transposition
	// TODO spec issue: inconsistent naming between coeffs_8x4 and coeffs_4x8
	tmp = *J40__AT(bufv, 0, 0) + *J40__AT(bufv, 0, 1);
	*J40__AT(bufv, 0, 1) = *J40__AT(bufv, 0, 0) - *J40__AT(bufv, 0, 1);
	*J40__AT(bufv, 0, 0) = tmp;
	j40__reshape_view_f32(&bufv, 4, 2);
	j40__inverse_dct_view(&scratchv, &bufv);
	j40__reshape_view_f32(&scratchv, 3, 3);
	j40__transpose_view_f32(&bufv, scratchv);
	j40__inverse_dct_view(&scratchv, &bufv);
	j40__oddeven_columns_to_halves_f32(&bufv, scratchv);
	J40__ASSERT(bufv.logw == 3 && bufv.logh == 3);
}

J40_STATIC void j40__inverse_dct23(float *buf) {
	float scratch[64];
	j40__view_f32 bufv = j40__make_view_f32(3, 3, buf);
	j40__view_f32 scratchv = j40__make_view_f32(3, 3, scratch);

	// coefficients form two 4 rows x 8 columns matrices from even and odd rows
	j40__copy_view_f32(&scratchv, bufv);
	*J40__AT(scratchv, 0, 0) = *J40__AT(bufv, 0, 0) + *J40__AT(bufv, 0, 1);
	*J40__AT(scratchv, 0, 1) = *J40__AT(bufv, 0, 0) - *J40__AT(bufv, 0, 1);
	j40__transpose_view_f32(&bufv, scratchv);
	j40__inverse_dct_view(&scratchv, &bufv);
	j40__transpose_view_f32(&bufv, scratchv);
	j40__reshape_view_f32(&bufv, 4, 2);
	j40__inverse_dct_view(&scratchv, &bufv);
	j40__reshape_view_f32(&scratchv, 3, 3);
	j40__oddeven_rows_to_halves_f32(&bufv, scratchv);
	J40__ASSERT(bufv.logw == 3 && bufv.logh == 3);
}

// TODO spec issue: the input is a 4x4 matrix but indexed like a 1-dimensional array
J40_STATIC void j40__inverse_afv22(float *J40_RESTRICT out, float *J40_RESTRICT in) {
	static const float AFV_BASIS[256] = { // AFVBasis in the specification, but transposed
		 0.25000000f,  0.87690293f,  0.00000000f,  0.00000000f,
		 0.00000000f, -0.41053776f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f,  0.22065181f,  0.00000000f,  0.00000000f,
		-0.70710678f,  0.62354854f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f, -0.10140050f,  0.40670076f, -0.21255748f,
		 0.00000000f, -0.06435072f, -0.45175566f, -0.30468475f,
		 0.30179295f,  0.40824829f,  0.17478670f, -0.21105601f,
		-0.14266085f, -0.13813540f, -0.17437603f,  0.11354987f,
		 0.25000000f, -0.10140050f,  0.44444817f,  0.30854971f,
		 0.00000000f, -0.06435072f,  0.15854504f,  0.51126161f,
		 0.25792363f,  0.00000000f,  0.08126112f,  0.18567181f,
		-0.34164468f,  0.33022826f,  0.07027907f, -0.07417505f,
		 0.25000000f,  0.22065181f,  0.00000000f,  0.00000000f,
		 0.70710678f,  0.62354854f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f, -0.10140050f,  0.00000000f,  0.47067023f,
		 0.00000000f, -0.06435072f, -0.04038515f,  0.00000000f,
		 0.16272340f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.73674975f,  0.08755115f, -0.29210266f,  0.19402893f,
		 0.25000000f, -0.10140050f,  0.19574399f, -0.16212052f,
		 0.00000000f, -0.06435072f,  0.00741823f, -0.29048013f,
		 0.09520023f,  0.00000000f, -0.36753980f,  0.49215859f,
		 0.24627108f, -0.07946707f,  0.36238173f, -0.43519050f,
		 0.25000000f, -0.10140050f,  0.29291001f,  0.00000000f,
		 0.00000000f, -0.06435072f,  0.39351034f, -0.06578702f,
		 0.00000000f, -0.40824829f, -0.30788221f, -0.38525014f,
		-0.08574019f, -0.46133749f,  0.00000000f,  0.21918685f,
		 0.25000000f, -0.10140050f, -0.40670076f, -0.21255748f,
		 0.00000000f, -0.06435072f, -0.45175566f,  0.30468475f,
		 0.30179295f, -0.40824829f, -0.17478670f,  0.21105601f,
		-0.14266085f, -0.13813540f, -0.17437603f,  0.11354987f,
		 0.25000000f, -0.10140050f, -0.19574399f, -0.16212052f,
		 0.00000000f, -0.06435072f,  0.00741823f,  0.29048013f,
		 0.09520023f,  0.00000000f,  0.36753980f, -0.49215859f,
		 0.24627108f, -0.07946707f,  0.36238173f, -0.43519050f,
		 0.25000000f, -0.10140050f,  0.00000000f, -0.47067023f,
		 0.00000000f, -0.06435072f,  0.11074166f,  0.00000000f,
		-0.16272340f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.14883399f,  0.49724647f,  0.29210266f,  0.55504438f,
		 0.25000000f, -0.10140050f,  0.11379074f, -0.14642919f,
		 0.00000000f, -0.06435072f,  0.08298163f, -0.23889774f,
		-0.35312385f, -0.40824829f,  0.48266891f,  0.17419413f,
		-0.04768680f,  0.12538059f, -0.43266080f, -0.25468277f,
		 0.25000000f, -0.10140050f, -0.44444817f,  0.30854971f,
		 0.00000000f, -0.06435072f,  0.15854504f, -0.51126161f,
		 0.25792363f,  0.00000000f, -0.08126112f, -0.18567181f,
		-0.34164468f,  0.33022826f,  0.07027907f, -0.07417505f,
		 0.25000000f, -0.10140050f, -0.29291001f,  0.00000000f,
		 0.00000000f, -0.06435072f,  0.39351034f,  0.06578702f,
		 0.00000000f,  0.40824829f,  0.30788221f,  0.38525014f,
		-0.08574019f, -0.46133749f,  0.00000000f,  0.21918685f,
		 0.25000000f, -0.10140050f, -0.11379074f, -0.14642919f,
		 0.00000000f, -0.06435072f,  0.08298163f,  0.23889774f,
		-0.35312385f,  0.40824829f, -0.48266891f, -0.17419413f,
		-0.04768680f,  0.12538059f, -0.43266080f, -0.25468277f,
		 0.25000000f, -0.10140050f,  0.00000000f,  0.42511496f,
		 0.00000000f, -0.06435072f, -0.45175566f,  0.00000000f,
		-0.60358590f,  0.00000000f,  0.00000000f,  0.00000000f,
		-0.14266085f, -0.13813540f,  0.34875205f,  0.11354987f,
	};

	int32_t i, j;
	for (i = 0; i < 16; ++i) {
		float sum = 0.0f;
		for (j = 0; j < 16; ++j) sum += in[j] * AFV_BASIS[i * 16 + j];
		out[i] = sum;
	}
}

J40_STATIC void j40__inverse_afv(float *buf, int flipx, int flipy) {
	// input          flipx/y=0/0     flipx/y=1/0     flipx/y=0/1     flipx/y=1/1
	//  _______       +-----+-----+   +-----+-----+   +-----------+   +-----------+
	// |_|_|_|_|      |'    |     |   |     |    '|   |           |   |           |
	// |_|_|_|_| ---> |AFV22|DCT22|   |DCT22|AFV22|   |   DCT23   |   |   DCT23   |
	// |_|_|_|_|      +-----+-----+   +-----+-----+   +-----+-----+   +-----+-----+
	// |_|_|_|_|      |   DCT23   |   |   DCT23   |   |AFV22|DCT22|   |DCT22|AFV22|
	//                |           |   |           |   |.    |     |   |     |    .|
	// (2x2 each)     +-----------+   +-----------+   +-----+-----+   +-----+-----+
	//
	// coefficients are divided by 16 2x2 blocks, where two top coefficients are for AFV22
	// and DCT22 respectively and two bottom coefficients are for DCT23.
	// all three corresponding DC coefficients are in the top-left block and handled specially.
	// AFV22 samples are then flipped so that the top-left cell is moved to the corner (dots above).
	//
	// TODO spec issue: identifiers have `*` in place of `x`

	float scratch[64];
	// buf23/buf32 etc. refer to the same memory region; numbers refer to the supposed dimensions
	float *bufafv = buf, *buf22 = buf + 16, *buf23 = buf + 32, *buf32 = buf23;
	float *scratchafv = scratch, *scratch22 = scratch + 16, *scratch23 = scratch + 32, *scratch32 = scratch23;
	int32_t x, y;

	J40__ASSERT(flipx == !!flipx && flipy == !!flipy);

	for (y = 0; y < 8; y += 2) for (x = 0; x < 8; ++x) {
		// AFV22 coefficients to scratch[0..16), DCT22 coefficients to scratch[16..32)
		scratch[(x % 2) * 16 + (y / 2) * 4 + (x / 2)] = buf[y * 8 + x];
	}
	for (y = 1; y < 8; y += 2) for (x = 0; x < 8; ++x) {
		// DCT23 coefficients to scratch[32..64) = scratch32[0..32), after transposition
		scratch32[x * 4 + (y / 2)] = buf[y * 8 + x];
	}
	scratchafv[0] = (buf[0] + buf[1] + buf[8]) * 4.0f;
	scratch22[0] = buf[0] - buf[1] + buf[8]; // TODO spec bug: x and y are swapped
	scratch32[0] = buf[0] - buf[8]; // TODO spec bug: x and y are swapped

	j40__inverse_afv22(bufafv, scratchafv);
	j40__inverse_dct(buf22, scratch22, 2, 4);
	j40__inverse_dct(buf32, scratch32, 3, 4);

	for (y = 0; y < 4; ++y) {
		for (x = 0; x < 4; ++x) scratchafv[y * 4 + x] = bufafv[y * 4 + x]; // AFV22, as is
		for (x = 0; x < 4; ++x) scratch22[x * 4 + y] = buf22[y * 4 + x]; // DCT22, transposed
	}
	for (y = 0; y < 8; ++y) {
		for (x = 0; x < 4; ++x) scratch23[x * 8 + y] = buf32[y * 4 + x]; // DCT23, transposed
	}

	j40__inverse_dct(buf22, scratch22, 2, 4);
	j40__inverse_dct(buf23, scratch23, 2, 8);
	memcpy(scratch + 16, buf + 16, sizeof(float) * 48);

	for (y = 0; y < 4; ++y) {
		static const int8_t FLIP_FOR_AFV[2][4] = {{0, 1, 2, 3}, {7, 6, 5, 4}};
		int32_t afv22pos = FLIP_FOR_AFV[flipy][y] * 8;
		int32_t dct22pos = (flipy * 4 + y) * 8 + (!flipx * 4);
		int32_t dct23pos = (!flipy * 4 + y) * 8;
		for (x = 0; x < 4; ++x) buf[afv22pos + FLIP_FOR_AFV[flipx][x]] = scratchafv[y * 4 + x];
		for (x = 0; x < 4; ++x) buf[dct22pos + x] = scratch22[y * 4 + x];
		// TODO spec issue: samples_4x4 should be samples_4x8
		for (x = 0; x < 8; ++x) buf[dct23pos + x] = scratch23[y * 8 + x];
	}
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// LfGlobal: additional image features, HF block context, global tree, extra channels

J40_STATIC J40_RETURNS_ERR j40__lf_global(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__lf_global(j40__st *st) {
	j40__frame_st *f = st->frame;
	int32_t sidx = 0;
	int32_t i, j;

	if (f->has_patches) J40__RAISE("TODO: patches");
	if (f->has_splines) J40__RAISE("TODO: splines");
	if (f->has_noise) J40__RAISE("TODO: noise");

	if (!j40__u(st, 1)) { // LfChannelDequantization.all_default
		// TODO spec bug: missing division by 128
		for (i = 0; i < 3; ++i) f->m_lf_scaled[i] = j40__f16(st) / 128.0f;
	}

	if (!f->is_modular) {
		f->global_scale = j40__u32(st, 1, 11, 2049, 11, 4097, 12, 8193, 16);
		f->quant_lf = j40__u32(st, 16, 0, 1, 5, 1, 8, 1, 16);

		// HF block context
		if (j40__u(st, 1)) {
			static const uint8_t DEFAULT_BLKCTX[] = {
				0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 6, 6, 6,
				7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
				7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
			};
			f->block_ctx_size = sizeof(DEFAULT_BLKCTX) / sizeof(*DEFAULT_BLKCTX);
			J40__SHOULD(f->block_ctx_map = malloc(sizeof(DEFAULT_BLKCTX)), "!mem");
			memcpy(f->block_ctx_map, DEFAULT_BLKCTX, sizeof(DEFAULT_BLKCTX));
			f->nb_qf_thr = f->nb_lf_thr[0] = f->nb_lf_thr[1] = f->nb_lf_thr[2] = 0; // SPEC is implicit
			f->nb_block_ctx = 15;
		} else {
			J40__RAISE_DELAYED();
			f->block_ctx_size = 39; // SPEC not 27
			for (i = 0; i < 3; ++i) {
				f->nb_lf_thr[i] = j40__u(st, 4);
				// TODO spec question: should this be sorted? (current code is okay with that)
				for (j = 0; j < f->nb_lf_thr[i]; ++j) {
					f->lf_thr[i][j] = j40__unpack_signed(j40__u32(st, 0, 4, 16, 8, 272, 16, 65808, 32));
				}
				f->block_ctx_size *= f->nb_lf_thr[i] + 1; // SPEC is off by one
			}
			f->nb_qf_thr = j40__u(st, 4);
			// TODO spec bug: both qf_thr[i] and HfMul should be incremented
			for (i = 0; i < f->nb_qf_thr; ++i) f->qf_thr[i] = j40__u32(st, 0, 2, 4, 3, 12, 5, 44, 8) + 1;
			f->block_ctx_size *= f->nb_qf_thr + 1; // SPEC is off by one
			// block_ctx_size <= 39*15^4 and never overflows
			J40__SHOULD(f->block_ctx_size <= 39 * 64, "hfbc"); // SPEC limit is not 21*64
			J40__SHOULD(f->block_ctx_map = malloc(sizeof(uint8_t) * (size_t) f->block_ctx_size), "!mem");
			J40__TRY(j40__cluster_map(st, f->block_ctx_size, 16, &f->nb_block_ctx, f->block_ctx_map));
		}

		if (!j40__u(st, 1)) { // LfChannelCorrelation.all_default
			f->inv_colour_factor = 1.0f / (float) j40__u32(st, 84, 0, 256, 0, 2, 8, 258, 16);
			f->base_corr_x = j40__f16(st);
			f->base_corr_b = j40__f16(st);
			f->x_factor_lf = j40__u(st, 8) - 127;
			f->b_factor_lf = j40__u(st, 8) - 127;
		}
	}

	if (j40__u(st, 1)) { // global tree present
		J40__TRY(j40__tree(st, &f->global_tree, &f->global_codespec));
	}
	J40__TRY(j40__init_modular_for_global(st, f->is_modular, f->do_ycbcr,
		f->log_upsampling, f->ec_log_upsampling, f->width, f->height, &f->gmodular));
	if (f->gmodular.num_channels > 0) {
		J40__TRY(j40__modular_header(st, f->global_tree, &f->global_codespec, &f->gmodular));
		J40__TRY(j40__allocate_modular(st, &f->gmodular));
		if (f->width <= (1 << f->group_size_shift) && f->height <= (1 << f->group_size_shift)) {
			f->num_gm_channels = f->gmodular.num_channels;
		} else {
			f->num_gm_channels = f->gmodular.nb_meta_channels;
		}
		for (i = 0; i < f->num_gm_channels; ++i) {
			J40__TRY(j40__modular_channel(st, &f->gmodular, i, sidx));
		}
		J40__TRY(j40__finish_and_free_code(st, &f->gmodular.code));
	} else {
		f->num_gm_channels = 0;
	}

J40__ON_ERROR:
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// LfGroup: downsampled LF image (optionally smoothed), varblock information

typedef struct {
	int32_t coeffoff_qfidx; // offset to coeffs (always a multiple of 64) | qf index (always < 16)
	int32_t hfmul_m1; // HfMul - 1 (to avoid overflow at this stage)
	// DctSelect is embedded in blocks
} j40__varblock;

typedef struct {
	int32_t idx;
	int32_t width, height; // <= 8192
	int32_t width8, height8; // <= 1024
	int32_t width64, height64; // <= 128
	int32_t nb_varblocks; // <= 2^20 (TODO spec issue: named nb_blocks)

	// these are either int16_t* or int32_t* depending on modular_16bit_buffers, aligned like PIXELS
	j40__plane xfromy, bfromy; // [width64*height64] each
	j40__plane sharpness; // [width8*height8]

	// bits 0..19: varblock index [0, nb_varblocks)
	// bits 20..24: DctSelect + 2, or 1 if not the top-left corner (0 is reserved for unused block)
	j40__plane blocks; // [width8*height8]
	j40__varblock *varblocks; // [nb_varblocks]

	float *llfcoeffs[3]; // [width8*height8] each
	float *coeffs[3]; // [width8*height8*64] each
	uint8_t coeffs_misalign[3];

	// precomputed lf_idx
	j40__plane lfindices; // [width8*height8]
} j40__lf_group_t;

J40_STATIC J40_RETURNS_ERR j40__lf_quant(
	j40__st *st, int32_t extra_prec, j40__modular_t *m, j40__lf_group_t *gg, j40__plane outlfquant[3]
);
J40_STATIC J40_RETURNS_ERR j40__hf_metadata(
	j40__st *st, int32_t nb_varblocks,
	j40__modular_t *m, const j40__plane lfquant[3], j40__lf_group_t *gg
);
J40_STATIC J40_RETURNS_ERR j40__lf_group(
	j40__st *st, int32_t ggw, int32_t ggh, int32_t ggidx, j40__lf_group_t *gg
);
J40_STATIC void j40__free_lf_group(j40__lf_group_t *gg);

// ----------------------------------------
// recursion for LF dequantization operations
#undef J40__RECURSING
#define J40__RECURSING 400
#define J40__P 16
#include J40_FILENAME
#define J40__P 32
#include J40_FILENAME
#undef J40__RECURSING
#define J40__RECURSING (-1)

#endif // J40__RECURSING < 0
#if J40__RECURSING == 400
	#define j40__intP_t J40__CONCAT3(int, J40__P, _t)
	#define J40__PIXELS J40__CONCAT3(J40__I, J40__P, _PIXELS)
// ----------------------------------------

#ifdef J40_IMPLEMENTATION

// out(x, y) = in(x, y) * mult (after type conversion)
J40_STATIC void j40__(dequant_lf,P)(const j40__plane *in, float mult, j40__plane *out) {
	int32_t x, y;
	J40__ASSERT(in->type == J40__(PLANE_I,P) && out->type == J40__PLANE_F32);
	J40__ASSERT(in->width <= out->width && in->height <= out->height);
	for (y = 0; y < in->height; ++y) {
		j40__intP_t *inpixels = J40__PIXELS(in, y);
		float *outpixels = J40__F32_PIXELS(out, y);
		for (x = 0; x < in->width; ++x) outpixels[x] = (float) inpixels[x] * mult;
	}
}

// plane(x, y) += # of lf_thr[i] s.t. in(x, y) > lf_thr[i]
J40_STATIC void j40__(add_thresholds,P)(
	j40__plane *plane, const j40__plane *in, const int32_t *lf_thr, int32_t nb_lf_thr
) {
	int32_t x, y, i;
	J40__ASSERT(in->type == J40__(PLANE_I,P) && plane->type == J40__PLANE_U8);
	J40__ASSERT(in->width <= plane->width && in->height <= plane->height);
	for (y = 0; y < plane->height; ++y) {
		j40__intP_t *inpixels = J40__PIXELS(in, y);
		uint8_t *pixels = J40__U8_PIXELS(plane, y);
		for (i = 0; i < nb_lf_thr; ++i) {
			int32_t threshold = lf_thr[i];
			for (x = 0; x < in->width; ++x) {
				pixels[x] = (uint8_t) (pixels[x] + (inpixels[x] > threshold));
			}
		}
	}
}

#endif // defined J40_IMPLEMENTATION

// ----------------------------------------
// end of recursion
	#undef j40__intP_t
	#undef J40__PIXELS
	#undef J40__P
#endif // J40__RECURSING == 400
#if J40__RECURSING < 0
// ----------------------------------------

#ifdef J40_IMPLEMENTATION

J40_ALWAYS_INLINE void j40__dequant_lf(const j40__plane *in, float mult, j40__plane *out) {
	switch (in->type) {
	case J40__PLANE_I16: j40__dequant_lf16(in, mult, out); break;
	case J40__PLANE_I32: j40__dequant_lf32(in, mult, out); break;
	default: J40__UNREACHABLE();
	}
}

J40_ALWAYS_INLINE void j40__add_thresholds(
	j40__plane *plane, const j40__plane *in, const int32_t *lf_thr, int32_t nb_lf_thr
) {
	switch (in->type) {
	case J40__PLANE_I16: return j40__add_thresholds16(plane, in, lf_thr, nb_lf_thr);
	case J40__PLANE_I32: return j40__add_thresholds32(plane, in, lf_thr, nb_lf_thr);
	default: J40__UNREACHABLE();
	}
}

J40_STATIC void j40__multiply_each_u8(j40__plane *plane, int32_t mult) {
	int32_t x, y;
	J40__ASSERT(plane->type == J40__PLANE_U8);
	for (y = 0; y < plane->height; ++y) {
		uint8_t *pixels = J40__U8_PIXELS(plane, y);
		for (x = 0; x < plane->width; ++x) pixels[x] = (uint8_t) (pixels[x] * mult);
	}
}

J40_STATIC J40_RETURNS_ERR j40__smooth_lf(j40__st *st, j40__lf_group_t *gg, j40__plane lfquant[3]) {
	static const float W0 = 0.05226273532324128f, W1 = 0.20345139757231578f, W2 = 0.0334829185968739f;

	j40__frame_st *f = st->frame;
	int32_t ggw8 = gg->width8, ggh8 = gg->height8;
	float *temp;
	float inv_m_lf[3];
	int32_t x, y, c;

	for (c = 0; c < 3; ++c) {
		// TODO spec bug: missing 2^16 scaling
		inv_m_lf[c] = (float) (f->global_scale * f->quant_lf) / f->m_lf_scaled[c] / 65536.0f;
	}

	J40__SHOULD(temp = malloc(sizeof(float) * (size_t) (ggw8 * 6)), "!mem");
	for (c = 0; c < 3; ++c) {
		memcpy(temp + c * ggw8, J40__F32_PIXELS(&lfquant[c], 0), sizeof(float) * (size_t) ggw8);
	}

	for (y = 1; y < ggh8 - 1; ++y) {
		float *nline[3], *line[3], *outline[3], *sline[3];
		for (c = 0; c < 3; ++c) {
			nline[c] = temp + (y & 1 ? c : c + 3) * ggw8;
			line[c] = temp + (y & 1 ? c + 3 : c) * ggw8;
			outline[c] = J40__F32_PIXELS(&lfquant[c], y);
			sline[c] = J40__F32_PIXELS(&lfquant[c], y + 1);
			memcpy(line[c], outline[c], sizeof(float) * (size_t) ggw8);
		}
		for (x = 1; x < ggw8 - 1; ++x) {
			float wa[3], diff[3], gap = 0.5f;
			for (c = 0; c < 3; ++c) {
				wa[c] = nline[c][x - 1] * W2 + nline[c][x] * W1 + nline[c][x + 1] * W2 +
					line[c][x - 1] * W1 + line[c][x] * W0 + line[c][x + 1] * W1 +
					sline[c][x - 1] * W2 + sline[c][x] * W1 + sline[c][x + 1] * W2;
				diff[c] = fabsf(wa[c] - line[c][x]) * inv_m_lf[c];
				if (gap < diff[c]) gap = diff[c];
			}
			gap = 3.0f - 4.0f * gap;
			if (gap < 0.0f) gap = 0.0f;
			// TODO spec bug: s (sample) and wa (weighted average) are swapped in the final formula
			for (c = 0; c < 3; ++c) outline[c][x] = (wa[c] - line[c][x]) * gap + line[c][x];
		}
	}

J40__ON_ERROR:
	free(temp);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__lf_quant(
	j40__st *st, int32_t extra_prec, j40__modular_t *m, j40__lf_group_t *gg, j40__plane outlfquant[3]
) {
	static const int32_t YXB2XYB[3] = {1, 0, 2}; // TODO spec bug: this reordering is missing

	j40__frame_st *f = st->frame;
	int32_t ggw8 = gg->width8, ggh8 = gg->height8;
	j40__plane *channel[3], lfquant[3] = {{0}}, lfindices = {0};
	int32_t c;

	J40__ASSERT(j40__plane_all_equal_sized(m->channel, m->channel + 3));

	for (c = 0; c < 3; ++c) J40__TRY(j40__init_plane(st, J40__PLANE_F32, ggw8, ggh8, &lfquant[c]));
	J40__TRY(j40__init_and_clear_plane(st, J40__PLANE_U8, ggw8, ggh8, &lfindices));

	// extract LfQuant from m and populate lfindices
	for (c = 0; c < 3; ++c) {
		// TODO spec bug: missing 2^16 scaling
		float mult_lf = f->m_lf_scaled[c] / (float) (f->global_scale * f->quant_lf) * (float) (65536 >> extra_prec);
		channel[c] = &m->channel[YXB2XYB[c]];
		j40__dequant_lf(channel[c], mult_lf, &lfquant[c]);
	}
	j40__add_thresholds(&lfindices, channel[0], f->lf_thr[0], f->nb_lf_thr[0]);
	j40__multiply_each_u8(&lfindices, f->nb_lf_thr[0] + 1);
	j40__add_thresholds(&lfindices, channel[2], f->lf_thr[2], f->nb_lf_thr[2]);
	j40__multiply_each_u8(&lfindices, f->nb_lf_thr[2] + 1);
	j40__add_thresholds(&lfindices, channel[1], f->lf_thr[1], f->nb_lf_thr[1]);

	// apply smoothing to LfQuant
	if (!f->skip_adapt_lf_smooth) J40__TRY(j40__smooth_lf(st, gg, lfquant));

	memcpy(outlfquant, lfquant, sizeof(j40__plane) * 3);
	gg->lfindices = lfindices;
	return 0;

J40__ON_ERROR:
	for (c = 0; c < 3; ++c) j40__free_plane(&lfquant[c]);
	j40__free_plane(&lfindices);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__hf_metadata(
	j40__st *st, int32_t nb_varblocks,
	j40__modular_t *m, const j40__plane lfquant[3], j40__lf_group_t *gg
) {
	j40__frame_st *f = st->frame;
	j40__plane blocks = {0};
	j40__varblock *varblocks = NULL;
	float *coeffs[3 /*xyb*/] = {NULL}, *llfcoeffs[3 /*xyb*/] = {NULL};
	size_t coeffs_misalign[3] = {0};
	int32_t log_gsize8 = f->group_size_shift - 3;
	int32_t ggw8 = gg->width8, ggh8 = gg->height8;
	int32_t voff, coeffoff;
	int32_t x0, y0, x1, y1, i, j, c;

	gg->xfromy = m->channel[0];
	gg->bfromy = m->channel[1];
	gg->sharpness = m->channel[3];
	memset(&m->channel[0], 0, sizeof(j40__plane));
	memset(&m->channel[1], 0, sizeof(j40__plane));
	memset(&m->channel[3], 0, sizeof(j40__plane));

	J40__TRY(j40__init_and_clear_plane(st, J40__PLANE_I32, ggw8, ggh8, &blocks));
	J40__SHOULD(varblocks = malloc(sizeof(j40__varblock) * (size_t) nb_varblocks), "!mem");
	for (c = 0; c < 3; ++c) { // TODO account for chroma subsampling
		J40__SHOULD(llfcoeffs[c] = malloc((size_t) (ggw8 * ggh8) * sizeof(float)), "!mem");
		J40__SHOULD(
			coeffs[c] = j40__alloc_aligned(
				sizeof(float) * (size_t) (ggw8 * ggh8 * 64), J40__PIXELS_ALIGN, &coeffs_misalign[c]),
			"!mem");
	}

	// temporarily use coeffoff_qfidx to store DctSelect
	// TODO spec issue: HfMul seems to be capped to [1, Quantizer::kQuantMax = 256] in libjxl
	if (m->channel[2].type == J40__PLANE_I16) {
		int16_t *blockinfo0 = J40__I16_PIXELS(&m->channel[2], 0);
		int16_t *blockinfo1 = J40__I16_PIXELS(&m->channel[2], 1);
		for (i = 0; i < nb_varblocks; ++i) {
			varblocks[i].coeffoff_qfidx = blockinfo0[i];
			varblocks[i].hfmul_m1 = blockinfo1[i];
		}
	} else {
		int32_t *blockinfo0 = J40__I32_PIXELS(&m->channel[2], 0);
		int32_t *blockinfo1 = J40__I32_PIXELS(&m->channel[2], 1);
		for (i = 0; i < nb_varblocks; ++i) {
			varblocks[i].coeffoff_qfidx = blockinfo0[i];
			varblocks[i].hfmul_m1 = blockinfo1[i];
		}
	}

	// place varblocks
	voff = coeffoff = 0;
	for (y0 = 0; y0 < ggh8; ++y0) for (x0 = 0; x0 < ggw8; ++x0) {
		int32_t dctsel, log_vh, log_vw, vh8, vw8;
		const j40__dct_select *dct;
		if (J40__I32_PIXELS(&blocks, y0)[x0]) continue;

		J40__SHOULD(voff < nb_varblocks, "vblk"); // TODO spec issue: missing
		dctsel = varblocks[voff].coeffoff_qfidx;
		J40__SHOULD(0 <= dctsel && dctsel < J40__NUM_DCT_SELECT, "dct?");
		dct = &J40__DCT_SELECT[dctsel];
		f->dct_select_used |= 1 << dctsel;
		f->order_used |= 1 << dct->order_idx;
		varblocks[voff].coeffoff_qfidx = coeffoff;
		J40__ASSERT(coeffoff % 64 == 0);

		log_vh = dct->log_rows;
		log_vw = dct->log_columns;
		J40__ASSERT(log_vh >= 3 && log_vw >= 3 && log_vh <= 8 && log_vw <= 8);
		vw8 = 1 << (log_vw - 3);
		vh8 = 1 << (log_vh - 3);
		x1 = x0 + vw8 - 1;
		y1 = y0 + vh8 - 1;
		// SPEC the first available block in raster order SHOULD be the top-left corner of
		// the next varblock, otherwise it's an error (no retry required)
		J40__SHOULD(x1 < ggw8 && (x0 >> log_gsize8) == (x1 >> log_gsize8), "vblk");
		J40__SHOULD(y1 < ggh8 && (y0 >> log_gsize8) == (y1 >> log_gsize8), "vblk");

		for (i = 0; i < vh8; ++i) {
			int32_t *blockrow = J40__I32_PIXELS(&blocks, y0 + i);
			for (j = 0; j < vw8; ++j) blockrow[x0 + j] = 1 << 20 | voff;
		}
		J40__I32_PIXELS(&blocks, y0)[x0] = (dctsel + 2) << 20 | voff;

		// compute LLF coefficients from dequantized LF
		if (log_vw <= 3 && log_vh <= 3) {
			for (c = 0; c < 3; ++c) llfcoeffs[c][coeffoff >> 6] = J40__F32_PIXELS(&lfquant[c], y0)[x0];
		} else {
			float scratch[1024]; // DCT256x256 requires 32x32
			for (c = 0; c < 3; ++c) {
				float *llfcoeffs_c = llfcoeffs[c] + (coeffoff >> 6);
				for (i = 0; i < vh8; ++i) {
					float *lfquantrow = J40__F32_PIXELS(&lfquant[c], y0 + i);
					for (j = 0; j < vw8; ++j) llfcoeffs_c[i * vw8 + j] = lfquantrow[x0 + j];
				}
				// TODO spec bug: DctSelect type IDENTIFY [sic] no longer exists
				// TODO spec issue: DCT8x8 doesn't need this
				j40__forward_dct2d_scaled_for_llf(llfcoeffs_c, scratch, log_vh - 3, log_vw - 3);
			}
		}

		coeffoff += 1 << (log_vw + log_vh);
		++voff;
	}
	J40__SHOULD(voff == nb_varblocks, "vblk"); // TODO spec issue: missing
	// TODO both libjxl and spec don't check for coeffoff == ggw8 * ggh8, but they probably should?

	// compute qf_idx for later use
	J40__ASSERT(f->nb_qf_thr < 16);
	for (j = 0; j < f->nb_qf_thr; ++j) {
		for (i = 0; i < nb_varblocks; ++i) {
			varblocks[i].coeffoff_qfidx += varblocks[i].hfmul_m1 >= f->qf_thr[j];
		}
	}

	gg->nb_varblocks = nb_varblocks;
	gg->blocks = blocks;
	gg->varblocks = varblocks;
	for (c = 0; c < 3; ++c) {
		gg->llfcoeffs[c] = llfcoeffs[c];
		gg->coeffs[c] = coeffs[c];
		gg->coeffs_misalign[c] = (uint8_t) coeffs_misalign[c];
	}
	return 0;

J40__ON_ERROR:
	j40__free_plane(&blocks);
	free(varblocks);
	for (c = 0; c < 3; ++c) {
		j40__free_aligned(coeffs[c], J40__PIXELS_ALIGN, coeffs_misalign[0]);
		free(llfcoeffs[c]);
	}
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__lf_group(
	j40__st *st, int32_t ggw, int32_t ggh, int32_t ggidx, j40__lf_group_t *gg
) {
	j40__frame_st *f = st->frame;
	int32_t sidx0 = 1 + ggidx, sidx1 = 1 + f->num_lf_groups + ggidx, sidx2 = 1 + 2 * f->num_lf_groups + ggidx;
	j40__plane lfquant[3] = {{0}};
	j40__modular_t m = {0};
	int32_t i, c;

	// TODO factor into j40__init_modular_for_lf_group
	for (i = f->num_gm_channels; i < f->gmodular.num_channels; ++i) {
		j40__plane *c = &f->gmodular.channel[i];
		if (c->hshift >= 3 && c->vshift >= 3) {
			(void) sidx1;
			J40__RAISE("TODO: ModularLfGroup decoding should continue here");
		}
	}

	if (!f->is_modular) {
		int32_t ggw8 = j40__ceil_div32(ggw, 8), ggh8 = j40__ceil_div32(ggh, 8);
		int32_t ggw64 = j40__ceil_div32(ggw, 64), ggh64 = j40__ceil_div32(ggh, 64);
		int32_t w[4], h[4], nb_varblocks;

		J40__ASSERT(gg);
		J40__ASSERT(ggw8 <= 1024 && ggh8 <= 1024);
		gg->width = ggw; gg->width8 = ggw8; gg->width64 = ggw64;
		gg->height = ggh; gg->height8 = ggh8; gg->height64 = ggh64;

		// LfQuant
		if (!f->use_lf_frame) {
			int32_t extra_prec = j40__u(st, 2);
			J40__SHOULD(f->jpeg_upsampling == 0, "TODO: subimage w/h depends on jpeg_upsampling");
			w[0] = w[1] = w[2] = ggw8;
			h[0] = h[1] = h[2] = ggh8;
			J40__TRY(j40__init_modular(st, 3, w, h, &m));
			J40__TRY(j40__modular_header(st, f->global_tree, &f->global_codespec, &m));
			J40__TRY(j40__allocate_modular(st, &m));
			for (c = 0; c < 3; ++c) J40__TRY(j40__modular_channel(st, &m, c, sidx0));
			J40__TRY(j40__finish_and_free_code(st, &m.code));
			J40__TRY(j40__inverse_transform(st, &m));
			// TODO spec issue: this modular image is independent of bpp/float_sample/etc.
			// TODO spec bug: channels are in the YXB order
			J40__TRY(j40__lf_quant(st, extra_prec, &m, gg, lfquant));
			j40__free_modular(&m);
		} else {
			J40__RAISE("TODO: persist lfquant and use it in later frames");
		}

		// HF metadata
		// SPEC nb_block is off by one
		nb_varblocks = j40__u(st, j40__ceil_lg32((uint32_t) (ggw8 * ggh8))) + 1; // at most 2^20
		w[0] = w[1] = ggw64; h[0] = h[1] = ggh64; // XFromY, BFromY
		w[2] = nb_varblocks; h[2] = 2; // BlockInfo
		w[3] = ggw8; h[3] = ggh8; // Sharpness
		J40__TRY(j40__init_modular(st, 4, w, h, &m));
		J40__TRY(j40__modular_header(st, f->global_tree, &f->global_codespec, &m));
		J40__TRY(j40__allocate_modular(st, &m));
		for (i = 0; i < 4; ++i) J40__TRY(j40__modular_channel(st, &m, i, sidx2));
		J40__TRY(j40__finish_and_free_code(st, &m.code));
		J40__TRY(j40__inverse_transform(st, &m));
		J40__TRY(j40__hf_metadata(st, nb_varblocks, &m, lfquant, gg));
		j40__free_modular(&m);
		for (i = 0; i < 3; ++i) j40__free_plane(&lfquant[i]);
	}

	return 0;

J40__ON_ERROR:
	j40__free_modular(&m);
	for (i = 0; i < 3; ++i) j40__free_plane(&lfquant[i]);
	if (gg) j40__free_lf_group(gg);
	return st->err;
}

J40_STATIC void j40__free_lf_group(j40__lf_group_t *gg) {
	int32_t i;
	for (i = 0; i < 3; ++i) {
		free(gg->llfcoeffs[i]);
		j40__free_aligned(gg->coeffs[i], J40__PIXELS_ALIGN, gg->coeffs_misalign[i]);
		gg->llfcoeffs[i] = NULL;
		gg->coeffs[i] = NULL;
	}
	j40__free_plane(&gg->xfromy);
	j40__free_plane(&gg->bfromy);
	j40__free_plane(&gg->sharpness);
	j40__free_plane(&gg->blocks);
	j40__free_plane(&gg->lfindices);
	free(gg->varblocks);
	gg->varblocks = NULL;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// HfGlobal and HfPass

J40_STATIC J40_RETURNS_ERR j40__hf_global(j40__st *st);

#ifdef J40_IMPLEMENTATION

// reads both HfGlobal and HfPass (SPEC they form a single group)
J40_STATIC J40_RETURNS_ERR j40__hf_global(j40__st *st) {
	j40__frame_st *f = st->frame;
	int32_t sidx_base = 1 + 3 * f->num_lf_groups;
	j40__code_spec_t codespec = {0};
	j40__code_t code = { .spec = &codespec };
	int32_t i, j, c;

	J40__ASSERT(!f->is_modular);

	// dequantization matrices
	if (!j40__u(st, 1)) {
		// TODO spec improvement: encoding mode 1..5 are only valid for 0-3/9-10 since it requires 8x8 matrix, explicitly note this
		for (i = 0; i < J40__NUM_DCT_PARAMS; ++i) { // SPEC not 11, should be 17
			const struct j40__dct_params dct = J40__DCT_PARAMS[i];
			int32_t rows = 1 << (int32_t) dct.log_rows, columns = 1 << (int32_t) dct.log_columns;
			J40__TRY(j40__dq_matrix(st, rows, columns, sidx_base + i, &f->dq_matrix[i]));
		}
	}

	// TODO is it possible that num_hf_presets > num_groups? otherwise j40__at_most is better
	f->num_hf_presets = j40__u(st, j40__ceil_lg32((uint32_t) f->num_groups)) + 1;
	J40__RAISE_DELAYED();

	// HfPass
	for (i = 0; i < f->num_passes; ++i) {
		int32_t used_orders = j40__u32(st, 0x5f, 0, 0x13, 0, 0, 0, 0, 13);
		if (used_orders > 0) J40__TRY(j40__code_spec(st, 8, &codespec));
		for (j = 0; j < J40__NUM_ORDERS; ++j) {
			if (used_orders >> j & 1) {
				int32_t size = 1 << (J40__LOG_ORDER_SIZE[j][0] + J40__LOG_ORDER_SIZE[j][1]);
				for (c = 0; c < 3; ++c) { // SPEC this loop is omitted
					J40__TRY(j40__permutation(st, &code, size, size / 64, &f->orders[i][j][c]));
				}
			}
		}
		if (used_orders > 0) {
			J40__TRY(j40__finish_and_free_code(st, &code));
			j40__free_code_spec(&codespec);
		}

		J40__TRY(j40__code_spec(st, 495 * f->nb_block_ctx * f->num_hf_presets, &f->coeff_codespec[i]));
	}

J40__ON_ERROR:
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// PassGroup

J40_STATIC J40_RETURNS_ERR j40__hf_coeffs(
	j40__st *st, int32_t ctxoff, int32_t pass,
	int32_t gx_in_gg, int32_t gy_in_gg, int32_t gw, int32_t gh, j40__lf_group_t *gg
);
J40_STATIC J40_RETURNS_ERR j40__pass_group(
	j40__st *st, int32_t pass, int32_t gx_in_gg, int32_t gy_in_gg, int32_t gw, int32_t gh, int32_t gidx,
	int32_t ggx, int32_t ggy, j40__lf_group_t *gg
);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__hf_coeffs(
	j40__st *st, int32_t ctxoff, int32_t pass,
	int32_t gx_in_gg, int32_t gy_in_gg, int32_t gw, int32_t gh, j40__lf_group_t *gg
) {
	const j40__frame_st *f = st->frame;
	int32_t gw8 = j40__ceil_div32(gw, 8), gh8 = j40__ceil_div32(gh, 8);
	int8_t (*nonzeros)[3] = NULL;
	j40__code_t code = { .spec = &f->coeff_codespec[pass] };
	int32_t lfidx_size = (f->nb_lf_thr[0] + 1) * (f->nb_lf_thr[1] + 1) * (f->nb_lf_thr[2] + 1);
	int32_t x8, y8, i, j, c_yxb;

	J40__ASSERT(gx_in_gg % 8 == 0 && gy_in_gg % 8 == 0);

	// TODO spec bug: there are *three* NonZeros for each channel
	J40__SHOULD(nonzeros = malloc(sizeof(int8_t[3]) * (size_t) (gw8 * gh8)), "!mem");

	for (y8 = 0; y8 < gh8; ++y8) for (x8 = 0; x8 < gw8; ++x8) {
		const j40__dct_select *dct;
		// TODO spec issue: missing x and y (here called x8 and y8)
		int32_t ggx8 = x8 + gx_in_gg / 8, ggy8 = y8 + gy_in_gg / 8, nzpos = y8 * gw8 + x8;
		int32_t voff = J40__I32_PIXELS(&gg->blocks, ggy8)[ggx8], dctsel = voff >> 20;
		int32_t log_rows, log_columns, log_size;
		int32_t coeffoff, qfidx, lfidx, bctx0, bctxc;

		if (dctsel < 2) continue; // not top-left block
		dctsel -= 2;
		voff &= 0xfffff;
		J40__ASSERT(dctsel < J40__NUM_DCT_SELECT);
		dct = &J40__DCT_SELECT[dctsel];
		log_rows = dct->log_rows;
		log_columns = dct->log_columns;
		log_size = log_rows + log_columns;

		coeffoff = gg->varblocks[voff].coeffoff_qfidx & ~15;
		qfidx = gg->varblocks[voff].coeffoff_qfidx & 15;
		// TODO spec improvement: explain why lf_idx is separately calculated
		// (answer: can be efficiently precomputed via vectorization)
		lfidx = J40__U8_PIXELS(&gg->lfindices, ggy8)[ggx8];
		bctx0 = (dct->order_idx * (f->nb_qf_thr + 1) + qfidx) * lfidx_size + lfidx;
		bctxc = 13 * (f->nb_qf_thr + 1) * lfidx_size;

		// unlike most places, this uses the YXB order
		for (c_yxb = 0; c_yxb < 3; ++c_yxb) {
			static const int32_t YXB2XYB[3] = {1, 0, 2};
			static const int8_t TWICE_COEFF_FREQ_CTX[64] = { // pre-multiplied by 2, [0] is unused
				-1,  0,  2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28,
				30, 30, 32, 32, 34, 34, 36, 36, 38, 38, 40, 40, 42, 42, 44, 44,
				46, 46, 46, 46, 48, 48, 48, 48, 50, 50, 50, 50, 52, 52, 52, 52,
				54, 54, 54, 54, 56, 56, 56, 56, 58, 58, 58, 58, 60, 60, 60, 60,
			};
			// TODO spec bug: CoeffNumNonzeroContext[9] should be 123, not 23
			static const int16_t TWICE_COEFF_NNZ_CTX[64] = { // pre-multiplied by 2
				  0,   0,  62, 124, 124, 186, 186, 186, 186, 246, 246, 246, 246, 304, 304, 304,
				304, 304, 304, 304, 304, 360, 360, 360, 360, 360, 360, 360, 360, 360, 360, 360,
				360, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412,
				412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412, 412,
			};

			int32_t c = YXB2XYB[c_yxb];
			float *coeffs = gg->coeffs[c] + coeffoff;
			int32_t *order = f->orders[pass][dct->order_idx][c];
			int32_t bctx = f->block_ctx_map[bctx0 + bctxc * c_yxb]; // BlockContext()
			int32_t nz, nzctx, cctx, qnz, prev;

			J40__ASSERT(order); // orders should have been already converted from Lehmer code

			// predict and read the number of non-zero coefficients
			nz = x8 > 0 ?
				(y8 > 0 ? (nonzeros[nzpos - 1][c] + nonzeros[nzpos - gw8][c] + 1) >> 1 : nonzeros[nzpos - 1][c]) :
				(y8 > 0 ? nonzeros[nzpos - gw8][c] : 32);
			// TODO spec improvement: `predicted` can never exceed 63 in NonZerosContext(),
			// so better to make it a normative assertion instead of clamping
			// TODO spec question: then why the predicted value of 64 is reserved in the contexts?
			J40__ASSERT(nz < 64);
			nzctx = ctxoff + bctx + (nz < 8 ? nz : 4 + nz / 2) * f->nb_block_ctx;
			nz = j40__code(st, nzctx, 0, &code);
			// TODO spec issue: missing
			J40__SHOULD(nz <= (63 << (log_size - 6)), "coef");

			qnz = j40__ceil_div32(nz, 1 << (log_size - 6)); // [0, 64)
			for (i = 0; i < (1 << (log_rows - 3)); ++i) {
				for (j = 0; j < (1 << (log_columns - 3)); ++j) {
					nonzeros[nzpos + i * gw8 + j][c] = (int8_t) qnz;
				}
			}
			cctx = ctxoff + 458 * bctx + 37 * f->nb_block_ctx;

			prev = (nz <= (1 << (log_size - 4))); // TODO spec bug: swapped condition
			// TODO spec issue: missing size (probably W*H)
			for (i = 1 << (log_size - 6); nz > 0 && i < (1 << log_size); ++i) {
				int32_t ctx = cctx +
					TWICE_COEFF_NNZ_CTX[j40__ceil_div32(nz, 1 << (log_size - 6))] +
					TWICE_COEFF_FREQ_CTX[i >> (log_size - 6)] + prev;
				// TODO spec question: can this overflow?
				// unlike modular there is no guarantee about "buffers" or anything similar here
				int32_t ucoeff = j40__code(st, ctx, 0, &code);
				// TODO int-to-float conversion, is it okay?
				coeffs[order[i]] += (float) j40__unpack_signed(ucoeff);
				// TODO spec issue: normative indicator has changed from [[...]] to a long comment
				nz -= prev = (ucoeff != 0);
			}
			J40__SHOULD(nz == 0, "coef"); // TODO spec issue: missing
		}
	}

	J40__TRY(j40__finish_and_free_code(st, &code));
	free(nonzeros);
	return 0;

J40__ON_ERROR:
	j40__free_code(&code);
	free(nonzeros);
	return st->err;
}

J40_STATIC J40_RETURNS_ERR j40__pass_group(
	j40__st *st, int32_t pass, int32_t gx_in_gg, int32_t gy_in_gg, int32_t gw, int32_t gh, int32_t gidx,
	int32_t ggx, int32_t ggy, j40__lf_group_t *gg
) {
	j40__frame_st *f = st->frame;
	// SPEC "the number of tables" is fixed, no matter how many RAW quant tables are there
	int32_t sidx = 1 + 3 * f->num_lf_groups + J40__NUM_DCT_PARAMS + pass * f->num_groups + gidx;
	j40__modular_t m = {0};
	int32_t i;

	if (!f->is_modular) {
		int32_t ctxoff;
		J40__ASSERT(gg);
		// TODO spec issue: this offset is later referred so should be monospaced
		ctxoff = 495 * f->nb_block_ctx * j40__u(st, j40__ceil_lg32((uint32_t) f->num_hf_presets));
		J40__TRY(j40__hf_coeffs(st, ctxoff, pass, gx_in_gg, gy_in_gg, gw, gh, gg));
	}

	J40__TRY(j40__init_modular_for_pass_group(st, f->num_gm_channels, gw, gh, 0, 3, &f->gmodular, &m));
	if (m.num_channels > 0) {
		J40__TRY(j40__modular_header(st, f->global_tree, &f->global_codespec, &m));
		J40__TRY(j40__allocate_modular(st, &m));
		for (i = 0; i < m.num_channels; ++i) {
			J40__TRY(j40__modular_channel(st, &m, i, sidx));
		}
		J40__TRY(j40__finish_and_free_code(st, &m.code));
		J40__TRY(j40__inverse_transform(st, &m));
		j40__combine_modular_from_pass_group(st, f->num_gm_channels,
			ggy + gy_in_gg, ggx + gx_in_gg, 0, 3, &f->gmodular, &m);
		j40__free_modular(&m);
	}

	return 0;

J40__ON_ERROR:
	j40__free_modular(&m);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// coefficients to samples

J40_STATIC void j40__dequant_hf(j40__st *st, j40__lf_group_t *gg);
J40_STATIC J40_RETURNS_ERR j40__combine_vardct_from_lf_group(
	j40__st *st, int32_t ggx, int32_t ggy, const j40__lf_group_t *gg
);

#ifdef J40_IMPLEMENTATION

J40_STATIC void j40__dequant_hf(j40__st *st, j40__lf_group_t *gg) {
	// QM_SCALE[i] = 0.8^(i - 2)
	static const float QM_SCALE[8] = {1.5625f, 1.25f, 1.0f, 0.8f, 0.64f, 0.512f, 0.4096f, 0.32768f};

	j40__frame_st *f = st->frame;
	int32_t ggw8 = gg->width8, ggh8 = gg->height8;
	float x_qm_scale, b_qm_scale, quant_bias_num = st->image->quant_bias_num, *quant_bias = st->image->quant_bias;
	int32_t x8, y8, c, i;

	J40__ASSERT(f->x_qm_scale >= 0 && f->x_qm_scale < 8);
	J40__ASSERT(f->b_qm_scale >= 0 && f->b_qm_scale < 8);
	x_qm_scale = QM_SCALE[f->x_qm_scale];
	b_qm_scale = QM_SCALE[f->b_qm_scale];

	for (y8 = 0; y8 < ggh8; ++y8) for (x8 = 0; x8 < ggw8; ++x8) {
		const j40__dct_select *dct;
		const j40__dq_matrix_t *dqmat;
		int32_t voff = J40__I32_PIXELS(&gg->blocks, y8)[x8], dctsel = voff >> 20, size;
		float mult[3 /*xyb*/];

		if (dctsel < 2) continue; // not top-left block
		voff &= 0xfffff;
		dct = &J40__DCT_SELECT[dctsel - 2];
		size = 1 << (dct->log_rows + dct->log_columns);
		// TODO spec bug: spec says mult[1] = HfMul, should be 2^16 / (global_scale * HfMul)
		mult[1] = 65536.0f / (float) f->global_scale / (float) (gg->varblocks[voff].hfmul_m1 + 1);
		mult[0] = mult[1] * x_qm_scale;
		mult[2] = mult[1] * b_qm_scale;
		dqmat = &f->dq_matrix[dct->param_idx];
		J40__ASSERT(dqmat->mode == J40__DQ_ENC_RAW); // should have been already loaded

		for (c = 0; c < 3; ++c) {
			float *coeffs = gg->coeffs[c] + (gg->varblocks[voff].coeffoff_qfidx & ~15);
			for (i = 0; i < size; ++i) { // LLF positions are left unused and can be clobbered
				// TODO spec issue: "quant" is a variable name and should be monospaced
				if (-1.0f <= coeffs[i] && coeffs[i] <= 1.0f) {
					coeffs[i] *= quant_bias[c]; // TODO coeffs[i] is integer at this point?
				} else {
					coeffs[i] -= quant_bias_num / coeffs[i];
				}
				coeffs[i] *= mult[c] / dqmat->params[i][c]; // TODO precompute this
			}
		}
	}
}

J40_STATIC J40_RETURNS_ERR j40__combine_vardct_from_lf_group(
	j40__st *st, int32_t ggx, int32_t ggy, const j40__lf_group_t *gg
) {
	j40__image_st *im = st->image;
	j40__frame_st *f = st->frame;
	int32_t ggw8 = gg->width8, ggh8 = gg->height8;
	int32_t ggw = gg->width, ggh = gg->height;
	float kx_lf, kb_lf, cbrt_opsin_bias[3 /*xyb*/];
	float *scratch = NULL, *scratch2, *samples[3] = {};
	int32_t x8, y8, x, y, i, c;

	for (c = 0; c < 3; ++c) {
		J40__SHOULD(samples[c] = malloc(sizeof(float) * (size_t) (ggw * ggh)), "!mem");
	}
	// TODO allocates the same amount of memory regardless of transformations used
	J40__SHOULD(scratch = malloc(sizeof(float) * 2 * 65536), "!mem");
	scratch2 = scratch + 65536;

	kx_lf = f->base_corr_x + (float) f->x_factor_lf * f->inv_colour_factor;
	kb_lf = f->base_corr_b + (float) f->b_factor_lf * f->inv_colour_factor;

	for (y8 = 0; y8 < ggh8; ++y8) for (x8 = 0; x8 < ggw8; ++x8) {
		const j40__dct_select *dct;
		int32_t voff = J40__I32_PIXELS(&gg->blocks, y8)[x8], dctsel = voff >> 20;
		int32_t size, effvw, effvh, vw8, vh8, samplepos;
		int32_t coeffoff;
		float *coeffs[3 /*xyb*/], *llfcoeffs[3 /*xyb*/], kx_hf, kb_hf;

		if (dctsel < 2) continue; // not top-left block
		dctsel -= 2;
		voff &= 0xfffff;
		dct = &J40__DCT_SELECT[dctsel];
		size = 1 << (dct->log_rows + dct->log_columns);
		coeffoff = gg->varblocks[voff].coeffoff_qfidx & ~15;
		for (c = 0; c < 3; ++c) {
			coeffs[c] = gg->coeffs[c] + coeffoff;
			llfcoeffs[c] = gg->llfcoeffs[c] + (coeffoff >> 6);
		}

		// TODO spec bug: x_factor and b_factor (for HF) is constant in the same varblock,
		// even when the varblock spans multiple 64x64 rectangles
		kx_hf = f->base_corr_x + f->inv_colour_factor * (gg->xfromy.type == J40__PLANE_I16 ?
			(float) J40__I16_PIXELS(&gg->xfromy, y8 / 8)[x8 / 8] :
			(float) J40__I32_PIXELS(&gg->xfromy, y8 / 8)[x8 / 8]);
		kb_hf = f->base_corr_b + f->inv_colour_factor * (gg->bfromy.type == J40__PLANE_I16 ?
			(float) J40__I16_PIXELS(&gg->bfromy, y8 / 8)[x8 / 8] :
			(float) J40__I32_PIXELS(&gg->bfromy, y8 / 8)[x8 / 8]);

		effvh = j40__min32(ggh - y8 * 8, 1 << dct->log_rows);
		effvw = j40__min32(ggw - x8 * 8, 1 << dct->log_columns);
		samplepos = (y8 * 8) * ggw + (x8 * 8);
		// this is for LLF coefficients, which may have been transposed
		vh8 = 1 << (j40__min32(dct->log_rows, dct->log_columns) - 3);
		vw8 = 1 << (j40__max32(dct->log_rows, dct->log_columns) - 3);

		for (c = 0; c < 3; ++c) {
			// chroma from luma (CfL), overwrite LLF coefficients on the way
			// TODO skip CfL if there's subsampled channel
			switch (c) {
			case 0: // X
				for (i = 0; i < size; ++i) scratch[i] = coeffs[0][i] + coeffs[1][i] * kx_hf;
				for (y = 0; y < vh8; ++y) for (x = 0; x < vw8; ++x) {
					scratch[y * vw8 * 8 + x] = llfcoeffs[0][y * vw8 + x] + llfcoeffs[1][y * vw8 + x] * kx_lf;
				}
				break;
			case 1: // Y
				for (i = 0; i < size; ++i) scratch[i] = coeffs[1][i];
				for (y = 0; y < vh8; ++y) for (x = 0; x < vw8; ++x) {
					scratch[y * vw8 * 8 + x] = llfcoeffs[1][y * vw8 + x];
				}
				break;
			case 2: // B
				for (i = 0; i < size; ++i) scratch[i] = coeffs[2][i] + coeffs[1][i] * kb_hf;
				for (y = 0; y < vh8; ++y) for (x = 0; x < vw8; ++x) {
					scratch[y * vw8 * 8 + x] = llfcoeffs[2][y * vw8 + x] + llfcoeffs[1][y * vw8 + x] * kb_lf;
				}
				break;
			default: J40__UNREACHABLE();
			}

			// inverse DCT
			switch (dctsel) {
			case 1: j40__inverse_hornuss(scratch); break; // Hornuss
			case 2: j40__inverse_dct11(scratch); break; // DCT11
			case 3: j40__inverse_dct22(scratch); break; // DCT22
			case 12: j40__inverse_dct23(scratch); break; // DCT23
			case 13: j40__inverse_dct32(scratch); break; // DCT32
			case 14: j40__inverse_afv(scratch, 0, 0); break; // AFV0
			case 15: j40__inverse_afv(scratch, 1, 0); break; // AFV1
			case 16: j40__inverse_afv(scratch, 0, 1); break; // AFV2
			case 17: j40__inverse_afv(scratch, 1, 1); break; // AFV3
			default: // every other DCTnm where n, m >= 3
				j40__inverse_dct2d(scratch, scratch2, dct->log_rows, dct->log_columns);
				break;
			}

			if (0) { // TODO display borders for the debugging
				for (x = 0; x < (1<<dct->log_columns); ++x) scratch[x] = 1.0f - (float) ((dctsel >> x) & 1);
				for (y = 0; y < (1<<dct->log_rows); ++y) scratch[y << dct->log_columns] = 1.0f - (float) ((dctsel >> y) & 1);
			}

			// reposition samples into the rectangular grid
			// TODO spec issue: overflown samples (due to non-8n dimensions) are probably ignored
			for (y = 0; y < effvh; ++y) for (x = 0; x < effvw; ++x) {
				samples[c][samplepos + y * ggw + x] = scratch[y << dct->log_columns | x];
			}
		}
	}

	// coeffs is now correctly positioned, copy to the modular buffer
	for (c = 0; c < 3; ++c) cbrt_opsin_bias[c] = cbrtf(im->opsin_bias[c]);
	for (y = 0; y < ggh; ++y) for (x = 0; x < ggw; ++x) {
		int32_t pos = y * ggw + x;
		float p[3] = {
			samples[1][pos] + samples[0][pos],
			samples[1][pos] - samples[0][pos],
			samples[2][pos],
		};
		float itscale = 255.0f / im->intensity_target;
		for (c = 0; c < 3; ++c) {
			float pp = p[c] - cbrt_opsin_bias[c];
			samples[c][pos] = (pp * pp * pp + im->opsin_bias[c]) * itscale;
		}
	}
	for (c = 0; c < 3; ++c) {
		if (f->gmodular.channel[c].type == J40__PLANE_I16) {
			for (y = 0; y < ggh; ++y) {
				int16_t *pixels = J40__I16_PIXELS(&f->gmodular.channel[c], ggy + y);
				for (x = 0; x < ggw; ++x) {
					int32_t p = y * ggw + x;
					float v = 
						samples[0][p] * im->opsin_inv_mat[c][0] +
						samples[1][p] * im->opsin_inv_mat[c][1] +
						samples[2][p] * im->opsin_inv_mat[c][2];
					v = (v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f); // to sRGB
					// TODO overflow check
					pixels[ggx + x] = (int16_t) ((float) ((1 << im->bpp) - 1) * v + 0.5f);
				}
			}
		} else {
			J40__RAISE("TODO: don't keep this here");
		}
	}

J40__ON_ERROR:
	free(scratch);
	for (c = 0; c < 3; ++c) free(samples[c]);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
// frame

J40_STATIC J40_RETURNS_ERR j40__frame(j40__st *st);

#ifdef J40_IMPLEMENTATION

J40_STATIC J40_RETURNS_ERR j40__frame(j40__st *st) {
	j40__frame_st *f = st->frame;
	j40__lf_group_t *gg = NULL;
	int32_t i, j, c;
	int single_section;

	J40__TRY(j40__frame_header(st));
	J40__TRY(j40__toc(st));
	// TODO subsequent groups can be reordered and should have separate bit readers

	// TODO spec issue: if there is a single section, there is no zero padding after the TOC
	single_section = f->num_passes == 1 && f->num_groups == 1;

	J40__TRY(j40__lf_global(st));
	if (!single_section) J40__TRY(j40__zero_pad_to_byte(st));

	// LfGroups
	{
		int32_t ggsize = 8 << f->group_size_shift;
		int32_t ggx, ggy, ggidx = 0;
		J40__SHOULD(gg = calloc((size_t) f->num_lf_groups, sizeof(j40__lf_group_t)), "!mem");
		for (ggy = 0; ggy < f->height; ggy += ggsize) {
			int32_t ggh = j40__min32(ggsize, f->height - ggy);
			for (ggx = 0; ggx < f->width; ggx += ggsize, ++ggidx) {
				int32_t ggw = j40__min32(ggsize, f->width - ggx);
				gg->idx = ggidx;
				printf("lf group ggy %d ggx %d ggidx %d\n", ggy, ggx, ggidx); fflush(stdout);
				J40__TRY(j40__lf_group(st, ggw, ggh, ggidx, f->is_modular ? NULL : &gg[ggidx]));
				if (!single_section) J40__TRY(j40__zero_pad_to_byte(st));
			}
		}
	}

	if (!f->is_modular) {
		J40__TRY(j40__hf_global(st));
		if (!single_section) J40__TRY(j40__zero_pad_to_byte(st));
	}

	// ensure all needed dequantization matrices loaded
	for (j = 0; j < J40__NUM_DCT_SELECT; ++j) {
		if (f->dct_select_used >> j & 1) {
			const j40__dct_select *dct = &J40__DCT_SELECT[j];
			int32_t param_idx = dct->param_idx;
			J40__TRY(j40__load_dq_matrix(st, param_idx, &f->dq_matrix[param_idx]));
		}
	}

	// PassGroups
	for (i = 0; i < f->num_passes; ++i) {
		int32_t gsize = 1 << f->group_size_shift, log_ggsize = 3 + f->group_size_shift;
		int32_t gx, gy, gidx = 0;

		if (i > 0) J40__RAISE("TODO: more passes");

		// compute all needed coefficient orders and discard others
		for (j = 0; j < J40__NUM_ORDERS; ++j) {
			if (f->order_used >> j & 1) {
				int32_t log_rows = J40__LOG_ORDER_SIZE[j][0];
				int32_t log_columns = J40__LOG_ORDER_SIZE[j][1];
				int32_t *order, temp, skip = 1 << (log_rows + log_columns - 6);
				for (c = 0; c < 3; ++c) {
					J40__TRY(j40__natural_order(st, log_rows, log_columns, &order));
					j40__apply_permutation(order + skip, &temp, sizeof(int32_t), f->orders[i][j][c]);
					free(f->orders[i][j][c]);
					f->orders[i][j][c] = order;
				}
			} else {
				for (c = 0; c < 3; ++c) {
					free(f->orders[i][j][c]);
					f->orders[i][j][c] = NULL;
				}
			}
		}

		for (gy = 0; gy < f->height; gy += gsize) {
			int32_t gh = j40__min32(gsize, f->height - gy);
			int32_t ggy = gy >> log_ggsize << log_ggsize;
			int32_t ggrowidx = (gy >> log_ggsize) * f->num_lf_groups_per_row;
			for (gx = 0; gx < f->width; gx += gsize, ++gidx) {
				int32_t gw = j40__min32(gsize, f->width - gx);
				int32_t ggx = gx >> log_ggsize << log_ggsize;
				int32_t ggidx = ggrowidx + (gx >> log_ggsize);
				printf("pass %d gy %d gx %d gw %d gh %d ggy %d ggx %d ggidx %d\n", i, gy, gx, gw, gh, ggy, ggx, ggidx); fflush(stdout);
				J40__TRY(j40__pass_group(st, i, gx - ggx, gy - ggy, gw, gh, gidx, ggx, ggy,
					f->is_modular ? NULL : &gg[ggidx]));
				if (!single_section) J40__TRY(j40__zero_pad_to_byte(st));
			}
		}
	}

	J40__TRY(j40__zero_pad_to_byte(st)); // frame boundary is always byte-aligned
	J40__TRY(j40__inverse_transform(st, &f->gmodular));

	// render the LF group into modular buffers
	if (!f->is_modular) {
		int32_t ggsize = 8 << f->group_size_shift, ggx, ggy, ggidx = 0;

		// TODO pretty incorrect to do this
		J40__SHOULD(!f->do_ycbcr && st->image->cspace != J40__CS_GREY, "TODO: we don't yet do YCbCr or gray");
		J40__SHOULD(st->image->modular_16bit_buffers, "TODO: !modular_16bit_buffers");
		f->gmodular.num_channels = 3;
		J40__SHOULD(f->gmodular.channel = calloc(3, sizeof(j40__plane)), "!mem");
		for (i = 0; i < f->gmodular.num_channels; ++i) {
			J40__TRY(j40__init_plane(st, J40__PLANE_I16, f->width, f->height, &f->gmodular.channel[i]));
		}
		for (ggy = 0; ggy < f->height; ggy += ggsize) {
			for (ggx = 0; ggx < f->width; ggx += ggsize, ++ggidx) {
				j40__dequant_hf(st, &gg[ggidx]);
				J40__TRY(j40__combine_vardct_from_lf_group(st, ggx, ggy, &gg[ggidx]));
			}
		}
	}

J40__ON_ERROR:
	if (gg) for (i = 0; i < f->num_lf_groups; ++i) j40__free_lf_group(&gg[i]);
	free(gg);
	return st->err;
}

#endif // defined J40_IMPLEMENTATION

////////////////////////////////////////////////////////////////////////////////
#endif // J40__RECURSING < 0                       // internal code ends here //
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// public API, continued

#if J40__RECURSING <= 0

const char *dumppath;
void end(const j40__st st);

j40_err j40_from_file(const char *path);

#ifdef J40_IMPLEMENTATION

const char *dumppath = NULL;
void end(const j40__st st) { (void) st; }

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

j40_err j40_from_file(const char *path) {
	j40__source src = {0};
	j40__container_st container = {0};
	j40__image_st im = {
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
	j40__st st_ = { .source = &src, .container = &container, .image = &im };
	j40__st *st = &st_;
	int32_t i, j, k;

	J40__TRY(j40__init_file_source(st, path, 8 * 1024 * 1024, st->source));
	J40__TRY(j40__refill_backing_buffer(st));

	J40__TRY(j40__container(st));

	J40__SHOULD(st->remaining >= 2, "shrt");
	J40__SHOULD(st->ptr[0] == 0xff && st->ptr[1] == 0x0a, "!jxl");
	st->ptr += 2;
	st->remaining -= 2;

	J40__TRY(j40__size_header(st, &im.width, &im.height));
	J40__TRY(j40__image_metadata(st));
	if (im.want_icc) J40__TRY(j40__icc(st));

	j40__frame_st frame;
	do {
		st->frame = &frame;
		J40__TRY(j40__frame(st));
		printf("%sframe: %s, %d+%d+%dx%d\n", frame.is_last ? "last " : "",
			(char*[]){"regular", "lf", "refonly", "regular-but-skip-progressive"}[frame.type],
			frame.x0, frame.y0, frame.width, frame.height);
		fflush(stdout);

		if (dumppath && frame.type == J40__FRAME_REGULAR) {
			J40__ASSERT(im.modular_16bit_buffers && im.bpp >= 8 && im.exp_bits == 0);
			FILE *f = fopen(dumppath, "wb");
			uint32_t crc, adler, unused = 0, idatsize;
			j40__plane *c[4];
			int32_t nchan;
			uint8_t buf[32];
			nchan = (!frame.do_ycbcr && !im.xyb_encoded && im.cspace == J40__CS_GREY ? 1 : 3);
			for (i = 0; i < nchan; ++i) c[i] = &frame.gmodular.channel[i];
			for (i = nchan; i < frame.gmodular.num_channels; ++i) {
				j40__ec_info *ec = &im.ec_info[i - nchan];
				if (ec->type == J40__EC_ALPHA) {
					J40__ASSERT(ec->bpp == im.bpp && ec->exp_bits == im.exp_bits);
					J40__ASSERT(ec->dim_shift == 0 && !ec->data.alpha_associated);
					c[nchan++] = &frame.gmodular.channel[i];
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
						int32_t p = j40__min32(j40__max32(0, J40__I16_PIXELS(c[k], i)[j]), (1 << im.bpp) - 1);
						update_cksum(buf[k] = (uint8_t) (p * 255 / ((1 << im.bpp) - 1)), &crc, &adler);
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

	end(st_);
	return 0;

J40__ON_ERROR:
	return st->err;
}

#endif // defined J40_IMPLEMENTATION
#endif // J40__RECURSING <= 0

////////////////////////////////////////////////////////////////////////////////

#if J40__RECURSING <= 0

#ifdef __cplusplus
}
#endif

// prevents double `#include`s---we can't really use `#pragma once` or simple `#ifndef` guards...
#undef J40__RECURSING
#define J40__RECURSING 9999

#endif // J40__RECURSING <= 0

// vim: noet ts=4 st=4 sts=4 sw=4 list colorcolumn=100