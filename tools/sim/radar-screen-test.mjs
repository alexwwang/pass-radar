// tools/sim/radar-screen-test.mjs —— 用 passport-sim(QEMU-wasm)加载 pass-radar 固件,
// 注入按键,抓取 ST7789 帧,导出 BMP 截图 + ASCII 缩略图,用于在无真机时检查页面布局。
//
// 复用 meta-pass/tools/sim 的板级桥接器(AI Passport 板:240x320 ST7789 + ADC 按键)。
// QEMU 没有 RF:ESP-NOW/FTM/BLE 都不会有真实对端,页面显示的是"无对端"状态 ——
// 这正好能验证:布局/字体/文案在缺数据时是否正常(±、+/~ 前缀、FTM fail 状态码等)。
//
// 运行: node tools/sim/radar-screen-test.mjs [固件.bin]
// 产物: /tmp/pass-radar-sim/*.bmp(macOS 可直接预览)
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { initSync, WasmEmulator } from "../../../meta-pass/tools/sim/esp_emu.js";
import { AiPassportBoard } from "../../../meta-pass/tools/sim/ai-passport-board.js";

const DEFAULT_IMAGE = new URL(
  "../../build/pass-radar_v0.1-8-g0c02d44.bin",
  import.meta.url,
);
const imagePath = process.argv[2] ?? DEFAULT_IMAGE;
if (!existsSync(imagePath)) {
  console.error(`固件不存在: ${imagePath}(先跑 tools/build-firmware.sh)`);
  process.exit(1);
}

const SCREEN_W = 240;
const SCREEN_H = 320;
const BATCH = 50_000;
const BOOT_CYCLES = 2_000_000_000;   // 引导 + Wi-Fi/BLE 初始化 + LVGL 首帧
const CLICK_CYCLES = 12_000_000;     // 短按(与 meta-pass 测试一致,避免触发 LONG)
const RELEASE_CYCLES = 20_000_000;   // 松开等重绘
const SWEEP_CYCLES = 700_000_000;    // 扫盘 ~4s + 余量(160MHz ≈ 640M cycles)

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
  onFrame: (f) => frames.push(f),
});

const runCycles = async (cycles) => {
  for (let n = 0; n < Math.ceil(cycles / BATCH); n++) {
    emulator.run_batch(BATCH);
    board.drain();
  }
};
const lastFrame = () => frames.at(-1);
const frameHash = (f) =>
  createHash("sha256").update(Buffer.from(f.pixels.buffer)).digest("hex").slice(0, 12);

// ---- 导出 ----
const OUT_DIR = "/tmp/pass-radar-sim";
await mkdir(OUT_DIR, { recursive: true });

function saveBmp(name, frame) {
  const { width: w, height: h, pixels } = frame;
  const rowSize = Math.ceil((w * 3) / 4) * 4;
  const dataOff = 54;
  const buf = Buffer.alloc(dataOff + rowSize * h);
  buf.write("BM", 0);
  buf.writeUInt32LE(buf.length, 2);
  buf.writeUInt32LE(dataOff, 10);
  buf.writeUInt32LE(40, 14);
  buf.writeInt32LE(w, 18);
  buf.writeInt32LE(h, 22);
  buf.writeUInt16LE(1, 26);
  buf.writeUInt16LE(24, 28);
  buf.writeUInt32LE(rowSize * h, 34);
  for (let y = 0; y < h; y++) {
    const dstRow = dataOff + (h - 1 - y) * rowSize; // BMP 自底向上
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4; // RGBA
      buf[dstRow + x * 3 + 0] = pixels[i + 2]; // B
      buf[dstRow + x * 3 + 1] = pixels[i + 1]; // G
      buf[dstRow + x * 3 + 2] = pixels[i + 0]; // R
    }
  }
  const path = `${OUT_DIR}/${name}.bmp`;
  return writeFile(path, buf).then(() => path);
}

