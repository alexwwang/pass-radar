#!/usr/bin/env bash
# tools/build-firmware.sh —— 一条命令完成 pass-radar 固件本地构建(面向不熟悉 ESP-IDF 的用户)。
#
# 与 meta-pass 保持一致:仅产出一个发布固件文件,兼顾所有场景。
#   1. 自动寻找 ESP-IDF v5.5.3(常用路径 / --idf-path 参数 / $IDF_PATH 环境变量);
#   2. idf.py build 编译固件;
#   3. idf.py merge-bin 合并完整镜像(bootloader + 分区表 + phy + app 铺平);
#   4. 生成唯一发布单文件 build/pass-radar_<版本>.bin(可引导本体 + 44B MPUPV2
#      指纹尾段):
#        - 双卡实测:原样烧写 0x0(尾部 44B 落在 app 区末尾的空闲区,无副作用);
#        - 安装/升级通道:install 页按 MPUPV2 指纹解析切片(同一契约,
#          见 meta-pass install-slot/launcher-upgrade.js parseUpgradeArtifact());
#   5. verify_firmware.py 校验受保护布局(cardid@0x356000 不受影响);
#   6. 打印烧写指引。
#
# 用法:
#   tools/build-firmware.sh                  # 标准构建
#   tools/build-firmware.sh --idf-path ~/esp/esp-idf-v5.5.3
#   PORT=/dev/cu.usbserial-xxxx tools/build-firmware.sh   # 末尾直接给出该端口的烧写命令
#
# 前置(仅首次):
#   安装 ESP-IDF v5.5.3 与其依赖:https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32c3/get-started/
#   例:mkdir -p ~/esp && git clone -b v5.5.3 --recursive \
#        https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3 && ~/esp/esp-idf-v5.5.3/install.sh esp32c3
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

IDF_PATH_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --idf-path)
            [ $# -lt 2 ] && { echo "error: --idf-path needs a value" >&2; exit 2; }
            IDF_PATH_ARG="$2"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. 寻找并激活 ESP-IDF v5.5.3 ---------------------------------------
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
        cat >&2 <<'EOF'
error: 未找到 ESP-IDF v5.5.3。

方式一:已安装但不在默认位置?指定路径重跑:
    tools/build-firmware.sh --idf-path <你的 esp-idf-v5.5.3 目录>

方式二:尚未安装?约 10 分钟:
    mkdir -p ~/esp
    git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3
    ~/esp/esp-idf-v5.5.3/install.sh esp32c3
    tools/build-firmware.sh          # 重新运行本脚本即可
EOF
        exit 1
    fi
else
    info "激活 ESP-IDF: $IDF_DIR"
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
fi

# ---- 2. 编译 -------------------------------------------------------------
info "idf.py build(首次构建约 3~10 分钟,之后增量构建更快)"
idf.py build || fail "编译失败——把上方完整报错贴给 AI 助手或搜索 esp-idf issue"

# ---- 3. 合并完整镜像 -----------------------------------------------------
info "合并完整镜像(merge-bin)"
idf.py merge-bin -o pass-radar-full.bin >/dev/null \
    || fail "merge-bin 失败(注意:输出路径必须是文件名)"

# ---- 4. 单文件固件产物(可引导本体 + 44B MPUPV2 指纹尾段)----
# 与 meta-pass 同一单文件契约:同一个文件服务多个场景——
#   双卡实测/线刷    —— 原样写 0x0;
#   安装/升级通道    —— 按 MPUPV2 指纹解析后从本体切片 bootloader/分区表/app。
# app 分区镜像与全量镜像保留在 build/ 供 verify_firmware.py 校验,不对用户分发。
info "生成单文件固件产物(build/pass-radar_<版本>.bin)"
python3 - <<'PYEOF'
import hashlib
import struct
import subprocess
from pathlib import Path

version = subprocess.run(
    ["git", "-C", ".", "describe", "--tags", "--match", "v[0-9]*"],
    capture_output=True, text=True).stdout.strip() or "v0.0.0-dev"

# 本体 = 合并镜像头部(0x0 .. app 末尾):bootloader+分区表+phy+app 按 flash 偏移
# 铺平。与 meta-pass 的 MPUPV2 契约一致:magic "MPUPV2"(8B,NUL 结尾)+
# body_len u32le + body SHA-256 32B,共 44B 尾段。
app = (Path("build/pass-radar.bin")).read_bytes()
assert app[0] == 0xE9, "app magic"
full = (Path("build/pass-radar-full.bin")).read_bytes()
body = full[: 0x10000 + len(app)]
assert body[0] == 0xE9 and body[0x8000:0x8000+2] == b"\xAA\x50", "head layout"

assert len(body) < 1 << 32, "body must fit u32"
footer = b"MPUPV2\x00\x00" + struct.pack("<I", len(body)) + hashlib.sha256(body).digest()
assert len(footer) == 44, "footer layout"

out = Path("build") / f"pass-radar_{version}.bin"
out.write_bytes(body + footer)
print(f"Single-file firmware: {out.name} = {len(body)}B body + 44B MPUPV2 footer "
      f"({len(body) + len(footer)} bytes total; 0x0 flash + install/upgrade channel)")
PYEOF

# ---- 5. 受保护布局校验 ---------------------------------------------------
info "校验受保护固件布局(verify_firmware.py)"
python3 tools/verify_firmware.py build/ || fail "布局校验未通过"

# ---- 6. 汇总产物与烧写指引 -----------------------------------------------
VERSION="$(git -c safe.directory='*' -C "$REPO_ROOT" describe --tags --match 'v[0-9]*' 2>/dev/null || echo v0.0.0-dev)"

info "构建完成 ✓"
cat <<EOF

产物(build/ 下):
  pass-radar_${VERSION}.bin         唯一发布单文件:双卡实测原样写 0x0;
                                    安装/升级通道解析 MPUPV2 指纹(同一个文件)
  pass-radar.bin                    app 分区镜像(OTA / 安装页导入子固件用)
  pass-radar-full.bin               全量镜像(仅校验/工厂烧写,不对用户分发)

双卡实测烧写(两台卡各一次;PORT 换成你的串口,macOS 形如 /dev/cu.usbserial-xxxx):
  python -m esptool --chip esp32c3 -b 460800 -p PORT write-flash 0x0 build/pass-radar_${VERSION}.bin
  (烧完拔插开机;日志用 idf.py -p PORT monitor)

完整门禁(可选):
  ./tools/validate.sh               # 仓库检查 + host 测试 + 固件构建校验
EOF

if [ -n "${PORT:-}" ]; then
    echo
    info "检测到 \$PORT=$PORT,直接烧写:"
    echo "  python -m esptool --chip esp32c3 -b 460800 -p $PORT write-flash 0x0 build/pass-radar_${VERSION}.bin"
fi
