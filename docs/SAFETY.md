# Safety rules

1. Never use the legacy combined diagnostic counter value as the authoritative meter total.
2. Do not write to the preserved legacy FRAM area.
3. Do not alter the validated 100 ms counter/persistence core without concrete new field evidence.
4. Keep counter reset and local-config reset separate.
5. GPIO5 actions are classified only after button release; no destructive action occurs during the hold or at boot.
6. Factory reset requires a second short confirmation and healthy persistence state.
7. GPIO5 never resets the meter counter and never clears the hardware pulse counter.
8. Avoid probing the live pulse-output path; instrumentation can alter a sensitive signal path.
9. When diagnosing unexpected counts, first exclude real hydraulic flow before attributing events to electronics.
