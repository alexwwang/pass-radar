# Recheck of the Remediation Design After the Four Corrections

> Chinese peer: `pass-radar-v3-model-parameter-update-remediation-design-recheck.zh_CN.md`
> Date: 2026-09-23
> Reviewed: `pass-radar-v3-model-parameter-update-remediation-design.md` (version including the four corrections) and the Chinese peer document
> Method: with the v65 firmware code (`db55cc9`) as the factual baseline, each correction was verified for correctness, cross-document consistency, and newly introduced gaps; no business code was modified.

## 1. Recheck verdict

**All four corrections are correctly in place and consistent with code facts; the recheck surfaced and fixed three additional issues on the spot (see §3). The two language documents are now content-equivalent. Verdict: the remediation design is ready for v5 implementation.**

| # | Correction | Location (EN/CN) | Recheck verdict |
| --- | --- | --- | --- |
| F1 | Capacity upper bound derived from code constants (38 s ÷ 2 s = 19, capacity 32) | §3.4 / §3.4 | ✅ Correct; arithmetic rechecked |
| F2 | Sidecar time-alignment check (`DISTANCE_SIDECAR_MISALIGNED`) | §4.1 / §4.1 | ✅ Correct; recheck strengthened the time-anchor definition |
| F3 | Canonical CRC float-text authority = firmware output | §5.3 / §5.3 | ✅ Correct; matches ESP-IDF newlib reality |
| F4 | Two-phase header read (§5.4) + v5 layout table (§8.1) + NVS disclosure (§8.2) | §5.4/§8.1/§8.2 | ✅ Correct; layout numbers match the offsetof audit |

## 2. Per-item evidence (code facts)

### F1 Capacity derivation

Code constants (`main/radar_ftm.c`):

```text
FTM_PERIOD_US          = 2 s    (L22)   initiation gate on the success path
FTM_SESSION_TIMEOUT_US = 1.5 s  (L23)
FTM_BACKOFF_US         = 2 s    (L25)
FTM_PROBE_PERIOD_US    = 5 s    (L32)
FTM_PROBE_FRAMES       = 8      (L35)
```

Independent recomputation:

- Capture window = countdown 3 s + settle 5 s + running 30 s = 38 s;
- On the success path, completed sessions are spaced ≥ 2 s apart (`FTM_PERIOD_US`; L343 also checks backoff), so the worst case is floor(38/2) = **19** completed sessions;
- The failure path costs 1.5 s timeout + 2 s backoff = 3.5 s per attempt, roughly 10 in 38 s — slower, so not the worst case; the document states this correctly;
- Probes fire every 5 s and add no separate session bound;
- Capacity 32 = 19 + safety margin (≈1.68×); `RADAR_CAPTURE_AGG_MAX_SESSIONS 32u` costs 32×6 B = 192 B, within the §3.4 budget.

