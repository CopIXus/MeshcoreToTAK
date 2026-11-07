# Changelog

The gateway version is the UTC time of the commit that built it, `YY.MMDD.HHMM`. A push to `main` that changes the gateway publishes a GitHub release with that version. The release notes are the **Unreleased** section below, copied by `.github/workflows/tak-gateway-release.yml`.

Write user-visible changes under `Unreleased` before merging to `main`. After a release is published, move those bullets under a heading named with the released version and leave `Unreleased` empty for the next one.

## Unreleased

### Gateway

- The Heltec hears group chat, tracker fixes, and adverts that are still addressed through a repeater. Repeaters do not rewrite the message. They add a path hash and a hop. The gateway reads the payload either way.
- A tracker line wrapped by a room or an extra name (`Room: Callsign: !MT1;...`) is still taken as a tracker fix. The callsign is the name written immediately in front of `!MT1`.
- Serial logs include the hop count and whether the flood carried a region. A packet that does not decrypt, or an advert whose signature fails, is logged instead of being silent.

### Tracker

- USB command `scope <region>` sends fixes inside a MeshCore flood region, which is what repeaters require when they drop an unscoped flood. `scope off` clears it. `status` and each send line show the region.
- On TennMesh East the region is `tn-east`. It is not the channel key. The gateway still has to have that channel open with tracker parsing on.
- Nothing is selected automatically. A build can set `-D TRACKER_DEFAULT_SCOPE=\"tn-east\"` to fill an empty scope on boot.
- Flash the image that matches the radio. `Tbeam_SX1276_meshcore_tracker` for the SX1276 T-Beam, `Tbeam_SX1262_meshcore_tracker` for the SX1262. The SX1262 image on an SX1276 board stops during radio start and the screen stays on "Loading...".

## Earlier

Tracker roles, the USB console, and sending a fix as soon as GPS locks are already on `main`. Their notes are the commit subjects from `36a562ff` through `e134ec87`.
