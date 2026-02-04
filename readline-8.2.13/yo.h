/* yo.h -- LLM-powered shell assistant for readline */

/* Copyright (C) 2026 Epic Games, Inc.

   This file is part of the GNU Readline Library (Readline), a library
   for reading lines of text with interactive input and history editing.

   Readline is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Readline is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Readline.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _YO_H_
#define _YO_H_

#ifdef __cplusplus
extern "C" {
#endif

#if defined (READLINE_LIBRARY)
#  include "rlstdc.h"
#  include "rltypedefs.h"
#else
#  include <readline/rlstdc.h>
#  include <readline/rltypedefs.h>
#endif

/* Enable "yo" LLM features. Call this to opt-in (like using_history()).
   Binds Enter key to yo-aware accept-line and loads config from env vars.
   The system_prompt parameter is the prompt sent to the LLM - the shell
   should provide this to give context about the environment. */
extern void rl_yo_enable (const char *system_prompt);

/* Check if yo is currently enabled. Returns non-zero if enabled. */
extern int rl_yo_enabled (void);

/* The yo-aware accept-line function. Checks for "yo " prefix and
   calls LLM if found, otherwise behaves like normal accept-line. */
extern int rl_yo_accept_line (int, int);

/* Clear yo session history. Can be called to reset conversation context. */
extern void rl_yo_clear_history (void);

/* Get recent terminal scrollback text.
   Returns up to max_lines lines from the scrollback buffer.
   ANSI escape sequences are stripped for readability.
   Returns malloc'd string, caller must free.
   Returns empty string if scrollback is not available. */
extern char *rl_yo_get_scrollback (int max_lines);

#ifdef __cplusplus
}
#endif

#endif /* _YO_H_ */
