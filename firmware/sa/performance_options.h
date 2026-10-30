/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 RemoteFM1 contributors */
/* saadet's fm6_core.c includes "performance_options.h" (saadet's build options). FM6 reads one of them:
 * MELODEE_FM6_FAST, on in saadet's production and host builds (a sustained or finished operator's envelope skips its
 * call; the same samples). The others are saadet's instrument's. */
#pragma once
#ifndef MELODEE_FM6_FAST
#define MELODEE_FM6_FAST 1
#endif
