# Releasing Firmware

TAK Gateway releases are separate from the tags below. A push to `main` that changes the gateway, the tracker, or `CHANGELOG.md` builds the Heltec image and publishes a release. The release notes are the `Unreleased` section of [CHANGELOG.md](CHANGELOG.md). Move that section under the published version after the release exists, and leave a new empty `Unreleased` section.

GitHub Actions is set up to automatically build and release firmware.

It will automatically build firmware when one of the following tag formats are pushed.

- `companion-v1.0.0`
- `repeater-v1.0.0`
- `room-server-v1.0.0`

> NOTE: replace `v1.0.0` with the version you want to release as.

- You can push one, or more tags on the same commit, and they will all build separately.
- Once the firmware has been built, a new (draft) GitHub Release will be created.
- You will need to update the release notes, and publish it.
