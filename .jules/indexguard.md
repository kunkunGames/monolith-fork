## 2025-02-23 - Bound ProjectFindByType limits and offsets
**Query contract:** ProjectFindByTypeAction limit and offset parameters
**Learning:** Limit and offset provided by the user must be explicitly clamped using FMath::Clamp and FMath::Max before being passed down to subsystem and DB handlers, to prevent unbounded DB scanning and negative array slicing.
**Prevention:** Always sanitize limit and offset parameters with FMath::Clamp(limit, 1, max_val) and FMath::Max(0, offset) in the Action layer.
