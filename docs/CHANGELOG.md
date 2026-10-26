# Changelog

English | [简体中文](CHANGELOG.zh_CN.md)

## v0.3.0 (2026-10-26)

### Added
- Capture semantics finalized (design clarification, 2026-10-26): a batch holds as many captures as the partition physically fits — **no count limit anywhere** (1 MiB carve: 254 captures at typical payload, 127 at worst case). The old `MAX_CAPTURES` gate and the `expected_captures` append refusal are removed; the header field survives as a pure plan number clamped to a 65535 format sanity bound. Each capture run now samples at a fixed 250 ms interval until **120 records** are collected (`CAPTURE_RECORD_TARGET`; the previous 30 s window produced only ~99 records at the real FTM cadence), with a 45 s safety timeout
- `/api/capture/info` gains `records`: total sample count across all committed captures in the batch (sum of per-capture `record_count`), so the upload page can show "N captures · M records" instead of leaking the internal `expected` bound
- Upload-service resilience fixes from real-device testing (2026-10-24): (1) a failed `httpd_start` (e.g. transient `ESP_ERR_HTTPD_TASK`) left a half-started server holding port 80 — every later enter then died with `listen (112)` EADDRINUSE, so the upload page became permanently unreachable until reboot; start_httpd now stops and clears the zombie on failure. (2) The BLE sentinel scan and Wi-Fi channel scanning fight over the radio: even a pure-STA full-channel scan returned only 2 networks (one with an empty SSID) while the phone sees a dozen, and STA joins repeatedly timed out; the upload session now pauses BLE (`radar_ble_pause`, same precedent as sweep/calibration) and resumes it on exit. Enter-path breadcrumbs (`upload enter: begin`, `sta join res…
- Provisioning scan fix from real-device testing (2026-10-24): with the softAP up, the radio is pinned to the hotspot channel and `esp_wifi_scan_start` only sees APs on that one channel (device repeatedly found 2 networks while the phone saw a dozen; the target router was never listed). The upload service now does a full-channel scan while the AP is still down (mode flipped to STA-only for the scan, then back to APSTA), caches the result at entry, and `/api/scan` serves the cache for 120 s (also fixing the slow "scanning..." wait — it now answers instantly) merging any live re-scan results afterwards
- Upload-page fixes from real-device testing (2026-10-24): the upload page text was written in Chinese but rendered with `lv_font_montserrat_14` (no CJK glyphs) — every non-ASCII line showed as tofu squares. All upload page strings are now English, matching the rest of the UI which ships only Montserrat. Also, repeatedly entering the upload service crashed with `assert failed: netif_add (netif already added)` and rebooted: the bring-up fix replays `WIFI_EVENT_STA_START` to sync the default glue, but the glue's start action calls `netif_add`, and lwIP asserts when the same netif struct is added twice — the replay now happens only once per netif lifetime (first creation), which is sufficient since the glue stays in sync afterwards
- Device identity, redesign: MAC-derived Ed25519 (cardid dropped). The cardid partition on this unit turned out to be fully erased (16 KiB of 0xFF — erased identity is unrecoverable, and meta-pass never writes cardid), so identity now derives from the two factory efuse MACs instead: `device_hash = SHA-256("mpr-id-v1" || wifi_mac || ble_mac)[:16]`, Ed25519 seed = `SHA-256("mpr-ed25519-seed-v1" || wifi_mac || ble_mac)` (deterministic, survives reflashing and NVS erasure; the private key never leaves the device). New `radar_identity` module (`radar_identity_get` / `radar_identity_sign`, PSA pure EdDSA). Same-privacy property as before: only the hash and the public key ever reach the cloud. Trade-off vs cardid: MACs are broadcast values, so an attacker who has sniffed the device on the local radio link can derive the key — the scheme resists remote/cloud forgery, not a nearby adversary. Old v6 headers stamped with the cardid hash remain opaque hashes to the cloud.
- Batch header v7 + signed trailer (`SGN1`): the batch header stays 128 bytes (layout unchanged, `version = 7`); the last 4 KiB sector of the capture region is now a signed trailer — `{magic, version, batch_id, device_hash, payload_sha256, ed25519_sig, crc32}`. The signature covers `SHA-256` of the payload region plus header binding (preimage `"MPRCAP-SIG-v1" || version || batch_id || device_hash || payload_sha256`), so a swapped payload or a tampered header breaks verification. Signing is lazy: entering the upload service hashes the payload region (~1 MiB, sector-chunked) and writes the trailer sector once (idempotent; `begin()` erases the whole region so old signatures never go stale). Payload capacity shrinks by one 4 KiB sector. The cloud worker rejects a batch whose device is enrolled but whose signature does not verify (403), marks unenrolled v7 uploads `verified: false`, and meta.json records `verified`/`pubkey`/`device_kind: "mac-sha256-trunc16"`. New `POST /api/capture/enroll` (first-wins binding of device hash to public key, rate-limited). New `GET /api/identity` on the upload service (`{device, pubkey, kind: "mac-ed25519"}`) powers enrollment from the phone page or `tools/capture_upload.py --register` (the `--cardid` option is removed; the CLI also parses and locally validates the trailer before uploading).
- Standalone upload service (plan A standalone channel): `radar_upload.c` hosts a minimal HTTP service for phone-based batch export, entered from the main page with DOWN double-click and exited the same way. **Wi-Fi first**: on entry it reuses the meta-pass provisioning credentials (NVS namespace `metapass`, keys `sta_ssid`/`sta_pass` — present whenever the device went through meta-pass provisioning; flashing the standalone firmware does not erase NVS) to join the home router as STA (default STA netif + DHCP, 10 s timeout); the phone reaches the device on the LAN and the whole pull + upload runs over Wi-Fi, zero cellular traffic. Three modes: `STA` (credentials present and joined — Wi-Fi throughout), `PROV` (no credentials — open hotspot `passradar-up` plus a built-in provisioning portal: captive-portal DNS signal, `GET /api/scan` network list, `POST /api/wifi`; on success the device joins the router in place and the portal page at `/` hands over to the upload boot page at `/up`, the informed mobile-data fallback, always linked from the portal), and `AP` (credentials exist but the join failed — hotspot with explicit on-screen disclosure that the upload will use the phone's mobile data, portal still reachable for re-provisioning). The device serves a boot page that loads the cloud-hosted `capture-upload.js` module (metapass.chuanxilu.net, same interaction model as the meta-pass phone management), exposing `GET /api/capture/info` (batch overview incl. device hash) and chunked `GET /api/capture/read` (4096-byte blocks, bounds-checked); the phone relays the raw RCB1 region to `POST /api/capture/ingest` — the same ingest → R2 path as the meta-pass child channel. Provisioning writes the same NVS contract meta-pass reads, so both firmwares share credentials. Exiting stops the DNS relay, disconnects the STA (channel restored) or restores the FTM SoftAP configuration, whichever applies. The upload page text follows the mode via a 1 s LVGL refresh timer (self-deleting on page leave). A CLI fallback `tools/capture_upload.py` reads the region via esptool and can register the device identity (`--register`) as well as validate the signed trailer before upload. Verified on real hardware: STA mode serves the full endpoint family on the LAN IP; the provisioning portal serves its page, captive-portal 302, scan list, and rejects malformed credential posts; credential submit stores the shared NVS contract and wakes the join task
- Standalone upload service bring-up fixes (found by real-device testing): the STA netif is created after the radio stack has already started the STA interface (ESP-NOW owns Wi-Fi bring-up), so the default glue missed `WIFI_EVENT_STA_START` — the netif never came up (`dhcp client start failed`) and the first RX packet jumped to a NULL `netif->input` (instruction access fault at address 0). Fix: replay `WIFI_EVENT_STA_START` once after netif/handler creation (idempotent for the glue), with a `join_in_progress` guard so the replayed event cannot trigger a duplicate `esp_wifi_connect` racing the real `set_config`. The provisioning task and its semaphore are now created together (`prov_task_ensure`) — the task takes the semaphore as its first instruction, so creating the semaphore later in the POST handler was a guaranteed NULL assert. `on_key` now releases the LVGL lock across `radar_upload_enter()` (the STA join can block up to 10 s; holding the lock froze rendering and tripped the task watchdog)
- Batch header v6: `device_hash[16]` replaces the first 16 bytes of the header reserved area — `SHA-256(cardid[0:16])` truncated to 16 bytes, stamped by `radar_capture_device_hash()` at `begin()` (read via partition label `cardid` + `SUBTYPE_ANY`, so the same code path serves standalone installs (partitions.csv subtype nvs) and meta-pass child installs (carve table subtype 0x02)). The raw cardid never leaves the device; holders verify identity locally by recomputing the hash. Firmware without a readable cardid (unprovisioned dev board) stamps the deterministic erased-input hash. New public accessors `radar_capture_batch_data_size()` / `radar_capture_batch_data_read()` (bounds-checked raw region reads for the upload service)

### Fixed
- `sync_committed_counter` corrupted the batch header: the signing path erases sector 0 and rewrites the committed count without recomputing `header_crc32`, so the next boot rejected the header (`partition is occupied`) and every capture then failed with `ESP_ERR_INVALID_STATE` (found on hardware; the CRC is now recomputed before the write, with a host regression test)
- Cloud-ingest payload hash range: the worker hashed `[128, len-4096)` while the on-device digest covers the sector-aligned payload region `[4096, len-4096)` — every real upload died with `403 payload sha mismatch`. LAN end-to-end now verified on hardware: pull 1 MiB over `/api/capture/read`, ingest, `verified:true`
- Split boot for Wi-Fi coexistence (root cause found by controlled experiment): with BLE initialized, the radio sees only 1–2 networks on a single channel and STA joins time out — the BT coexistence scheduler starves the STA scan, not a hardware fault. DOWN double-click now sets an NVS flag and reboots into a BLE-free boot (BT never powered, same radio conditions as the meta-pass launcher), which auto-enters upload mode; exiting clears the flag and reboots back to the full firmware. A BLE-free full-channel scan finds 15 networks and joins succeed
- Provisioning from any state: `/prov` portal reachable in every mode, `POST /api/wifi` re-joins even when already connected (`sta_join_with` first disconnects the old AP so `esp_wifi_connect` cannot swallow the new config as a roam), and a failed re-provision falls back to the hotspot. Scan deadlock fixed with a bounded `scan_start_and_wait()` (SCAN_DONE event wait + hard timeout, never blocks forever)
- Batch magic endianness: RCB1/RCC1/RCOK constants were defined as host-order u32 values but the on-flash layout is byte order, so every real upload was rejected at the first magic gate (`1BCR` vs `RCB1`)
- Signing idempotency now includes the payload digest: appending captures after a sign used to leave the old trailer valid-looking, and the cloud rejected the new data with 403; the committed counter is also persisted to flash before signing
- Ed25519 without PSA: ESP-IDF 5.5.3's mbedTLS ships no Edwards-curve code (every `psa_import_key` for Ed25519 fails), so `orlp/ed25519` is vendored and `radar_identity` uses standard RFC 8032 semantics (verification side unchanged); host tests pin the RFC 8032 test vectors
- v8 compact record layout: header slot + payload + commit now byte-program into one erased sector (legal NOR: each byte written once, 0xFF padding), one sector per small-payload capture instead of three — worst-case capacity rises from 84 to 127 captures per 1 MiB and typical payloads fit 254; `FORMAT_VERSION = 8`, minimum 8 so old-layout regions are rejected instead of misparsed
- Upload UI interaction: save/scan buttons disable on submit and re-enable only on failure (double-submit raced the device HTTP server)

## v0.2.0 (2026-10-21)

### Fixed
- Batch Capture SAVE failing with `CAPTURE_SAVE FAIL BATCH err=ESP_ERR_INVALID_STATE` whenever the operator started a run without touching the Capture menu context: the power-on default room size was `LARGE` while the default space is `OUTDOOR_OPEN`, and the capture matrix admits only `NOT_APPLICABLE` for outdoor-open space — the batch append guard rejected the meta after the full 43 s run (menu-driven selections are filtered and never produce that pair; only the uninitialized defaults did). Default is now `NOT_APPLICABLE`, self-consistent with the default outdoor scene (root-caused on hardware 2026-10-21)
- Batch Capture storage under the meta-pass v2 (dynslot) launcher: the
  hardcoded v1 `ota_2` lookup never resolves on the dynamic slot table
  (`BATCH data unavailable: slot2 is missing`), so no collected run could be
  stored. The batch region is now discovered in priority order: the
  child-declared `capture` DATA carve (subtype `0x81`, declared 1 MiB in
  `partitions.csv`, user-extensible at install), materialized by the
  launcher for the active play and resolved by label; then the legacy v1
  empty `ota_2` slot. Capacity is partition-derived at runtime and `begin()`
  clamps the requested batch count to it (1 MiB carve = 85 worst-case
  Captures; the on-flash RCB1/RCC1/RCOK format is unchanged)
- Device-test round 1 findings: sweeps reporting OUT OF RANGE within 10 m
  (radio starvation during 100ms channel hops + BLE scan) — hop dwell raised
  to 250 ms and BLE scanning paused during sweeps/calibration
- FTM never establishing: coexistence back-off now pauses BLE scan for 5 s
  after repeated initiate failures, resumes on success/timeout
- Debug page unreachable via UP long-press (misread as OK on the ADC keypad):
  entry changed to UP double-click; distance display now IIR-smoothed;
  debug page shows the last FTM failure status code

### Added
- `tools/build-firmware.sh`: one-command delivery build producing the single
  release file `pass-radar_<版本>.bin` (bootable body + 44B MPUPV2 footer,
  same contract as meta-pass) with protected-layout verification
- Rotation-speed self-correction: harmonic-fit period search estimates the
  actual sweep completion time, trims post-stop samples and rescales the
  time→angle mapping when the user finishes the 360° rotation early
  (host-tested with 3s early-stop patterns)
- Debug/regression page (UP long-press from tracking): v1 RSSI-only distance
  vs v2 fused distance (A/B baseline), FTM quality, BLE sentinel, environment
  n and calibration reference, last vote spread
- Dual-end simultaneous sweep: the idle card auto-starts a sweep when the peer
  begins one (per sweep-id, ≤1s late), and the result fuses both angles via
  circular mean with the pair disagreement folded into the reported spread
- Result page low-confidence sector: a ±spread wedge marks direction
  uncertainty; distance error bands (±0.5m HIGH / ±2m MED) shown on the
  result and tracking pages
- Tracking page gates HIGH confidence to MED when the last channel-vote
  spread exceeds 20°
- BLE sentinel near/far grading: "FAR NEAR (BLE)" when sentinel RSSI >
  -75dBm, otherwise "FAR (BLE link)" (threshold pending device calibration)
- Environment estimator: sustained weak signal (long-term mean < -80dBm)
  selects the obstructed class (n=3.5) in addition to the indoor jitter rule
- Wi-Fi FTM (802.11mc RTT) ranging: symmetric SoftAP responder + periodic
  initiator sessions (every 2s, 16 bursts), median-filtered per-burst distances
- BLE 5.0 Coded PHY long-range sentinel link (NimBLE, 1s extended advertising +
  sparse passive scan): "FAR (BLE link)" hint beyond Wi-Fi range
- Fusion engine (`radar_fusion`): symmetric dual-end RSSI averaging, FTM/RSSI
  weighted fusion with reflected-path rejection, confidence levels
  (HIGH/MED/LOW/OOR), adaptive path-loss exponent (indoor/outdoor), circular
  angle statistics
- Channel-diversity sweep: 1/6/11 fast hopping (100ms dwell) with per-channel
  first-harmonic least-squares angle fitting and circular-median voting
- Parabolic-interpolated peak angle (sub-sample resolution) and peak-prominence
  gating for weak direction detection
- 1m two-card calibration (OK LONG on main page, 3s sampling, NVS persistence)
- Result page shows angle spread (±) and confidence; low-confidence results
  highlighted in warning color
- ESP-NOW protocol v2: bidirectional RSSI exchange, sweep-sync flag for
  follower channel hopping, result sharing (backward-compatible with v1)
- Host tests for fusion engine and new sweep algorithms

### Changed
- Wi-Fi mode STA → APSTA (required by FTM responder)
- Home channel fixed to channel 1; sweeps hop 1/6/11 and restore afterwards
- BLE re-enabled (NimBLE) with Wi-Fi/BLE software coexistence

## v0.1.0 (2026-09-13)

### Added
- Kalman-filtered RSSI tracking with polar radar UI
- 360° direction-finding sweep (body-shielding peak detection, 4s guided rotation)
- Distance estimation via free-space path loss model (outdoor n=2.2)
- ESP-NOW broadcast transmitter and receiver at 20Hz with max TX power (19.5dBm)
- Dual-role firmware: same binary on both devices, auto-pairing
- Weak-signal guard (RSSI < -88dBm shows "OUT OF RANGE")
- Three-page UI: tracking / sweeping / result
- meta-pass child firmware adaptation (mark_valid, LONG2 return to launcher)
- BSP button LONG2 event (3000ms)
- Host tests for Kalman filter and sweep algorithm

### Fixed (simulator testing)
- LVGL memory exhaustion: radar rings use single border-style circles (3 objects)
  instead of 135 dot objects; LVGL pool increased to 32KB
- Float printf unsupported in LVGL: distance displayed as integer decimeters
- UI init order: display built before ESP-NOW init for graceful degradation
- Coordinate system: panel-relative positioning for all radar dots
