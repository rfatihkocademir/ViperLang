# ViperLang Packaging And ABI Guide

This file explains `viper pkg` and package-facing contracts.

## Project Initialization

```bash
viper pkg init my_project
```

This creates project scaffolding such as:

- `viper.vpmod`
- `viper.lock`
- `.viper/packages`
- `docs/llm/*`

`viper.vpmod` is the project marker and package manifest.

## Package Commands

### Add a dependency

```bash
viper pkg add @web 0.1.0
```

### Remove a dependency

```bash
viper pkg remove @web
```

### Install dependencies

```bash
viper pkg install
```

### List dependencies

```bash
viper pkg list
```

### Build package artifacts

```bash
viper pkg build
```

### Lock dependencies

```bash
viper pkg lock
```

## Importing Packages

Typical pattern:

```viper
use "@web" as web

pr(web.webPing())
```

Prefer aliases for readability.

## Package Artifacts

`pkg build` emits package module artifacts such as:

- `.vbc`
- `.vbb`
- `.vabi`

The important LLM-facing one is `.vabi`.

## What `.vabi` Carries

The ABI records public symbols such as:

- kind
- name
- arity or metric
- typed parameter contract
- return type
- effect contract

Example ABI line:

```text
fn webEcho 1 params=msg:str ret=str eff=web
```

## ABI Commands

### Validate installed package ABIs

```bash
viper pkg abi check
```

### Compare versions

```bash
viper pkg abi diff @web 0.1.0 0.2.0
viper pkg abi diff @web 0.1.0 0.2.0 --fail-on-breaking
```

Use this after changing exported package APIs.

## LLM Packaging Rules

- If a function is package-public, give it a stable name.
- Prefer typed returns on exported functions.
- Add `@effect(...)` to exported side-effecting functions.
- Keep public surface small.
- After changing exported functions, run `viper pkg build` and `viper pkg abi check`.
- If comparing versions or preparing a release, run `viper pkg abi diff`.
