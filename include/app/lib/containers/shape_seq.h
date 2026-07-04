/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_CONTAINERS_SHAPE_SEQ_H_
#define APP_LIB_CONTAINERS_SHAPE_SEQ_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_shape_seq Sequence shape (seq_ops)
 * @ingroup lib
 * @{
 *
 * @brief The abstract Sequence contract (L2) provided by seq.
 *
 * SKELETON — see doc/layers/l2_containers.md. Not yet implemented.
 */

/** A sequence instance is a root i-node id plus its provider's op vector. */
struct seq_ops {
	int (*len)(uint64_t root, size_t *out_len);
	int (*read)(uint64_t root, size_t off, void *out, size_t len,
		    size_t *out_read);
	int (*append)(uint64_t root, const void *data, size_t len);
	int (*truncate)(uint64_t root, size_t len);
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_CONTAINERS_SHAPE_SEQ_H_ */
