# ViperLang Testing And Proof Guide

This file explains how tests connect to semantic state.

## Simple Script Tests

Many Viper tests are just runnable `.vp` scripts:

```bash
viper tests/scripts/std_test.vp
```

Use this style for focused behavior checks.

## Semantic Test Linkage With `@covers`

Tests can declare which source symbols they protect.

Current manifest style:

```viper
// @covers semantic_diff_after.vp read_user route

pr("semantic-link")
```

Guidelines:

- Put `@covers` near the top of the test file.
- Reference the target file and the important symbols.
- Keep coverage lists tight; do not dump unrelated names.

## State-Based Change Planning

Typical flow:

```bash
viper --emit-project-state=before.vstate --focus=read_user --impact before.vp
viper --emit-project-state=after.vstate --focus=read_user --impact after.vp
viper --emit-state-plan=before.vstate after.vstate
```

This gives semantic changes and linked tests.

## Targeted Test Execution

```bash
viper --run-state-plan=before.vstate after.vstate
```

This runs only the linked tests from the state diff when available.

## Persistent Proof Sidecars

To persist verification for later resume/query:

```bash
viper --run-state-plan=before.vstate after.vstate > after.vstate.vproof
```

Later commands can read that proof automatically:

- `viper --resume-project-state=after.vstate --focus=read_user --impact`
- `viper --query-project-state=after.vstate --query-name=read_user --impact`
- `viper --verify-project-state=after.vstate`
- `viper --refresh-project-state=after.vstate`

## Verification Fields

Proof-aware state output can include:

- `verified`
- `verified_tests`
- `verified_failed`
- `verification_stale`
- `verification_stale_symbols`
- `verification_stale_tests`

Meaning:

- `verified=yes` means there is a proof sidecar for the state
- `verification_stale=yes` means the proof exists but the workspace changed underneath it

## LLM Testing Rules

- Add `@covers` to tests that protect public or semantically important symbols.
- Keep test scope narrow so linked plans stay useful.
- After changing a focused symbol slice, prefer `emit-state-plan` and `run-state-plan` over broad reruns.
- When handing work to another model, persist `.vproof` when possible.
