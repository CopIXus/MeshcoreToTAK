# OTA rollback bench checklist

Short hardware pass to confirm gateway OTA rejects bad images and keeps the last good firmware.

## Hardware

- [ ] Heltec V3 with USB power and serial log (115200)
- [ ] Wi‑Fi AP the gateway already joins (or setup via USB first)
- [ ] USB cable for recovery if OTA fails badly

## Baseline

- [ ] Flash a **known-good** `tak_gateway_heltec_v3_full.bin` at offset 0x0
- [ ] Confirm web UI loads and **System → Firmware** shows current version
- [ ] Note `version.txt` line 1 (version) from the running release

## Broken build on a test tag

- [ ] Publish (or select) a **non-latest** release/tag with firmware that fails boot or checksum (CI size gate can be bypassed only for this manual test build)
- [ ] From the device UI, point OTA at that test tag or upload the bad `tak_gateway_heltec_v3.bin`
- [ ] Start update; watch serial for download, verify, and boot attempt

## Expect rollback / skip

- [ ] After failed boot, device returns to previous firmware (or refuses install with a clear error)
- [ ] Web UI and mesh/TAK behavior match the **pre-update** version
- [ ] Serial shows rollback or “skipped” — no brick; USB flash still works

## Record

- [ ] Versions before / after, tag tested, and pass or fail