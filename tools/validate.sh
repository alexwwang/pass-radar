#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

resolve_python() {
    local candidate
    local configured="${PASS_RADAR_PYTHON:-}"
    local candidates=()

    if [[ -n "${configured}" ]]; then
        candidates+=("${configured}")
    else
        [[ -n "${VIRTUAL_ENV:-}" ]] && candidates+=("${VIRTUAL_ENV}/bin/python")
        [[ -n "${IDF_PYTHON_ENV_PATH:-}" ]] && candidates+=("${IDF_PYTHON_ENV_PATH}/bin/python")
        candidate="$(command -v python3 || true)"
        [[ -n "${candidate}" ]] && candidates+=("${candidate}")
        candidate="$(command -v python || true)"
        [[ -n "${candidate}" ]] && candidates+=("${candidate}")
    fi

    for candidate in "${candidates[@]}"; do
        [[ -x "${candidate}" ]] || continue
        if "${candidate}" -c 'import sys; assert sys.prefix != sys.base_prefix' >/dev/null 2>&1 \
            && "${candidate}" -c 'import sys; from pathlib import Path; assert "uv = " in (Path(sys.prefix) / "pyvenv.cfg").read_text()' 2>/dev/null; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    cat >&2 <<'EOF'
ERROR: no uv-managed Python environment was found.
Activate the existing uv environment or set PASS_RADAR_PYTHON to its Python executable.
For ESP-IDF 5.5.3, set IDF_PYTHON_ENV_PATH before sourcing export.sh when needed.
EOF
    return 1
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local python_cmd

    python_cmd="$(resolve_python)"
    echo "Python environment: ${python_cmd}"
    "${python_cmd}" tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/pass-radar-host-tests.XXXXXX)"
    # 纯逻辑 host tests:新增测试源时在此登记编译/运行(radar 纯逻辑模块)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_radar_kalman.c main/radar_kalman.c \
        -o "${test_dir}/test_radar_kalman" -lm
    "${test_dir}/test_radar_kalman"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_radar_sweep.c main/radar_sweep.c main/radar_fusion.c \
        -o "${test_dir}/test_radar_sweep" -lm
    "${test_dir}/test_radar_sweep"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_radar_fusion.c main/radar_fusion.c main/radar_sweep.c \
        -o "${test_dir}/test_radar_fusion" -lm
    "${test_dir}/test_radar_fusion"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_radar_protocol.c main/radar_protocol.c main/radar_sweep.c \
        main/radar_fusion.c -o "${test_dir}/test_radar_protocol" -lm
    "${test_dir}/test_radar_protocol"
    "${python_cmd}" tests/test_verify_firmware.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir
    local python_cmd

    python_cmd="$(resolve_python)"
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/pass-radar-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/pass-radar-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/pass-radar-full.bin"
    "${python_cmd}" tools/verify_firmware.py "${validation_build_dir}"
    local version
    version="$(git -c safe.directory='*' -C "${repo_root}" describe --tags --match 'v[0-9]*' 2>/dev/null || echo v0.0.0-dev)"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/pass-radar-full.bin" \
        "${repo_root}/build/pass-radar_${version}.bin"
    echo "Firmware build: PASS (build/pass-radar_${version}.bin)"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
