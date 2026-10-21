#!/usr/bin/env bash
# CI bootstrap: provide the uv-managed Python environment that
# tools/validate.sh requires (docs/development/engineering/build-and-test.md).
# Source this script before invoking validate.sh:
#   source ./tools/ci-uv-env.sh && ./tools/validate.sh --static
set -euo pipefail

if ! command -v uv >/dev/null 2>&1; then
  python3 -m pip install --quiet uv
fi
uv venv .ci-venv >/dev/null
uv pip install --quiet --python .ci-venv pyyaml
export PASS_RADAR_PYTHON="${PWD}/.ci-venv/bin/python"
echo "PASS_RADAR_PYTHON=${PASS_RADAR_PYTHON}"
