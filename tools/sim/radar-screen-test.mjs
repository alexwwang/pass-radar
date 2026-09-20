// tools/sim/radar-screen-test.mjs —— 用 passport-sim 加载正式 pass-radar 固件，
// 只验证固件能够 boot、进入应用并渲染首帧。
//
// passport-sim 当前不提供 BLE 控制器。BLE ROM 探测到后，模拟器会暂停并禁止
// 交互，因此本测试不注入按键，也不把页面切换或无线测量结果作为 PASS 条件。
// 真机上的 BLE/ESP-NOW/FTM/CSI 行为必须通过 host/真机测试验证。
//
// 运行: node tools/sim/radar-screen-test.mjs [正式固件.bin]
// 产物: /tmp/pass-radar-sim/1-boot.bmp + 1-boot.txt
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { initSync, WasmEmulator } from "../../../meta-pass/tools/sim/esp_emu.js";
import { AiPassportBoard } from "../../../meta-pass/tools/sim/ai-passport-board.js";

const DEFAULT_IMAGE = new URL("../../build/pass-radar.bin", import.meta.url);
const imagePath = process.argv[2] ?? DEFAULT_IMAGE;
if (!existsSync(imagePath)) {
  console.error(`正式固件不存在: ${imagePath}(先运行 ./tools/validate.sh --firmware)`);
  process.exit(1);
}

const SCREEN_W = 240;
const SCREEN_H = 320;
const BATCH = 50_000;
const BOOT_CYCLES = 2_000_000_000;

const wasm = initSync(
  await readFile(new URL("../../../meta-pass/tools/sim/esp_emu_bg.wasm", import.meta.url)),
);
const emulator = new WasmEmulator("esp32c3");
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.load_firmware(new Uint8Array(await readFile(imagePath)));

let frames = [];
const board = new AiPassportBoard(wasm, {
  emulator,
  onFrame: (frame) => frames.push(frame),
});

for (let n = 0; n < Math.ceil(BOOT_CYCLES / BATCH); n += 1) {
  emulator.run_batch(BATCH);
  board.drain();
}

const frame = frames.at(-1);
if (!frame) {
  emulator.free();
  throw new Error("[boot] 没有任何显示帧：正式固件未进入应用渲染路径");
}
if (frame.width !== SCREEN_W || frame.height !== SCREEN_H) {
  emulator.free();
  throw new Error(`[boot] 显示尺寸异常: ${frame.width}x${frame.height}`);
}
const pixels = frame.pixels;
let nonZero = false;
let nonWhite = false;
for (const pixel of pixels) {
  if (pixel !== 0) nonZero = true;
  if (pixel !== 255) nonWhite = true;
}
if (!nonZero || !nonWhite) {
  emulator.free();
  throw new Error("[boot] 首帧为纯色：显示管线可能未激活");
}

const outDir = "/tmp/pass-radar-sim";
await mkdir(outDir, { recursive: true });
const hash = createHash("sha256")
  .update(Buffer.from(pixels.buffer))
  .digest("hex")
  .slice(0, 12);

const rowSize = Math.ceil((SCREEN_W * 3) / 4) * 4;
const bmp = Buffer.alloc(54 + rowSize * SCREEN_H);
bmp.write("BM", 0);
bmp.writeUInt32LE(bmp.length, 2);
bmp.writeUInt32LE(54, 10);
bmp.writeUInt32LE(40, 14);
bmp.writeInt32LE(SCREEN_W, 18);
bmp.writeInt32LE(SCREEN_H, 22);
bmp.writeUInt16LE(1, 26);
bmp.writeUInt16LE(24, 28);
bmp.writeUInt32LE(rowSize * SCREEN_H, 34);
for (let y = 0; y < SCREEN_H; y += 1) {
  const dst = 54 + (SCREEN_H - 1 - y) * rowSize;
  for (let x = 0; x < SCREEN_W; x += 1) {
    const source = (y * SCREEN_W + x) * 4;
    bmp[dst + x * 3] = pixels[source + 2];
    bmp[dst + x * 3 + 1] = pixels[source + 1];
    bmp[dst + x * 3 + 2] = pixels[source];
  }
}
await writeFile(`${outDir}/1-boot.bmp`, bmp);

const ascii = [];
for (let y = 0; y < SCREEN_H; y += 8) {
  let line = "";
  for (let x = 0; x < SCREEN_W; x += 3) {
    const i = (y * SCREEN_W + x) * 4;
    const lum = 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
    line += lum > 200 ? " " : lum > 140 ? "." : lum > 70 ? "+" : "#";
  }
  ascii.push(line);
}
await writeFile(`${outDir}/1-boot.txt`, ascii.join("\n"));

console.log(`[boot] PASS hash=${hash} frames=${frames.length} -> ${outDir}/1-boot.bmp`);
console.log("[scope] 仅验证 boot/首帧；未验证 BLE、无线测量或按键交互");
console.log(`pc=${emulator.pc()} cycles=${emulator.cycles()}`);
emulator.free();
