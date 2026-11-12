#ifndef STUB_TOOLCHAIN_H
#define STUB_TOOLCHAIN_H
#define __packed __attribute__((packed))
#define BUILD_ASSERT(cond, ...) _Static_assert(cond, "" __VA_ARGS__)
#define ARG_UNUSED(x) ((void)(x))
#endif
