/* SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */
/* FIZZ's descriptor (core/glue/core_quadrille_desc.c, unchanged) with its source_url pointing at the public repo at
 * the commit built: FIZZ's own says "FMToneSeq (local)". As RemoteFM1IOS's cores/fizz/fizz_desc.c. version.h is
 * #pragma once, so the descriptor's include of it keeps this definition. */
#include "version.h"
#undef FM1CORE_SOURCE_URL
#define FM1CORE_SOURCE_URL(repo) "https://github.com/Quixotic7/FIZZ@" FM1CORE_COMMIT
#include "core_quadrille_desc.c"
