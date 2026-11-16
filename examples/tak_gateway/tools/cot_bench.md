# Two-gateway CoT ordering bench

1. Flash two Heltec V3 gateways with the same firmware and put them on the same TAK group (certificates with read and write).
2. Name them differently (e.g. `GW-A` and `GW-B`) on the web page.
3. Set stale to 10 minutes on both.
4. Power a tracker so both gateways hear it. Confirm remarks show `role | GW-x | seen …` and that only one marker moves.
5. Pull Wi-Fi on GW-A for two minutes while the tracker keeps sending. GW-B should keep updating the marker.
6. Restore Wi-Fi on GW-A. It must **not** push an older fix: status should show superseded counts rising, and the map marker should keep GW-B's newer position / remarks.
7. Confirm "Sees other gateways' markers: yes" on both once they have received each other's traffic.
