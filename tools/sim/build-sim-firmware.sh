#!/usr/bin/env bash
# The simulator must load the production firmware, not a simulator-specific build.
# Keep this command as a migration guard so stale workflows fail explicitly.
set -euo pipefail
cat >&2 <<'EOF'
ERROR: simulator-specific firmware builds are retired.
Build the production image with:
  ./tools/validate.sh --firmware
Then load build/pass-radar_v<version>.bin in passport-sim.
EOF
exit 2