function asciiArt(frame, xStep = 3, yStep = 8) {
  const { width: w, height: h, pixels } = frame;
  const lines = [];
  for (let y = 0; y < h; y += yStep) {
    let line = "";
    for (let x = 0; x < w; x += xStep) {
      const i = (y * w + x) * 4;
      const lum = 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
      line += lum > 200 ? " " : lum > 140 ? "." : lum > 70 ? "+" : "#";
    }
    lines.push(line);
  }
  return lines.join("\n");
}

function asciiFine(frame, name) {
  // 1:2 采样(xStep=1, yStep=2):14px 字体一行 ≈ 7 行 ASCII,可辨认字形。
  const { width: w, height: h, pixels } = frame;
  const lines = [];
  for (let y = 0; y < h; y += 2) {
    let line = "";
    for (let x = 0; x < w; x += 1) {
      const i = (y * w + x) * 4;
      const lum = 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
      line += lum > 200 ? " " : lum > 140 ? "." : lum > 70 ? "+" : "#";
    }
    lines.push(`${String(y).padStart(3)} ${line}`);
  }
  return writeFile(`${OUT_DIR}/${name}.txt`, lines.join("\n"));
}

let lastHash = "";
const snap = async (name) => {
  const f = lastFrame();
  if (!f) {
    console.error(`[${name}] 没有任何帧!`);
    return;
  }
  const path = await saveBmp(name, f);
  await asciiFine(f, name);
  const h = frameHash(f);
  const changed = h !== lastHash ? "changed" : "UNCHANGED!";
  lastHash = h;
  console.log(`[${name}] hash=${h} ${changed} frames=${frames.length} -> ${path} + ${name}.txt`);
};

// ---- 场景 ----
// 顺序:主页 → 短按 OK 扫盘 → 结果页 → OK 返回主页 → UP 双击诊断页 →
//       OK 返回 → 长按 OK 标定(等满 3s 采样期)。
// (不要在标定采样期按键:标定状态下 OK 会被吞,后续页面全部 UNCHANGED)
console.log(`镜像: ${imagePath}`);
await runCycles(BOOT_CYCLES);
await snap("1-main");

// OK 短按 → 进入扫盘
board.setButton("OK", true);
await runCycles(CLICK_CYCLES);
board.setButton("OK", false);
await runCycles(RELEASE_CYCLES);
await runCycles(150_000_000);   // ~0.9s:扫盘动画进行中
await snap("2-sweeping");

// 等扫盘结束(4s) → 结果页
await runCycles(SWEEP_CYCLES);
await snap("3-result");

// OK 返回主页,UP 双击 → 诊断页
board.setButton("OK", true);
await runCycles(CLICK_CYCLES);
board.setButton("OK", false);
await runCycles(RELEASE_CYCLES);
await runCycles(30_000_000);
board.setButton("UP", true);
await runCycles(CLICK_CYCLES);
board.setButton("UP", false);
await runCycles(12_000_000);
board.setButton("UP", true);
await runCycles(CLICK_CYCLES);
board.setButton("UP", false);
await runCycles(40_000_000);
await snap("4-debug");

// OK 返回主页,长按 OK → 标定页,等满 3s 采样 + 完成重绘
board.setButton("OK", true);
await runCycles(CLICK_CYCLES);
board.setButton("OK", false);
await runCycles(RELEASE_CYCLES);
await runCycles(30_000_000);
const framesBeforeLong = frames.length;
board.setButton("OK", true);
await runCycles(480_000_000);   // ~3s@160MHz,超过 LONG 阈值 1.5s
console.log(`[long-press] 长按期间新帧: ${frames.length - framesBeforeLong}`);
await snap("5b-calib-countdown");
board.setButton("OK", false);
await runCycles(600_000_000);   // 等标定采样(3s)完成
await snap("5c-calib-done");

console.log(`\npc=${emulator.pc()} cycles=${emulator.cycles()}(模拟器存活)`);
emulator.free();
