# Jules Agent Cross-Domain Coordination Map

This file establishes clear file boundaries and domain coordination rules for Jules scheduled agents. It prevents cross-domain overlaps where agents of different categories (e.g., Performance, Docs, Infrastructure) race to touch the same files.

## Domain Boundaries

### 1. Code / Source Agents
- **Primary Targets:** `Source/*/*.cpp`, `Source/*/*.h`
- **Agents:** ParamGuard, IndexGuard, Crashguard, Sentinel (Refactoring/Security), Bolt (Performance), Domain Smiths (NiagaraSmith, MaterialSmith, BlueprintSmith, etc.)
- **Rules:**
  - Code refactoring, limit clamping, param hardening, and performance micro-optimizations must strictly reside in their designated `Source/` domain.
  - Do NOT modify `AGENTS.md` or `.jules/` files unless logging a strictly code-related durable learning in your specific `.jules/<agent>.md` file.

### 2. Documentation / Specification Agents
- **Primary Targets:** `README.md`, `Docs/`, `Skills/`
- **Agents:** SkillDocSmith, ReleaseNotesScribe, MCPContractAuditor
- **Rules:**
  - Count drift updates, spec parity, and action documentations reside here.
  - Docs agents must NOT modify `Source/` files or production `Build.cs` configs just to fix a documented count. Do not create mixed-domain PRs.

### 3. Agent Orchestration / Hygiene Agents
- **Primary Targets:** `AGENTS.md`, `.jules/`, `.github/`
- **Agents:** Marshal, Curator
- **Rules:**
  - Only orchestration agents may update the global `AGENTS.md` and repository-wide coordination policies.
  - Do NOT modify production code (`Source/`) unless implementing a tiny, explicitly justified agent-infra helper.

### 4. Build / Optional Dependency Agents
- **Primary Targets:** `*.Build.cs`, `.uplugin`
- **Agents:** OptionalDependencyScout, Forge
- **Rules:**
  - Dependency/Plugin guards and platform compilation flags belong here.
  - Must coordinate with Code/Source agents when modifying module paths.

## Overlap Prevention Rules
- **Task vs Domain Collision:** Domain Smiths (e.g., anim-weaver, ai-director, blueprint-smith) must yield generic cross-cutting tasks (like param hardening, limit clamping, array reservations) to dedicated task agents (e.g., ParamGuard, Crashguard, Bolt) to prevent redundant PRs in the same files.
- **Strict Single Responsibility:** If an agent finds a task that requires modifying files outside its primary domain (e.g., a ParamGuard agent trying to fix a rule in `AGENTS.md`), the agent MUST stop without a PR. Let the correct agent type handle it.
- **Micro-Edits on Shared Files:** Shared coordination files (`AGENTS.md`, release scripts) are high-collision zones. Agents MUST NO-OP if the only available work is a micro-edit against these shared surfaces, unless it's their specific duty.
