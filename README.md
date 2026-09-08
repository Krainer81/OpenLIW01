# OpenLIW01

Unofficial, independent ESPHome replacement firmware for the **ZAMEL LIW-01** water-meter interface hardware.

Current public candidate: **V0.5.2 RC3**. The project is intended for owners of compatible hardware who want local operation, Home Assistant integration, persistent metering, a guarded local-service button, and an embedded local web UI.

> **Unaffiliated project.** ZAMEL, SUPLA and related names/trademarks belong to their respective owners. This repository is not endorsed by or affiliated with them.

## What is included

- Independently developed ESPHome/YAML firmware and custom component source.
- Persistent counter state in external FRAM with A/B records and guarded writes.
- Home Assistant API integration.
- Local authenticated web UI with embedded JS/CSS; no cloud dependency is required for the UI.
- Guarded service operations and a release-only local-button state machine.
- Documentation of the hardware/software interface needed to use this independent implementation.

## What is intentionally **not** included

This public repository deliberately excludes:

- original/vendor firmware images (`.bin`), dumps or extracted firmware contents;
- disassembly, decompilation output, lifted vendor source code, or vendor web assets;
- PCB photographs, Gerbers, board scans or reconstructed production artwork;
- vendor logos and copyrighted marketing artwork;
- private field-evidence archives, credentials, local network secrets or personal data.

The public scope is intentionally limited to the independent implementation, functional interface facts, validation summaries, and safe operating notes.

## Current status

- Counter/FRAM/startup core: field validated over long-run, reboot and power-loss tests.
- GPIO5 local-service input: field validated with active-low input, internal pull-up, interrupt mode and release-only action bands.
- V0.5.1 telemetry cleanup: operationally validated; a reported test failure was traced to a test-harness entity-name matching bug rather than firmware behavior.
- V0.5.2 RC2 custom web frontend: read-only field regression PASS.
- V0.5.2 RC3: presentation/reset-aware UI polish over the same functional backend; user-accepted on device. Functional firmware sections are unchanged from RC2.

## GPIO5 service bands

| Hold time | Action |
|---|---|
| `< 0.20 s` | Ignored |
| `0.20 – <4 s` | Status/diagnostic only |
| `4 – <8 s` | Normal ESP restart after release |
| `8 – <15 s` | Wi-Fi radio OFF for 5 s, then ON; no ESP reboot |
| `>= 15 s` | Factory-reset confirmation is armed only |

A factory reset requires a **second SHORT press within 10 s**. Any other band or timeout cancels it. Before confirmation the firmware requires healthy FRAM persistence. The local-config factory reset does **not** clear the external FRAM total. GPIO5 never performs a counter reset and never clears the hardware pulse counter.

## Install

1. Copy `firmware/liw01-analyzer.yaml`, `firmware/liw01_web.js`, `firmware/liw01_web.css` and `firmware/components/` into your ESPHome configuration directory.
2. Copy `firmware/secrets.example.yaml` to `secrets.yaml` and set your own values.
3. Review the static IP and hardware assumptions in the YAML before compiling.
4. Compile with a current ESPHome release compatible with ESP8266/Arduino.
5. Keep a recovery path available before changing destructive service settings.

## Safety / design rules

- Do not use a legacy combined hardware diagnostic value as the authoritative total.
- Do not write to the preserved legacy FRAM region.
- Do not probe the pulse-output signal on a live installation unless you fully understand the loading/coupling risk; project testing previously showed instrumentation could influence pulse behavior.
- Keep counter reset, local-config reset and network recovery as separate guarded operations.
- Do not change the validated counter/FRAM core to mask suspected electrical or hydraulic faults.

See [`docs/SAFETY.md`](docs/SAFETY.md) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Legal / publication note

This repository is structured to publish the **independent implementation**, not the vendor firmware. In the EU, Directive 2009/24/EC distinguishes protected program expression from underlying ideas/principles and contains limited rights/exceptions for observation/study/testing and, under conditions, decompilation necessary for interoperability. Czech law implements corresponding rules in Act No. 121/2000 Coll., §65–66. Those rules are fact-specific and do not eliminate other possible issues such as contract, trademarks, patents, trade secrets or anti-circumvention law.

See [`docs/LEGAL_NOTES.md`](docs/LEGAL_NOTES.md). This is project documentation, **not legal advice**.

## License

No public open-source license has been selected yet. Until a `LICENSE` file is intentionally added, normal copyright defaults apply. Choose a license only after deciding how you want third parties to use and redistribute the independent code.
