#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# the host simulator built without the shadow (RFM_SHADOW 0, as a WiFi build would be): CC=firmware/tests/cc_noshadow.sh
exec "${HOST_CC:-cc}" -DRFM_SHADOW=0 "$@"
