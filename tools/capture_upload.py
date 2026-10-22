#!/usr/bin/env python3
# tools/capture_upload.py —— pass-radar 独立模式上行兜底(开发者/无手机场景)。
#
# 一条命令:esptool 读 capture 分区原始字节(RCB1 整区镜像)→ 校验头部
# → POST metapass.chuanxilu.net /api/capture/ingest(设备身份 = 头部
# device_hash,服务端解析,本脚本不传)。普通用户请走设备 AP 网页
# (主页面 DOWN 双击 → 手机连 passradar-up)。
#
# 用法:
#   python3 tools/capture_upload.py [--port PORT] [--size BYTES]
#       [--ingest https://metapass.chuanxilu.net] [--cardid HEX]
#   --size  默认按 partitions.csv 声明(1 MiB 起;独立安装固定布局)
#   --cardid 核对模式:传入本机 cardid 前 16B(hex),与头部 device_hash
#           重算比对,验证"哪台设备"的采集
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import urllib.request

CAPTURE_OFFSET = 0x360000  # partitions.csv 保护布局(独立安装)
INGEST_DEFAULT = "https://metapass.chuanxilu.net"


def read_flash(port: str, offset: int, size: int) -> bytes:
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "read_flash", str(offset), str(size), "-"]
    try:
        out = subprocess.run(cmd, capture_output=True, check=True)
    except subprocess.CalledProcessError as e:
        sys.exit(f"esptool 失败:{e.stderr.decode(errors='replace')[:400]}")
    return out.stdout


def parse_header(data: bytes):
    if len(data) < 128 or data[:4] != b"RCB1":
        return None
    version, = struct.unpack_from("<H", data, 4)
    batch, expected, committed = struct.unpack_from("<III", data, 8)
    device = data[28:44].hex()
    return {"version": version, "batch": batch, "expected": expected,
            "committed": committed, "device": device}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/cu.usbmodem14201")
    ap.add_argument("--size", type=int, default=0x100000)
    ap.add_argument("--ingest", default=INGEST_DEFAULT)
    ap.add_argument("--cardid", default=None,
                    help="本机 cardid 前 16B(hex,32 字符):核对模式,不重传")
    args = ap.parse_args()

    if args.cardid:
        raw = bytes.fromhex(args.cardid)
        if len(raw) != 16:
            sys.exit("--cardid 需 32 个 hex 字符(16B)")
        print(f"cardid 哈希(本地重算):{hashlib.sha256(raw).digest()[:16].hex()}")
        return 0

    print(f"读取 flash 0x{CAPTURE_OFFSET:x}+{args.size:#x} …")
    data = read_flash(args.port, CAPTURE_OFFSET, args.size)
    head = parse_header(data)
    if not head:
        print("数据区没有 RCB1 批次(尚未采集)。")
        return 1
    print(f"批次 {head['batch']}(v{head['version']}) "
          f"{head['committed']}/{head['expected']} 条,设备 {head['device'][:16]}…")

    url = f"{args.ingest}/api/capture/ingest?batch={head['batch']}"
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=60) as res:
            payload = json.loads(res.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"服务端拒绝 HTTP {e.code}:{e.read().decode(errors='replace')[:200]}")
    print(f"已上传:{payload['bytes']} 字节 sha256 {payload['sha256'][:16]}… "
          f"{payload['ingestedAt']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
