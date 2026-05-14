## 2026-05-14 - LogicDriverKeeper: harden json fields on logicdriver component and node actions
**Malformed input pattern:** Optional JSON fields (booleans/numbers) were accessed via `HasField` followed by `GetBoolField`/`GetNumberField`, crashing if the type was wrong.
**Learning:** `HasField` does not guarantee type safety. Calling `GetBoolField` on a string or object causes an assertion failure.
**Prevention:** Always use `TryGetBoolField` and `TryGetNumberField` to explicitly reject wrong types by returning an `FMonolithActionResult::Error` before proceeding.
