#pragma once

#define LIKELY(x) __builtin_expect(!!(x), 1)   // !! to convert to boolean
#define UNLIKELY(x) __builtin_expect(!!(x), 0) // !! to convert to boolean

#define COUNTR_ZERO(x) __builtin_ctz(x) // count trailing zeros
#define COUNTR_ONE(x)  __builtin_ctzll(x) // count trailing ones
#define COUNTL_ZERO(x) __builtin_clz(x) // count leading zeros
#define COUNTL_ONE(x)  __builtin_clzll(x) // count leading ones
