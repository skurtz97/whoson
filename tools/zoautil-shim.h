/* SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Sean Kurtz
 *
 * Stand-in for ZOAU's <zoautil.h>, included only when __MVS__ is undefined so
 * that clangd can type-check whoson.c on a non-z/OS machine.  Never compiled
 * into the program: z/OS gets the real header.
 *
 * shortcut: mirrors just the ZOAU surface whoson.c touches; extend it when a
 * new ZOAU call appears, or the editor will report unknown declarations.
 */
#ifndef ZOAUTIL_SHIM_H
#define ZOAUTIL_SHIM_H

#include <stddef.h>

#define ZOAUTIL_EYECATCHER "ZOAUVEC4"
#define VERSION_CURRENT 4
#define FORMAT_TEXT 0

typedef struct {
	const char *eyecatchr;
	int version;
	int format;
} zoautil_options_t;

typedef struct {
	int cmd_rc;
	int stdout_len;
	int stderr_len;
	char *stdout_response;
	char *stderr_response;
} zoau_response;

typedef struct zoautil_vector4 {
	int (*opercmd)(char *cmd, size_t len, zoau_response **response,
		       zoautil_options_t options);
} zoautil_vector4_t;

unsigned int version(void);
zoautil_vector4_t *init(unsigned int vrm);
void term(zoautil_vector4_t *vector);
void free_response(zoau_response *response);

#endif
