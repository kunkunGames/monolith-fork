## 2025-02-06 - Harden audio/modify_sound_submix param parsing
**Malformed input pattern:** String passed to `output_volume_db` or `output_volume`
**Learning:** Checking `HasField` and directly calling `GetNumberField` causes a fatal engine crash if the client passes an unexpected type like a string.
**Prevention:** Always use `TryGetNumberField` for optional JSON number values to safely handle missing or invalid types.
