#pragma once

#ifdef __has_include
    #if __has_include(<stdbit.h>)
        #define HAS_STDBIT
#endif

#ifdef HAS_STDBIT
#include <stdbit.h>

#define COUNTR_ZERO(x) stdc_trailing_zeros_ul(x) // count trailing zeros
#define COUNTR_ONE(x)  stdc_trailing_ones_ul(x)  // count trailing ones
#define COUNTL_ZERO(x) stdc_leading_zeros_ul(x)  // count leading zeros
#define COUNTL_ONE(x)  stdc_leading_ones_ul(x)   // count leading ones
#else
#define COUNTR_ZERO(x) __builtin_ctzl(x)    // count trailing zeros
#define COUNTR_ONE(x)  __builtin_ctzl(~(x)) // count trailing ones
#define COUNTL_ZERO(x) __builtin_clzl(x)    // count leading zeros
#define COUNTL_ONE(x)  __builtin_clzl(~(x)) // count leading ones
#endif

#define LIKELY(x)   __builtin_expect(!!(x), 1) // !! to convert to boolean
#define UNLIKELY(x) __builtin_expect(!!(x), 0) // !! to convert to boolean
