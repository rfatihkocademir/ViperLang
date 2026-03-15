# ViperLang Tooling For LLMs

This file describes the CLI surface an LLM should use to understand and maintain a Viper project.

Rule:

`Prefer semantic state commands over opening large source trees.`

If a command fails with a bracketed error code like `[VFF005]`, resolve the code through `docs/llm/ERRORS.md` before opening more source.

## Run Source

```bash
viper path/to/main.vp
```

Use this for direct execution of a source file.

## Emit And Run Bytecode

```bash
viper --emit-bytecode=out.vbb path/to/main.vp
viper --run-bytecode=out.vbb
```

Use this when the model needs a portable compiled artifact or build pipeline step.

## Emit Context Pack

```bash
viper --emit-context-pack --focus=target_symbol --impact path/to/main.vp
```

Use this mostly for legacy/debug context output.

Preferred modern path:

- emit state
- resume/query from state

## Emit Project State

```bash
viper --emit-project-state=state.vstate --focus=target_symbol --impact path/to/main.vp
```

Use this as the main source-to-state step.

Important flags:

- `--focus=symbol` keeps the slice targeted
- `--impact` adds caller-side blast radius

## Resume Project State

```bash
viper --resume-project-state=state.vstate --focus=target_symbol --impact --brief
```

Use this when the symbol is already known and the model wants the smallest useful handoff.

Important flags:

- `--focus=symbol`
- `--impact`
- `--brief`

`--brief` is the default low-token handoff mode for LLM context.

## Query Project State

```bash
viper --query-project-state=state.vstate --query-name=read_user --impact --brief
viper --query-project-state=state.vstate --query-effect=os --impact
viper --query-project-state=state.vstate --query-call=read_user
viper --query-project-state=state.vstate --query-name=render --query-deps
```

Use this when the symbol is not known upfront.

Query styles:

- `--query-name=...` by symbol name
- `--query-effect=...` by effect label
- `--query-call=...` by callee

Expansion styles:

- `--impact` expands callers
- `--query-deps` expands dependencies/callees

`--brief` works here too and reduces ledger size.

## Verify State Freshness

```bash
viper --verify-project-state=state.vstate
```

Use this before trusting a cached state artifact after workspace changes.

## Refresh State

```bash
viper --refresh-project-state=state.vstate
```

Use this when the state exists but may be stale.

Important behavior:

- if only file hashes changed and semantics stayed the same, refresh can report a no-op style result
- if a proof sidecar exists, refresh can also report proof freshness

## Benchmark Handoff Size

```bash
viper --bench-project-state=state.vstate --focus=target_symbol --impact
viper --bench-project-state=state.vstate --query-name=read_user --impact
```

Use this to compare full ledger vs brief ledger bytes/token estimates.

Important outputs:

- `full_bytes`
- `brief_bytes`
- `full_tokens_est`
- `brief_tokens_est`
- `saved_pct`

## Semantic Diff From Source

```bash
viper --emit-semantic-diff=before.vp --focus=target_symbol --impact after.vp
```

Use this when source files are available but state artifacts are not.

## State Plan

```bash
viper --emit-state-plan=before.vstate after.vstate
```

Use this to compare two states and get:

- semantic changes
- change plan
- linked tests

## Run State Plan

```bash
viper --run-state-plan=before.vstate after.vstate
```

Use this to run only linked tests from the semantic diff.

If you want a persistent proof sidecar, redirect it:

```bash
viper --run-state-plan=before.vstate after.vstate > after.vstate.vproof
```

## Proof Sidecars

When `after.vstate.vproof` exists next to `after.vstate`:

- `resume`
- `query`
- `verify`
- `refresh`

can surface verification metadata without rereading source.

Important outputs:

- `verified`
- `verified_tests`
- `verified_failed`
- `verification_stale`
- `verification_stale_symbols`
- `verification_stale_tests`

## LLM Working Order

For existing repos, prefer this order:

1. `--resume-project-state` or `--query-project-state`
2. inspect only the modules named by the state
3. edit source
4. `--emit-project-state`
5. `--emit-state-plan`
6. `--run-state-plan`

Avoid starting from full source unless state is missing or clearly wrong.
