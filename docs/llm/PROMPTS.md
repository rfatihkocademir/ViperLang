# ViperLang Prompt Pack

These prompts are intended to be copied into LLM system or task context.

## 1. Authoring Prompt

```text
You are writing ViperLang.

Optimize for semantic clarity, not clever syntax.
Use explicit imports, small helper functions, visible effects, and stable return types.
Prefer state-friendly code that compresses well into Viper semantic slices.

Rules:
- Use `var`, `fn`, `pub fn`, `st`, `ret`, `use`.
- Add type annotations where stable.
- Add `@effect(...)` on important functions.
- Prefer `<` and `>` patterns over `<=` and `>=` when possible.
- Prefer `0 - n` over bare negative literals in important paths.
- Keep one clear orchestration flow.
- Avoid hidden side effects and unnecessary FFI spread.
```

## 2. Refactor Prompt

```text
Refactor this ViperLang module for better semantic compression.

Goals:
- smaller public surface
- clearer helper names
- explicit effects
- typed returns for important functions
- minimal cross-module coupling

Do not add abstraction unless it improves state/query output.
Prefer simple control flow over dense expressions.
```

## 3. Existing Project Prompt

```text
Work state-first.

Before reading full source:
1. Resume or query the project state.
2. Identify the exact symbol slice.
3. Read only the modules needed for that slice.
4. After editing, regenerate state and compare semantic changes.

Use:
- `--resume-project-state ... --brief`
- `--query-project-state ...`
- `--emit-project-state ...`
- `--emit-state-plan ...`
- `--run-state-plan ...`
```

## 4. Review Prompt

```text
Review this ViperLang code for:
- hidden effects
- missing type annotations
- oversized functions
- weak module boundaries
- state-unfriendly patterns

Prioritize changes that improve semantic state quality and reduce future LLM context cost.
```
