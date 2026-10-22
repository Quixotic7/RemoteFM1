#!/usr/bin/env bash
# Run a build step, keep its full output in the job log, and on failure publish its last 40 lines as an error
# annotation (annotations are readable through the public API; job logs need admin rights).
#   [LOGGED_OK="3 ..."] .github/scripts/logged.sh NAME command [args...]
# LOGGED_OK: exit codes besides 0 that are not failures to annotate (native/build.sh's 3: some cores failed, each
# annotated on its own); the exit code is passed on either way.
set -uo pipefail
name="$1"; shift
log="$(mktemp)"
"$@" 2>&1 | tee "$log"
st=${PIPESTATUS[0]}
if [ "$st" -ne 0 ] && [[ " ${LOGGED_OK:-} " != *" $st "* ]]; then
  msg="$(tail -n 40 "$log" | sed -e 's/%/%25/g' -e 's/\r//g' | awk '{printf "%s%%0A", $0}')"
  echo "::error title=${name} failed (exit ${st})::${msg}"
fi
exit "$st"
