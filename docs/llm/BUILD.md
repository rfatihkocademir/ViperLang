# ViperLang Build And Capability Guide

This file documents app packaging and runtime capability behavior.

## Build Command

```bash
viper build path/to/main.vp --out-dir=build --name=my_app
```

Current constraint:

- run this from the Viper source root, because the command expects the repository `Makefile`

## Build Outputs

The build command emits:

- `build/my_app/app.vbb`
- `build/my_app/viper-runtime`
- `build/my_app/capabilities.lock`
- `build/my_app/run.sh`

`run.sh` launches the app-specific runtime with the compiled bytecode.

## What `capabilities.lock` Means

Viper groups native/runtime usage into capability buckets:

- `os`
- `fs`
- `web`
- `db`
- `ai`
- `cache`
- `util`
- `meta`

These are build/runtime buckets, not exactly the same thing as public effect labels.

Important distinction:

- effect labels describe semantic contracts for functions
- capability buckets describe which native/runtime surfaces the built app needs

## Capability Inference

Build infers capabilities from used natives.

Examples:

- OS helpers activate `os`
- file/path helpers activate `fs`
- socket/http helpers activate `web`
- database helpers activate `db`
- AI provider helpers activate `ai`
- cache helpers activate `cache`
- math/time/text/array-heavy helpers can activate `util`
- meta/introspection helpers activate `meta`

## LLM Build Guidance

- Keep pure transformation helpers separate from side-effect entrypoints.
- Avoid mixing unrelated effect domains in one orchestration function.
- If only one small area needs DB or Web, isolate it so the app capability surface stays small.
- Prefer `@std/web` over raw network loops for app-facing HTTP code.
- Prefer stdlib wrappers over direct raw native usage.

## When To Use `build`

Use `viper build` when the model needs:

- a portable app artifact
- a runtime capability report
- a launcher script
- bytecode + runtime separation

Use direct `viper main.vp` when the model only needs local execution during development.
