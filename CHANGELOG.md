# Changelog

The gateway version is the UTC time of the commit that built it, `YY.MMDD.HHMM`. A push to `main` that changes the gateway publishes a GitHub release with that version. The release notes are the **Unreleased** section below, copied by `.github/workflows/tak-gateway-release.yml`.

Write user-visible changes under `Unreleased` before merging to `main`. After a release is published, move those bullets under a heading named with the released version and leave `Unreleased` empty for the next one.

## Unreleased

### Gateway

- The Heltec hears group chat, tracker fixes, and adverts that are still addressed through a repeater. Repeaters do not rewrite the message. They add a path hash and a hop. The gateway reads the payload either way.
- A tracker line wrapped by a room or an extra name (`Room: Callsign: !MT1;...`) is still taken as a tracker fix. The callsign is the name written immediately in front of `!MT1`.
- Serial logs include the hop count and whether the flood carried a region. A packet that does not decrypt, or an advert whose signature fails, is logged instead of being silent.
- `status` counts flood packets that already have at least one hop, split into unscoped (`flood`) and scoped (`flood-scope`). The same counts are on the setup page under Mesh packets. Serial `regions name,name` checks each scoped flood against those names and prints which one matches the transport code.
- A tracker that reboots and starts its sequence over is still plotted. A small step backward is still dropped as a replay.

### Tracker

- Leave `scope` off unless every repeater on the path has that region with flood allowed. A scoped flood is dropped by a repeater that does not have the region. `scope off` sends an ordinary flood, which stock repeaters forward. `status` warns when a scope is set.
- `scope <region>` keeps the name's case. Repeater region names are case-sensitive. A `$` private region is rejected, because its key is not the hash of the name.
- GPS stays on. A long-press on the GPS page, or the phone app setting GPS off, is turned back on and logged.
- The Tracker screen shows `NO FIX` and how many minutes the fix has been gone. `status` prints fixes sent, radio transmissions, airtime, repeater echoes, and other flood traffic heard with hops. `status log` prints the trip buffer (fix lost and acquired, sends, echoes, GPS baud, battery, scope).
- An echoed fix (the same ciphertext heard again with one or more hops) prints `echo N hop`. Three scoped sends with no echo, while other multi-hop flood traffic is arriving, prints a line telling you to try `scope off`.
- Nothing is selected automatically. A build can set `-D TRACKER_DEFAULT_SCOPE=\"name\"` to fill an empty scope on boot. Do not set that unless the repeaters actually carry that region.
- Flash the image that matches the radio. `Tbeam_SX1276_meshcore_tracker` for the SX1276 T-Beam, `Tbeam_SX1262_meshcore_tracker` for the SX1262. The SX1262 image on an SX1276 board stops during radio start and the screen stays on "Loading...".

## Earlier

Tracker roles, the USB console, and sending a fix as soon as GPS locks are already on `main`. Their notes are the commit subjects from `36a562ff` through `e134ec87`.
