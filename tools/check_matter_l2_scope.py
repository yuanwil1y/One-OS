#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware" / "components" / "matter_l2"

FORBIDDEN = {
    "DiscoverCommissionableNodes": "Matter L2 must not start generic/on-network discovery",
    "mdns_query": "Matter L2 must not own mDNS scanning",
    "mdns_browse": "Matter L2 must not own mDNS browsing",
    "ChipDeviceScanner": "Matter L2 must not own generic BLE candidate scanning",
    "nearby_": "Matter L2 must not create a generic nearby compatibility layer",
    "kismet_": "Matter L2 must not call Kismet L2",
    "wireshark_": "Matter L2 must not call Wireshark L2",
    "ha_mdns_": "Matter L2 must not call HA mDNS L2",
    "ha_core_": "Matter L2 must not call HA core L2",
    "openthread_": "Matter L2 must not call OpenThread L2",
    "zigpy_": "Matter L2 must not call zigpy L2",
    "zha_": "Matter L2 must not call ZHA L2",
    "theengs_": "Matter L2 must not call Theengs L2",
    "esphome_": "Matter L2 must not call ESPHome L2",
    "ExampleOperationalCredentialsIssuer": "production code must not use the connectedhomeip test issuer",
}

bad = []
for path in COMPONENT.rglob("*"):
    if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
        continue
    text = path.read_text(encoding="utf-8")
    for token, reason in FORBIDDEN.items():
        if token in text:
            bad.append(f"{path.relative_to(ROOT)}: forbidden {token!r}: {reason}")

if bad:
    print("Matter L2 scope check: FAIL")
    print("\n".join(bad))
    sys.exit(1)

print("Matter L2 scope check: PASS")
