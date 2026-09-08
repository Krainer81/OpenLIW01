# Architecture

## Measurement path

The firmware polls the hardware counter at 100 ms and maintains an authoritative total independently of the Home Assistant/web publication cadence. UI cleanup must never reduce the measurement cadence.

The production counter logic uses an 8-bit modulo delta, accepts ordinary deltas in a guarded range, and contains a narrowly scoped startup stale-output recovery path. The combined legacy hardware diagnostic value is not used as the total.

## Persistence

External FRAM uses redundant A/B records with sequence/generation, validation and commit semantics. The legacy/original data area is treated read-only by the independent implementation. Normal accepted counter events are persisted; service operations use guarded redundant commits.

## Presentation layers

- ESPHome native API for Home Assistant.
- Embedded authenticated HTTP service UI.
- V0.5.2 uses small local JavaScript/CSS assets served from firmware and the standard ESPHome v3 event/REST backend.
- No external CDN is required for the custom UI.

## Service separation

Counter reset, local configuration reset, Wi-Fi recovery and normal restart are separate operations. A local-config reset must not be treated as a metering reset.
