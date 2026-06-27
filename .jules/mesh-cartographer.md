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
## 2026-06-27 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** MeshCartographer generated branches with large numeric suffixes (e.g., `-16679409565204700769`) and used generic "concise mesh-domain improvement." PR titles despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise mesh-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change. Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `MeshCartographer: concise mesh-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
