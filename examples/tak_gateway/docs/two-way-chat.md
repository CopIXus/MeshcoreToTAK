# TAK → MeshCore chat (removed)

The gateway is one-way: MeshCore positions and channel chat go to the TAK server, and nothing
from TAK goes back onto the mesh. An earlier build also carried GeoChat from TAK back to the
mesh. It was removed to keep the firmware simple and the heap free, because the TAK server in
testing never delivered the replies to the gateway.

The complete implementation is in commit
[`d5fe3fdb`](https://github.com/CopIXus/MeshcoreToTAK/commit/d5fe3fdb7a30af94f18415bba4ce8627ca93fc62)
("Harden the gateway for long runs and route chat replies over the read link"). Check it out,
or diff it against the commit after it, to bring the feature back.

## How it worked

### Two TLS links

- **Write link** (`/tak/client.pem`, `client.key`, port `tak_port`): markers and mesh chat.
- **Read link** (`/tak/rx-client.pem`, `rx-client.key`, port `rx_port`, default 8089): an
  optional second TAK Portal integration whose group the ATAK phones *send* into. It only
  received; the setup page had a separate "Read" certificate panel and `/api/rx-certs`
  (install) and `/api/rx-certs/delete` endpoints.
- Both links verified the server against one CA chain held in the esp_tls global CA store
  (`esp_tls_set_global_ca_store`), so the CA PEM was parsed once and freed.
- The read link opened only after the write link was connected, and only with at least
  70 KB free heap and a 24 KB contiguous block. It was dropped below 12 KB free, and it
  backed off for 2 minutes after a low-memory refusal. Each mbedTLS session on this
  Arduino-ESP32 build pins about 45–55 KB (fixed 16 KB in and out record buffers), and the
  Heltec V3 has no PSRAM.
- Each link negotiated the TAK protocol on its own (`t-x-takp-v` offer, `t-x-takp-q` request,
  `t-x-takp-r` answer) and could switch to protobuf framing (`0xbf`, varint length,
  `TakMessage`).

### Gateway presence

TAK Server routes chat for a uid to the connection that last announced that uid. The gateway
sent its own contact event every 20 seconds:

- type `a-f-G-U-C`, uid `meshcore-gw-<mac>`, callsign from the "Callsign" setting
  (`chat_callsign`, default "MeshCore GW"), placed at the gateway location;
- a `<groups>` element with one `<group name=ROOM uid=ROOM/>` per bridged room, so ATAK
  listed the gateway as an online member of each room (ATAK only sends into a room when a
  member is online). The room uid matches the `__chat id` that `TakCot::buildChat` uses.

While the read link was up, it carried the presence and the write link stayed publish-only.
Without a read link, the write link sent it.

### Inbound GeoChat

`TakClient::processRx` / `handleEvent` parsed `b-t-f` events in both framings:

- XML events were matched directly.
- Protobuf `CotEvent`s carry the detail as `xmlDetail` (field 15 → 1). That XML was
  unescaped if needed and wrapped as `<event type='b-t-f'><detail>…</detail></event>` so the
  XML path could handle it.

For each GeoChat:

1. Ignore our own echo (`chatgrp uid0` equals the gateway uid).
2. Find the MeshCore channel whose TAK room equals `__chat chatroom`, `__chat id`, or
   `remarks to`.
3. If none matched but the message was addressed to the gateway (its uid or callsign in
   `chatgrp uid1..uid7`, `<dest>`, `remarks to`, or the room name), send it to the first
   enabled channel.
4. Take the text from `<remarks>`, unescape XML entities, trim to whole UTF-8 characters.
5. Drop duplicates by an FNV-1a hash of `messageId` (last 8 kept), since both links could be
   sent the same message.
6. Queue it (4 entries). `MyMesh::loopGateway` popped one per loop, cut it so
   `"<sender>: <text>"` fits `MAX_TEXT_LEN`, and sent it with `sendGroupMessage` on that
   channel. The Public channel was never transmitted on.

The dashboard showed "mesh→TAK / TAK→mesh" counts and the read link state.

## Why it was removed

- In testing, with the gateway user in both the READ and WRITE groups and with separate
  integrations per direction, the TAK Server sent the gateway nothing on either link apart
  from ping replies, both for room messages and direct messages from ATAK and CloudTAK. That
  points at server-side group or data-feed routing, not at the gateway, and could not be
  fixed from the device.
- The second TLS session cost about 50 KB of a heap that also has to hold Wi-Fi, the web
  server and firmware update checks.

## Bringing it back

Start from the commit above. On the TAK Server side, first confirm that a plain ATAK or
CloudTAK client logged in with the read certificate actually receives the room's GeoChat;
if it does not, the gateway will not either. The config file still has the `rx_port` and
`chat_callsign` fields, so old settings load unchanged.
