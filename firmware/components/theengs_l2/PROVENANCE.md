# Theengs L2 decoder provenance

This component is a clean-room selected-decoder implementation. It does not copy
Theengs Decoder GPL source or its device rule database, and it contains no
Runtime fingerprint matcher.

## Ruuvi RAWv2 (`THEENGS_DECODER_RUUVI_RAW_V2`)

- Classification: `CLEAN-ROOM REIMPLEMENT`
- Primary source: Ruuvi official Data Format 5 (RAWv2) specification
  - https://docs.ruuvi.com/communication/bluetooth-advertisements/data-format-5-rawv2
- Input contract: Manufacturer Specific Data payload after the Bluetooth SIG
  Company Identifier. The Device DB/AD parser has already selected this decoder.
- Golden vector: official Ruuvi valid RAWv2 vector
  `0512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F`.

## BTHome v2 (`THEENGS_DECODER_BTHOME_V2`)

- Classification: `CLEAN-ROOM REIMPLEMENT`
- Primary source: BTHome v2 data-format specification
  - https://bthome.io/format/
- Input contract: Service Data payload after UUID `0xFCD2`. The Device DB/AD
  parser has already selected the decoder and service-data field.
- Supported passive objects in this first bounded implementation:
  packet id, battery, temperature, humidity, pressure, illuminance, voltage,
  opening, CO2, moisture, motion, and button event.
- Encrypted BTHome advertisements are rejected as unsupported; this component
  performs no key handling or decryption.
- Golden vector: BTHome specification example payload after UUID,
  `4002C40903BF13` -> 25.00 C / 50.55 %RH.

## Theengs upstream relationship

Theengs Decoder and its device database were used only as research references
for the capability boundary. No upstream GPL code, JSON rules, or test corpus is
copied here. Expansion of the decoder set must retain per-decoder primary-source
provenance and the same explicit classification.
