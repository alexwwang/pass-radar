#!/usr/bin/env bash
# tools/sim/build-sim-firmware.sh —— 构建 passport-sim(QEMU-wasm)验证用固件。
#
# 与量产固件的区别(全部由 CONFIG_RADAR_SIM_BUILD 门控,量产零影响):
#   1. 裁剪 esp_now_send 发送路径(QEMU 无射频模型,发送会永久卡死);
#   2. BLE 真栈换 radar_ble_sim.c 空桩(QEMU 无 BLE 控制器);
#   3. 注入合成对端:ESP-NOW 包 + FTM 快照 + BLE RSSI + 扫盘余弦方向图。
#
# 用法: tools/sim/build-sim-firmware.sh [--idf-path <路径>]
# 产物: build-sim/pass-radar-sim.bin(喂给 radar-screen-test.mjs)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

IDF_PATH_ARG=""
SIM_SCENARIO="0"
while [ $# -gt 0 ]; do
    case "$1" in
        --idf-path)
            [ $# -lt 2 ] && { echo "error: --idf-path needs a value" >&2; exit 2; }
            IDF_PATH_ARG="$2"; shift 2 ;;
        --scenario)
            [ $# -lt 2 ] && { echo "error: --scenario needs 0, 1, or 2" >&2; exit 2; }
            case "$2" in 0|1|2) SIM_SCENARIO="$2" ;; *) echo "error: scenario must be 0, 1, or 2" >&2; exit 2 ;; esac
            shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 寻找并激活 ESP-IDF(与 build-firmware.sh 同一逻辑) ----
IDF_CANDIDATES=()
if [ -n "$IDF_PATH_ARG" ]; then
    IDF_CANDIDATES=("$IDF_PATH_ARG")
elif [ -n "${IDF_PATH:-}" ]; then
    IDF_CANDIDATES=("$IDF_PATH")
else
    IDF_CANDIDATES=("$HOME/esp/esp-idf-v5.5.3" "$HOME/.espressif/frameworks/esp-idf-v5.5.3" "/opt/esp-idf-v5.5.3")
fi

IDF_DIR=""
for d in "${IDF_CANDIDATES[@]}"; do
    [ -f "$d/export.sh" ] && IDF_DIR="$d" && break
done
if [ -z "$IDF_DIR" ]; then
    if command -v idf.py >/dev/null 2>&1; then
        info "检测到 PATH 中已有 idf.py,直接使用"
    else
        fail "未找到 ESP-IDF,可用 --idf-path 指定,或先安装 v5.5.3"
    fi
else
    info "激活 ESP-IDF: $IDF_DIR"
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
fi

# ---- 编译(独立 build-sim 目录 + 独立 sdkconfig,不碰量产的根 sdkconfig) ----
# 注意:若不指定 SDKCONFIG,idf.py 会复用/污染项目根目录的 sdkconfig
# (SDKCONFIG_DEFAULTS 也会被忽略),导致 sim 开关泄漏进量产配置。
SIM_DEFAULTS="sdkconfig.sim"
cp sdkconfig.sim build-sim/sdkconfig.sim
sed -i.bak "s/^CONFIG_RADAR_SIM_SCENARIO=.*/CONFIG_RADAR_SIM_SCENARIO=${SIM_SCENARIO}/" build-sim/sdkconfig.sim
rm -f build-sim/sdkconfig.sim.bak
SIM_DEFAULTS="build-sim/sdkconfig.sim"
if [ -f build-sim/sdkconfig ]; then
    if grep -q '^CONFIG_RADAR_SIM_SCENARIO=' build-sim/sdkconfig; then
        sed -i.bak "s/^CONFIG_RADAR_SIM_SCENARIO=.*/CONFIG_RADAR_SIM_SCENARIO=${SIM_SCENARIO}/" build-sim/sdkconfig
        rm -f build-sim/sdkconfig.bak
    else
        printf '\\nCONFIG_RADAR_SIM_SCENARIO=%s\\n' "$SIM_SCENARIO" >> build-sim/sdkconfig
    fi
fi
info "模拟场景: ${SIM_SCENARIO}"
info "idf.py build(RADAR_SIM_BUILD,输出在 build-sim/)"
idf.py -B build-sim -D SDKCONFIG=build-sim/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;${SIM_DEFAULTS}" build \
    || fail "sim 编译失败"

# 模拟器从 0x0 引导,需要合并镜像(bootloader+分区表+phy+app)。
info "合并完整镜像(merge-bin)"
idf.py -B build-sim merge-bin -o pass-radar-sim.bin >/dev/null \
    || fail "merge-bin 失败"
info "sim 固件就绪: build-sim/pass-radar-sim.bin"
echo "下一步: node tools/sim/radar-screen-test.mjs build-sim/pass-radar-sim.bin"
