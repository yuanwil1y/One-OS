# ha_core

Bounded RAM-only Home Assistant Device / Entity / State / Service semantics for One-OS.

This component is adapted from the project-owned predecessor implementation in
`yuanwil1y/NearBy-One-NEXT/components/ha_core` and preserves its fixed-pool,
single-owner-task model. The public API was narrowed and renamed to the approved
`ha_core_*` scope in One-OS. It deliberately contains no Device DB matcher,
protocol scanner, persistence layer, or cross-project L2 types.

Unknown/unmatched physical observations can be represented as ordinary Devices;
manufacturer/model/profile data are optional. Writable service dispatch is only
available when the application explicitly installs a handler and supported-service
mask on an Entity.
