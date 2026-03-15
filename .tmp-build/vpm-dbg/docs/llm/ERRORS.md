# ViperLang Error Codes For LLMs

Use this file first when a Viper log contains `[CODE]`.

Rule:

`Treat the code as the primary signal. Treat the message text as secondary.`

## Fast Workflow

1. read the bracketed code
2. map it in this file
3. inspect the tiny set of likely causes
4. open `docs/ERROR_CODES.md` only if more detail is needed

## High-Value Codes

| Code | Diagnosis | First Fix Step |
| --- | --- | --- |
| `VVM002` | bad IP / corrupted control flow | inspect recent jump lowering, callback frames, bytecode emission |
| `VVM003` | division by zero | inspect divisor source and guards |
| `VRT009` | function arity mismatch | compare call-site args with function signature |
| `VRT015` | undefined struct field | inspect field spelling and struct definition |
| `VRT016` | native disabled by profile | inspect capability/profile mismatch |
| `VRT019` | array index out of bounds | inspect loop bounds and index source |
| `VRT021` | invalid index target | ensure target is array or instance |
| `VRT027` | eval got non-string | inspect eval caller |
| `VRT028` | eval parse failed | inspect generated source text |
| `VRT029` | eval compile failed | inspect unsupported construct in eval source |
| `VTP001` | runtime type assertion mismatch | inspect declared type vs actual value |
| `VNT002` | shared library load failure | inspect path and loader visibility |
| `VNT010` | env key not string | inspect `os_env(...)` caller |
| `VNT011` | invalid `os.cron` schedule | rewrite using `*`, `*/N`, or `@every ...` |
| `VFF002` | symbol missing in shared library | inspect exported C symbol name |
| `VFF003` | malformed FFI signature | ensure `args->ret` format |
| `VFF004` | illegal FFI type use | inspect return type and illegal `v` arg |
| `VFF005` | garbage after return type | remove trailing chars in signature |
| `VFF008` | missing comma in FFI args | separate types with commas |
| `VFF009` | unknown FFI arg type | use only supported ffi tokens |
| `VAI001` | unsupported AI provider | use `openai` for `ai.ask` |
| `VCL001` | entry/source file open failed | verify CLI path before reading more code |
| `VBL001` | build entry missing | fix build command entry path |
| `VBL009` | runtime build failed | inspect build toolchain / Makefile execution |
| `VIX008` | import resolution failed in indexer | inspect focused module import path first |
| `VIX010` | requested focus symbol not found | query available symbols before opening source |
| `VIX014` | state format invalid | regenerate `.vstate` with current Viper |
| `VIX030` | state missing symbol ledger | regenerate state; do not trust resume/query output |
| `VIX033` | state query missing filter | add `--query-name`, `--query-effect`, or `--query-call` |
| `VIX048` | state refresh failed | inspect tracked modules / state integrity |
| `VIX065` | semantic diff inputs incomplete | provide before, after, and focus symbol |
| `VPK023` | invalid package name | fix CLI package identifier first |
| `VPK025` | package project root missing | run inside project or `viper pkg init` |
| `VPK033` | package build failed | inspect install/build pipeline, not app source |
| `VPK036` | package ABI mismatch | inspect generated package artifacts and ABI drift |
| `VPK040` | breaking ABI change detected | inspect diff output and bump version / contract |

## Category Hints

- `VVM...`: interpreter / bytecode issue
- `VRT...`: user code violated runtime contract
- `VTP...`: user code hit runtime type assertion
- `VNT...`: stdlib/native misuse or system failure
- `VFF...`: FFI declaration/setup failure
- `VAI...`: AI native surface limitation
- `VCL...`: top-level CLI file or argument failure
- `VBL...`: app build pipeline failure
- `VIX...`: semantic state / index / query / diff failure
- `VPK...`: package manager / ABI workflow failure

## Minimal Prompt Pattern

When a model sees a coded error, summarize it like this:

```text
Observed: VRT019
Meaning: array index out of bounds
Likely zone: caller loop or index expression
Next action: inspect the focused symbol and its callers, not the whole project
```

## State-First Rule

If a coded error happens in an existing project:

1. `viper --resume-project-state=state.vstate --focus=symbol --impact --brief`
2. if symbol is unknown, use `--query-project-state`
3. only then inspect source

## Detailed Reference

- `docs/ERROR_CODES.md`
