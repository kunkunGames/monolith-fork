## 2024-05-04 - ValidatePackagePath on unguarded CreatePackage calls in MonolithAudio

**Vulnerability pattern:** `CreatePackage` asserts when given malformed package paths with double leading slashes (e.g. `//Game/...`), killing the entire Unreal Editor. This was previously identified and documented in `MonolithPackagePathValidator.h` for `MonolithGAS` and `MonolithAI`.
**Learning:** Any file dealing with JSON-RPC string payloads calling `CreatePackage` (in this case `MonolithAudio`) must manually insert a `MonolithCore::ValidatePackagePath` check before the call. It does not crash cleanly; it asserts fatally.
**Prevention:** Include `"MonolithPackagePathValidator.h"` and call `MonolithCore::ValidatePackagePath` on user paths prior to passing them to `CreatePackage` or any factory method.
