#!/usr/bin/env python3
# tools/capture_upload.py —— pass-radar 独立模式上行兜底(开发者/无手机场景)。
#
# 一条命令:esptool 读 capture 分区原始字节(RCB1 整区镜像)→ 校验头部
# + v7 签名 trailer → POST metapass.chuanxilu.net /api/capture/ingest
# (设备身份 = 头部 device_hash,服务端解析,本脚本不传)。普通用户请走
# 设备 AP 网页(主页面 DOWN 双击 → 手机连 passradar-up)。
#
# 用法:
#   python3 tools/capture_upload.py [--port PORT] [--size BYTES]
#       [--ingest https://metapass.chuanxilu.net]
#   python3 tools/capture_upload.py --register [--device http://192.168.4.1]
#       (设备需在上传模式;读 /api/identity 并注册公钥到云端,first-wins)
# v7 身份源 = Wi-Fi/BLE 双 MAC 派生(cardid 方案已弃用,--cardid 随之移除)。
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import urllib.request

CAPTURE_OFFSET = 0x360000  # partitions.csv 保护布局(独立安装)
INGEST_DEFAULT = "https://metapass.chuanxilu.net"
TRAILER_MAGIC = b"SGN1"
SECTOR = 0x1000


def read_flash(port: str, offset: int, size: int) -> bytes:
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "read_flash", str(offset), str(size), "-"]
    try:
        out = subprocess.run(cmd, capture_output=True, check=True)
    except subprocess.CalledProcessError as e:
        sys.exit(f"esptool 失败:{e.stderr.decode(errors='replace')[:400]}")
    return out.stdout


def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return ~crc & 0xFFFFFFFF


def parse_header(data: bytes):
    if len(data) < 128 or data[:4] != b"RCB1":
        return None
    version, header_size = struct.unpack_from("<HH", data, 4)
    batch, expected, committed = struct.unpack_from("<III", data, 8)
    device = data[28:44].hex()
    return {"version": version, "header_size": header_size, "batch": batch,
            "expected": expected, "committed": committed, "device": device}


def parse_trailer(data: bytes, head: dict):
    """v7 SGN1 trailer:返回 (状态, 详情)。v6 及更早返回 (None, 无 trailer)。"""
    if head["version"] < 7 or len(data) < SECTOR + 128:
        return None, "无 trailer(v6 及更早)"
    off = len(data) - SECTOR
    raw = data[off:off + 128]
    if raw[:4] != TRAILER_MAGIC:
        return "none", "trailer 扇区未签名(上传模式未进入?)"
    version, size = struct.unpack_from("<HH", raw, 4)
    if version != 1 or size != 128:
        return "bad", f"trailer 版本/长度异常 v{version} size={size}"
    batch, = struct.unpack_from("<I", raw, 8)
    if batch != head["batch"]:
        return "bad", f"trailer batch={batch} ≠ 头 batch={head['batch']}"
    stored_crc, = struct.unpack_from("<I", raw, 124)
    if stored_crc != crc32(raw[:124] + b"\x00\x00\x00\x00"):
        return "bad", "trailer CRC 不符"
    payload_sha = raw[28:60]
    actual = hashlib.sha256(
        data[head["header_size"]:len(data) - SECTOR]).digest()
    if actual != payload_sha:
        return "bad", "载荷 SHA-256 不符(数据被改动?)"
    sig_blank = all(b in (0x00, 0xFF) for b in raw[60:124])
    if sig_blank:
        return "bad", "trailer 签名为空"
    return "ok", f"签名 trailer 完整,sig={raw[60:68].hex()}…"


def http_json(url: str, payload: dict = None, timeout: int = 30) -> dict:
    body = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        url, data=body, method="POST" if body is not None else "GET",
        headers={"Content-Type": "application/json",
                 "User-Agent": "capture-upload-cli/1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            return json.loads(res.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}:{e.read().decode(errors='replace')[:300]}")


def cmd_register(args) -> int:
    ident = http_json(f"{args.device.rstrip('/')}/api/identity")
    if ident.get("kind") != "mac-ed25519":
        sys.exit(f"设备身份端点异常:{ident}")
    print(f"设备 {ident['device'][:16]}… 公钥 {ident['pubkey'][:16]}…")
    res = http_json(f"{args.ingest}/api/capture/enroll",
                    {"device": ident["device"], "pubkey": ident["pubkey"]})
    print(f"注册结果:{res}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem14201")
    ap.add_argument("--size", type=int, default=0x100000)
    ap.add_argument("--ingest", default=INGEST_DEFAULT)
    ap.add_argument("--register", action="store_true",
                    help="注册设备身份到云端(设备需在上传模式)")
    ap.add_argument("--device", default="http://192.168.4.1",
                    help="--register 用的设备地址(上传模式 AP 或 STA IP)")
    args = ap.parse_args()

    if args.register:
        return cmd_register(args)

    print(f"读取 flash 0x{CAPTURE_OFFSET:x}+{args.size:#x} …")
    data = read_flash(args.port, CAPTURE_OFFSET, args.size)
    head = parse_header(data)
    if not head:
        print("数据区没有 RCB1 批次(尚未采集)。")
        return 1
    print(f"批次 {head['batch']}(v{head['version']}) "
          f"{head['committed']}/{head['expected']} 条,设备 {head['device'][:16]}…")
    status, detail = parse_trailer(data, head)
    if status:
        print(f"trailer: {detail}")
        if status == "bad":
            print("警告:trailer 异常仍会上传,云端验签将拒绝(403)。"
                  "建议重新进入上传模式让设备补签。")

    url = f"{args.ingest}/api/capture/ingest?batch={head['batch']}"
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/octet-stream",
                 "User-Agent": "capture-upload-cli/1"})
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
