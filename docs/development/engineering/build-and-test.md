<p align="right">
  <a href="build-and-test.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Build and Test

Use ESP-IDF 5.5.3. On a clean machine or when the toolchain is missing, follow
the [environment bootstrap](environment-setup.md) first.

> Prefer `tools/build-firmware.sh` for delivery builds: it produces the single
> release file `build/pass-radar_<版本>.bin` (bootable body + 44B MPUPV2
> footer, same contract as meta-pass) and verifies the protected layout. Flash
> that one file at offset `0x0` — it serves device testing, install, and the
> upgrade channel alike. `idf.py build` and `idf.py flash` remain incremental
> development commands, not the delivery path.

```bash
source <path-to-esp-idf-v5.5.3>/export.sh
idf.py --version             # must report ESP-IDF v5.5.3
./tools/validate.sh --firmware # preferred: build and verify merged 0x0 image
idf.py set-target esp32c3     # fresh checkout or changed target
idf.py build                  # optional incremental application build
idf.py flash monitor          # optional incremental application flash
idf.py fullclean              # remove stale generated build state only
```

`idf.py fullclean` does not fully synchronize an existing `sdkconfig` with
changed defaults. Preserve intentional local settings, then run
`idf.py set-target esp32c3` when the target or tracked defaults must be
regenerated.

The tracked `dependencies.lock` pins Managed Component resolution. After changing an `idf_component.yml`, regenerate the lock with ESP-IDF 5.5.3, review version changes, and commit it with the manifest. An ordinary build must not leave an unexplained lock-file diff.

Firmware validation (`tools/validate.sh --firmware`) uses a fresh temporary build directory and an isolated `sdkconfig` generated from the tracked defaults. It does not consume or overwrite a developer's root `sdkconfig`, and it copies only the verified merged image to `build/pass-radar-full.bin`. The gate also enforces the [protected Flash layout](protected-flash-layout.md): the protected `cardid` address, application size, partition-table MD5, and absence of device-specific identity data.

The baseline also has a hardware-independent logic test:

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_ui_pixel_math.c main/ui_pixel_math.c \
  -o /tmp/test_ui_pixel_math
/tmp/test_ui_pixel_math
```

Use the unified validation entry point:

```bash
# The existing uv-created ESP-IDF environment is selected before activation.
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.10_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
source <path-to-esp-idf-v5.5.3>/export.sh
./tools/validate.sh --static    # repository checks, workflows, links, secrets, host tests
./tools/validate.sh --firmware  # build, merge-bin, offsets, and protected layout
./tools/validate.sh             # complete gate; requires ESP-IDF v5.5.3
```

`tools/validate.sh` requires a uv-created Python virtual environment and prints
its selected interpreter. Set `PASS_RADAR_PYTHON` to an explicit Python
executable when the environment is not on `PATH`; the script fails before tests
if it cannot prove that the interpreter is uv-managed. CI calls the same script,
so provide an equivalent uv environment there rather than silently falling back
to the system `python3`. Fix the shared script or environment if local and CI
behavior differs; do not duplicate command sequences in workflows.

Hardware-affecting changes must also run the applicable on-device checklist in the hardware guide. Report compilation separately from physical-device validation.

Never upload intermediate artifacts (`build/pass-radar.bin`,
`build/pass-radar-full.bin`) to the community. The only release file is
`build/pass-radar_<版本>.bin` from `tools/build-firmware.sh` — one firmware
file for all scenarios, mirroring meta-pass.
