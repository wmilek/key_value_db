/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * `blob_db export` and `blob_db import`, registered by blob_db_shell.c.
 * The line format is doc/tools/blob_db_export.md.
 */

#ifndef BLOB_DB_SHELL_XFER_H_
#define BLOB_DB_SHELL_XFER_H_

#include <stddef.h>

#include <zephyr/shell/shell.h>

int blob_db_shell_cmd_export(const struct shell *sh, size_t argc, char **argv);

#if defined(CONFIG_BLOB_DB_SHELL_ADMIN)
int blob_db_shell_cmd_import(const struct shell *sh, size_t argc, char **argv);
#endif

#endif /* BLOB_DB_SHELL_XFER_H_ */
