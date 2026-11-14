/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_diag structure recognition: what a blob is, judged from its bytes.
 *
 * The formats are the ones the libraries write, copied here on purpose
 * together with the layouts earlier builds wrote, so that an image from an
 * older firmware is still named and decoded. Nothing in this directory is
 * used by the libraries, and nothing here calls into a container: the only
 * dependency is the public blob_db API (plus the inspect API to enumerate).
 *
 * Every judgement is a guess from the bytes. A magic and a length that agree
 * with it make a strong guess; a layout without a magic (a kvhash bucket, a
 * logring entry) is a weak one, reported as "shape". A file body or a spilled
 * value can hold any bytes, including another structure's magic.
 */

#ifndef APP_DIAG_DIAG_H_
#define APP_DIAG_DIAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct shell;

/** What a blob was recognised as. The order is the order of the summary. */
enum diag_type {
	DIAG_T_RREG,          /**< root registry (id 1)                       */
	DIAG_T_KVDB_META,     /**< kvdb instance meta                         */
	DIAG_T_BLOBFS_META,   /**< blobfs file-system meta                    */
	DIAG_T_PADB_SB,       /**< app_cbor_persondb superblock (CBOR)        */
	DIAG_T_KVHASH_DIR,    /**< kvhash directory (map root or sub-map)     */
	DIAG_T_CLOG_ROOT,     /**< logring: root i-node or whole ring         */
	DIAG_T_KVHASH_BUCKET, /**< kvhash bucket (shape only)                 */
	DIAG_T_CLOG_ENTRY,    /**< logring chain entry (shape only)           */
	DIAG_T_CBOR,          /**< one well-formed CBOR item (shape only)     */
	DIAG_T_EMPTY,         /**< zero-length payload                        */
	DIAG_T_UNKNOWN,       /**< none of the above                          */
	DIAG_T_COUNT,
};

/** Most variants any one type has; see diag_type_info(). */
#define DIAG_VARIANTS_MAX 4

/** The verdict on one blob. */
struct diag_class {
	enum diag_type type;
	uint8_t variant;    /**< index into the type's variant names     */
	bool    shape;      /**< no magic: recognised by layout alone    */
	bool    suspect;    /**< magic matches, the rest does not quite  */
	const char *note;   /**< static text: why suspect, or a state    */
};

/** Static description of a type, for summaries and `diag formats`. */
struct diag_type_info {
	const char *name;                             /**< short tag    */
	const char *what;                             /**< one line     */
	const char *variant[DIAG_VARIANTS_MAX];       /**< NULL-ended   */
};

const struct diag_type_info *diag_type_info(enum diag_type t);

/** Look a type up by its short tag (for `diag show <id> <type>`). */
int diag_type_by_name(const char *name, enum diag_type *out);

/**
 * @brief Classify a blob.
 *
 * @param id    the blob's id (a logring entry links forward, to a larger id)
 * @param buf   the payload, or its first @p n bytes
 * @param n     bytes in @p buf
 * @param size  the blob's full length; > @p n when only a prefix was read,
 *              in which case the shape-only checks, which need the whole
 *              payload, are skipped
 */
void diag_classify(uint64_t id, const uint8_t *buf, size_t n, size_t size,
		   struct diag_class *out);

/**
 * @brief Classify a blob as type @p t whatever its bytes say, for decoding
 *        a blob without a magic (a kvhash bucket, a logring entry) or one
 *        the classifier got wrong.
 *
 * @retval 0         @p out filled; marked suspect if @p t is not what
 *                   diag_classify() would have said
 * @retval -EMSGSIZE too short to hold a @p t header
 */
int diag_force(uint64_t id, enum diag_type t, const uint8_t *buf, size_t n,
	       size_t size, struct diag_class *out);

/**
 * @brief One line: the id, type, variant and the fields that identify the
 *        instance, after @p prefix (indentation, a label) and before
 *        @p suffix (who references it); either may be "".
 */
void diag_summary(const struct shell *sh, const char *prefix, uint64_t id,
		  const struct diag_class *c, const uint8_t *buf, size_t n,
		  size_t size, const char *suffix);

/**
 * @brief Decode a blob in full, as type @p c->type, variant @p c->variant.
 *
 * Every id the structure references is printed, so that the next
 * `diag show` can follow it.
 */
void diag_decode(const struct shell *sh, uint64_t id,
		 const struct diag_class *c, const uint8_t *buf, size_t n,
		 size_t size);

/** One id a structure's head points at. */
struct diag_ref {
	uint64_t id;
	const char *label;  /**< static: what the field is ("struct_root") */
};

/**
 * @brief The ids a structure head references, as far as one blob tells:
 *        a kvdb meta's map, a blobfs meta's root directory, a persondb
 *        superblock's tables, a logring root's head. A kvhash directory's
 *        children are not listed here (they can be hundreds); `diag show`
 *        prints them.
 *
 * @return number of refs written to @p out (at most @p max)
 */
int diag_refs(const struct diag_class *c, const uint8_t *buf, size_t n,
	      struct diag_ref *out, int max);

/* ---- root registry keys --------------------------------------------- */

/** Name of a registry key's FourCC when it is one this tool knows, or NULL. */
const char *diag_rreg_key_name(uint64_t key);

/**
 * @brief Walk a root registry image.
 * @retval 0       walked
 * @retval -EINVAL not a registry this tool reads
 */
int diag_rreg_foreach(const uint8_t *buf, size_t n,
		      void (*cb)(uint64_t key, uint64_t root, void *user),
		      void *user);

/* ---- CBOR (diag_cbor.c) ---------------------------------------------- */

/** Length of the one well-formed item at @p p, or a negative errno. */
int diag_cbor_item_len(const uint8_t *p, size_t n, size_t *len);

/**
 * @brief Format the item at @p p in CBOR diagnostic notation, compactly
 *        (`{1: 2, 2: [5, 6], 3: "ab"}`), into @p out. Long output ends in
 *        "...".
 * @return bytes of input consumed, or a negative errno
 */
int diag_cbor_format(const uint8_t *p, size_t n, char *out, size_t out_sz);

/** Minimal reader for decoding known CBOR layouts field by field. */
struct diag_cbor {
	const uint8_t *p;
	const uint8_t *end;
	bool err;
};

/** Major type of the next item (0..7), or -1 at the end or on error. */
int diag_cbor_peek(const struct diag_cbor *c);
/** Unsigned integer (major 0). */
bool diag_cbor_uint(struct diag_cbor *c, uint64_t *v);
/** Definite-length array (major 4) or map (major 5) header. */
bool diag_cbor_array(struct diag_cbor *c, uint64_t *n);
bool diag_cbor_map(struct diag_cbor *c, uint64_t *n);
/** Skip one whole item. */
bool diag_cbor_skip(struct diag_cbor *c);

#endif /* APP_DIAG_DIAG_H_ */
