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
