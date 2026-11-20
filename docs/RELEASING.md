# Releasing

The upstream project shipped multi-board builds, merged binaries and a Pages demo
from GitHub Actions. This fork has one board and no CI: the workflows were
deleted because they built eight boards nobody here maintains. This file is the
process instead.

Everything below runs locally. The only thing GitHub is used for is hosting the
release asset that the frame downloads.

## Board

```
waveshare_photopainter_73
```

## Versioning, and the trap that bites

The firmware reads its version from git at CMake configure time
(`scripts/get_version.py`). Two consequences:

- **Tag before building.** If you build an untagged tree, the binary reports
  `dev-<sha>`.
- `version_compare` treats any `dev-*` build as **older than every release**.
  So a release binary built untagged will, once installed, keep seeing its own
  release as "newer" and can re-download it on every check. The version has to
  be baked in.

Either tag first:

```bash
git tag v1.0.0
python3 build.py --board waveshare_photopainter_73
```

or force it:

```bash
python3 build.py --board waveshare_photopainter_73 -- -DFIRMWARE_VERSION=v1.0.0
```

## Build

```bash
python3 build.py --board waveshare_photopainter_73
```

That runs three steps: build the web UI (`npm run build` in `webapp/`), generate
the splash screens, then `idf.py build`. Individual steps:

```bash
python3 build.py --board waveshare_photopainter_73 --step webapp
python3 build.py --board waveshare_photopainter_73 --step splash
python3 build.py --board waveshare_photopainter_73 --step firmware
```

The splash step needs either `rsvg-convert` or `cairosvg`; it falls back
automatically.

## Publish the release

The OTA client looks for one asset, named exactly:

```
esp32-photoframe-<BOARD_HAL_NAME>.bin
```

For this board:

```bash
cp build/esp32-photoframe.bin esp32-photoframe-waveshare_photopainter_73.bin

gh release create v1.0.0 \
  esp32-photoframe-waveshare_photopainter_73.bin \
  --title "v1.0.0" \
  --notes "What changed in this build"
```

Requirements:

- The repository must be **public**; the device fetches releases anonymously.
- The asset name must match the board exactly, or the update fails with
  "No .bin file found in release assets".
- `main/config.h` → `GITHUB_API_URL` must point at **this** fork, not upstream.
  Changing it requires one USB flash, because a device cannot discover a new
  release repository by itself.

## USB recovery image (optional)

For a board that will not boot, merge the pieces into one image for `esptool`:

```bash
esptool.py --chip esp32s3 merge-bin \
  -o esp32-photoframe-waveshare_photopainter_73-merged.bin \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x20000 build/esp32-photoframe.bin
```

## Verification checklist

After publishing, before telling anyone an update exists:

1. `POST /api/ota/check` → `update_available: true`, `latest_version` as tagged.
2. Record the device's config: API key present, model order, place, template,
   rotation mode.
3. `POST /api/ota/update`; watch the serial log through download, write and
   reboot.
4. `GET /api/system-info` → version equals the tag, **not** `dev-…`.
5. Re-read the config and compare with step 2. NVS is a separate partition, so
   everything should survive.
6. `POST /api/ai/daily` → the frame generates and displays, proving the key and
   model order still work.

## Known risk: no rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is **not** set. An image that bootloops
will not revert itself; the frame needs reflashing over USB. That is acceptable
while the frame is on a desk and unacceptable for a handed-over frame, so enable
rollback before treating OTA as a remote-repair mechanism.

There is also no signature check: anyone able to publish a release to this
repository can push firmware to the frame.
