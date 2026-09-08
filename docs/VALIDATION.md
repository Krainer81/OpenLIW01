# Validation summary

This is a condensed public summary; raw private field-evidence archives are intentionally not included.

## Counter / FRAM core
- More than 32 h long-run validation captured every consecutive total step in the validated interval.
- Controlled rollover through 255 -> 0 confirmed modulo behavior.
- Reboot and full power-loss persistence tests passed.
- Startup stale-output recovery was field observed and validated.

## GPIO5
- Observation-only and legacy-input forensic branches did not reproduce false counts after hardware rework under isolated handling.
- Final V0.5.0 state machine passed SHORT, CONFIG restart, NETWORK Wi-Fi cycle and FACTORY-arm/timeout tests without count/FRAM deltas.
- Production post-install control passed.

## UI / telemetry
- V0.5.1 runtime behavior passed; one V1 harness reported a false negative caused by suffix matching of an entity name.
- V0.5.2 RC2 custom web frontend read-only field regression passed with healthy counter/FRAM and no movement during the quiet interval.
- V0.5.2 RC3 is a presentation/reset-aware UI update over the same functional sections and has been visually/operationally accepted on-device.

### Reference hashes
- V0.5.0 production: `faa1b04a5f1ef9294bec8890618d67b555aebbbee2bbb9df34ae2da32105b335`
- V0.5.1 production: `70341267904fb79ed1cdc72db97af4d4dae17db1d7ccfd21009eb7c5eadaefb6`
- V0.5.2 RC2: `4e9fadd9b39f1136940914c965829da4d1856aba7e21d800e5cd490673da8d24`
- V0.5.2 RC3: `568340e7e0f1b0725f6ae0ec5b94cfb4544124f39ebd7ec26c3d20f1462f66be`
