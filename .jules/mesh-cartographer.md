# MeshCartographer PR Guidance

## PR Intent
MeshCartographer PRs improve MonolithMesh parameter safety and spatial/geometry action robustness while keeping mesh operation behavior stable.

## Code Work Improvements
- Treat large mechanical `HasField` rewrites as high-risk. Review resulting control flow after edits, not only search/replace output.
- Keep `if`/`else` pairing intact when wrapping optional fields in scoped blocks. A bare block followed by `else` is a compile blocker.
- Preserve GeometryScript operation selection semantics exactly unless the PR is a feature change.
- For mesh actions, wrong-type input should fail before any actor, asset, or dynamic mesh mutation.

## Review Gate
- Compile the module for any broad mesh param parsing PR.
- Manually inspect each edited `if`/`else` region around geometry mutations.
2026-05-24 - Parameter Type Safety in Mesh Operation Actions
Malformed input pattern: Mesh simplify actions used HasField with GetNumberField directly, crashing or returning 0.0 for string types.
Learning: Checking HasField alone does not validate the type of an optional parameter before geometric mutation.
Prevention: Replace unguarded GetNumberField with TryGetNumberField to explicitly reject wrong-type JSON fields with FMonolithActionResult::Error.
## 2026-06-03 - MeshCartographer type-safety in Mesh Operation Actions
Malformed input pattern: Mesh operation actions used GetStringField and GetBoolField blindly which crashes on bad payloads (e.g. providing an integer where a string is expected).
Prevention: Use TryGetStringField and TryGetBoolField to handle missing properties or incorrect types safely before any GeometryScript API calls. If the payload is bad, return FMonolithActionResult::Error gracefully.
