/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_cbor_persondb — entry point.
 *
 * Deliberately empty of logic. The frontend is chosen at build time
 * (CONFIG_APP_CBOR_PERSONDB_FRONTEND_*) and everything it does goes through
 * scenario.h, which goes through persondb.h. See DESIGN.md §7.
 */

#include <zephyr/kernel.h>

/** Provided by ui_bench.c or ui_shell.c, whichever the build selected. */
int ui_main(void);

int main(void)
{
	return ui_main();
}
