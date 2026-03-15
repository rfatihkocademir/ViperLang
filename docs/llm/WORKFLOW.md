# ViperLang LLM Workflow

## Principle

Do not start from full source if a semantic state already exists.

Preferred order:

1. resume/query state
2. inspect focused source only if needed
3. edit
4. regenerate state
5. plan and validate

## Recommended Flow For Existing Projects

### 1. Load the smallest useful context

If you know the symbol:

```bash
viper --resume-project-state=state.vstate --focus=target_symbol --impact --brief
```

If you do not know the symbol:

```bash
viper --query-project-state=state.vstate --query-name=keyword --impact --brief
viper --query-project-state=state.vstate --query-effect=os --impact
viper --query-project-state=state.vstate --query-call=callee_name
```

### 2. Read only the needed files

Use the state output to identify the few modules that matter.

### 3. Make the change

Prefer:

- small helper additions
- minimal public surface changes
- explicit effect annotations

### 4. Regenerate focused state

```bash
viper --emit-project-state=after.vstate --focus=target_symbol --impact path/to/main.vp
```

### 5. Compare semantic changes

```bash
viper --emit-state-plan=before.vstate after.vstate
```

### 6. Run only linked tests if available

```bash
viper --run-state-plan=before.vstate after.vstate
```

## Recommended Flow For New Modules

When starting from scratch:

1. define imports
2. define structs
3. write small typed helpers
4. add `@effect(...)` to meaningful functions
5. keep one orchestration entrypoint
6. generate state and benchmark the result

Useful command:

```bash
viper --emit-project-state=state.vstate --focus=entry_symbol --impact path/to/file.vp
viper --bench-project-state=state.vstate --focus=entry_symbol --impact
```

## How To Decide What To Expose

Good public API:

- stable name
- typed return
- clear effect contract
- small call surface

Bad public API:

- giant orchestration blob
- mixed IO and transformation
- hidden FFI or panic behavior

## What A Model Should Ask Itself

- Which symbol is the real entrypoint for this task?
- Which effects matter?
- Can I break this module into smaller semantically obvious helpers?
- Will state output stay focused after this change?

## Quick Heuristics

- If a function both fetches and formats, split it.
- If a function touches OS/FS/DB/Web, declare the effect.
- If a function returns a struct or important scalar, annotate the return type.
- If a file grows large, push helpers into a sibling module.

## Failure Rule

If execution fails with a bracketed code like `[VRT019]`:

1. map the code in `docs/llm/ERRORS.md`
2. inspect the smallest symbol slice related to that diagnosis
3. avoid debugging from the whole raw log
