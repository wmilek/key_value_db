#ifndef STUB_UTIL_H
#define STUB_UTIL_H
#include <zephyr/toolchain.h>
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define ROUND_UP(x, align) ((((x) + ((align) - 1)) / (align)) * (align))
#define IS_ENABLED(cfg) 1
#define __ASSERT(test, ...) ((void)0)
#endif