Verdict: F1's arithmetic, constant references, and the rebuttal of "about eight sessions" are all correct. The upstream document §10.3's stale claims were replaced with wording that points at this conclusion (including the disclosure that v4's capacity 16 does not cover the bound of 19).

### F2 Sidecar time alignment

- The original correction required a time-window overlap but did not define where the time anchor comes from;
- Code fact: the capture header's `boot_sequence` comes from `radar_diag_current_sequence()` (`radar_capture.c` L767) — a **run ordinal with no wall-clock**;
- Recheck strengthening (now in both documents): the time anchor is the USB-monitor timestamp of the run's export or the recorded export session; `boot_sequence` only verifies ordering/continuity of runs within a batch; entries without a time anchor are flagged `DISTANCE_SIDECAR_NO_TIME_ANCHOR` and rejected.

Verdict: F2's direction is correct; with the strengthening its semantics close.

### F3 Float-format authority

- The factual basis holds: ESP-IDF `printf` float output depends on the newlib configuration (nano formatting vs. full newlib `%f` differ), and output can change across configurations and versions;
- The rule "canonical bytes = the device's printed text verbatim, no PC reformatting, cross-version comparison groups by `firmware_version` and recomputes per group" is executable and matches the actual USB-export data flow;
- It correctly avoids the trap of a PC-side decimal convention that would make the CRC never match.

Verdict: F3 is correct and necessary.

### F4 Two-phase read / layout table / NVS disclosure

Two-phase read:

- Code fact: `scan_committed_records()` and the export path read the header with a single `sizeof(header)` transfer, and the NVS fallback `read_key()` (L249) likewise truncates at `sizeof(*header)` — the original §6.2/§6.5 finding stands, and the fix is necessary;
- The Chinese version defined a tolerant form (`header_size < sizeof(struct)` reads only the declared prefix); the recheck ported it to the English version and unified the rule as "strict or tolerant must be declared; the tolerant form only permits reserved-area appends." Both documents are now consistent;
- The tolerant form's motivation (a future v6 that only appends reserved-area fields stays readable by old firmware) is logically sound.

v5 layout table (§8.1), checked field-by-field against `radar_capture_batch.h` L182–L223:

| Check | Document claim | Code fact | Verdict |
| --- | --- | --- | --- |
| magic/version/header_size | 0–7 | L183–185 | ✅ |
| batch/run/capture id | 8–19 | L186–188 | ✅ |
| condition area 16 B (incl. 2 observed bytes) | 20–35 | L189–203 | ✅ |
| target/measured | 36–43 | L204–205 | ✅ |
| record_count…record_crc32 7×4 B | 44–71 | L206–212 | ✅ |
| ftm_raw 4 fields 12 B | 72–83 | L215–218 | ✅ |
| reserved[28] | 84–111 | L219 | ✅ |
| total size | 112 B | L222 static_assert | ✅ |

Recheck correction (see §3.1): new fields shrink from 10 B to 8 B (mode reuses existing bytes); remaining reserved corrected from 18 B to 20 B.

NVS disclosure (§8.2):

- Code fact: `valid_header()` L214 checks `version == CAPTURE_STORAGE_VERSION` strictly (currently 2); after the v3 bump, old blobs hit the L245 invalid_header rejection and are overwritten by the next save — the disclosed loss behavior is real;
- NVS is only the fallback when slot2 is unavailable (written only when save fails), so the "export fully before upgrading" procedure is reasonable;
- The batch path has its own version and a v3/v4-compatible read path; the document states this accurately.

## 3. Issues found during the recheck and fixed on the spot

### 3.1 The §8.1 draft made mode a new field (duplicated semantics)

The draft added an `observed_mode` field in the reserved area, but the condition area's existing `observed_level`/`observed_obstruction` bytes already carry last-sample semantics in v4 and are redefined as mode in v5 — one semantic in two storage locations that could disagree.

Fix: mode takes no new field; the existing condition-area bytes are redefined and interpreted by version. New fields total 8 B (probe_session_count, aggregate_flags, observed_first_last, observed_valid_samples), leaving 20 B reserved. Readers must never mix interpretations across versions.

### 3.2 English §5.4 disagreed with the Chinese read policy

The Chinese version defined a tolerant form (`<` reads the prefix); the English draft had only the strict form (`!= sizeof` rejects). Unified as "strict or tolerant, implementation must declare," consistent in both languages.

### 3.3 Stale claims and dead references in the upstream document

- The English parameter-update document referenced a nonexistent "§10" (that section exists only in the Chinese version); now points at §4.1.4 and the remediation design;
- The Chinese §10.3 claims "about 8 sessions max / capacity 16 has margin / PC weights by bursts" contradict the static-verification conclusions; replaced with the 19-session bound, the v4 defect disclosure, and "bursts are not a weight."

## 4. Reminders (non-blocking)

1. §3.6 defines `observed_*_mode` as the run's mode; the implementation must define the tie-break rule (suggested: whichever of first/last occurred later) in an implementation note or a follow-up document;
2. The layout table assumes the v5 header stays 112 B; if fields exceed budget during implementation, prefer narrowing the packed bit width of `observed_first_last` over exceeding 112 B;
3. The full field list of the `BATCH_CAPTURE_SUMMARY` line is frozen together with the parser when P1/P2 are implemented; this report does not predefine it.

## 5. Gates

```text
./tools/validate.sh --static  PASS (549 files scanned, full host suite PASS)
Bilingual document header cross-links: present
```

This round updated design and recheck documents only; no business code was modified and no new local commit was created.
