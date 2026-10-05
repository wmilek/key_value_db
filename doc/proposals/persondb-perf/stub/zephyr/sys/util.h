#ifndef STUB_UTIL_H
#define STUB_UTIL_H
#include <zephyr/toolchain.h>
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define ROUND_UP(x, align) ((((x) + ((align) - 1)) / (align)) * (align))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define __ASSERT(test, ...) ((void)0)
#define __ASSERT_NO_MSG(test) ((void)0)
/* Zephyr's real IS_ENABLED: true only for macros defined to 1. */
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(_ZZZZ##config_macro)
#define _ZZZZ1 _YYYY,
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...) val
#endif
