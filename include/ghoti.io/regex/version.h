/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Runtime version query for the Ghoti.io Regex library.
 *
 * The macros in libver.h say what the caller compiled against; these say what
 * it is linked against, which is the only way to tell the two apart when a
 * shared library has been upgraded underneath a binary.
 */

#ifndef GHOTI_IO_GRX_VERSION_H
#define GHOTI_IO_GRX_VERSION_H

#include <ghoti.io/regex/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The version of the library actually loaded, as a string.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @return A version such as "0.0.0", or "0.0.0-debug" for a debug build.
 */
GRX_API const char * grx_version_string(void);

/**
 * @brief The version of the library actually loaded, packed.
 *
 * One byte per component, so 1.2.3 is 0x010203 and a plain `<` compares two
 * versions. Compare against GRX_MAKE_VERSION(1, 2, 3).
 *
 * @return The packed version.
 */
GRX_API unsigned grx_version_number(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_VERSION_H
