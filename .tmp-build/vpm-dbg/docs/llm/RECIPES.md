# ViperLang App Recipes For LLMs

These are recommended shapes for common applications.

## 1. CLI Tool

Good fit:

- `@std/os`
- `@std/fs`
- typed helper functions

Shape:

1. parse args
2. read inputs
3. transform data in pure helpers
4. print or write outputs

Good focus symbol:

- `main`
- `run_cli`

## 2. File Processor

Good fit:

- `@std/fs`
- `@std/io`
- `@std/time`

Pattern:

- one loader helper
- one transform helper
- one writer helper

Effects usually:

- `fs`

## 3. Web API

Good fit:

- `@std/web`
- `@std/db` when needed
- `@std/cache` when needed

Pattern:

- route helpers stay thin
- validation/formatting helpers stay pure
- DB calls stay in isolated functions

Effects usually:

- `web`
- `db`
- `cache`

## 4. DB Worker Or Sync Job

Good fit:

- `@std/db`
- `@std/fs`
- `@std/time`

Pattern:

- one entrypoint for orchestration
- small query helpers
- small normalization helpers
- clear transaction boundaries

Effects usually:

- `db`
- `fs` if it also writes files

## 5. AI Pipeline

Good fit:

- `@std/ai`
- `@std/cache`
- `@std/fs`

Pattern:

- prompt builder helper
- provider call helper
- response parser helper
- cache helper

Effects usually:

- `ai`
- `cache`
- `fs` if prompts/responses are stored

## 6. Plugin Or FFI Adapter

Good fit:

- raw `load_dl(...)`
- very small wrapper module

Pattern:

- isolate all FFI in one module
- expose only a narrow typed wrapper API
- keep business logic outside the adapter

Effects usually:

- `ffi`

## 7. State-First Maintenance Loop

For existing apps, the LLM should work like this:

1. `--resume-project-state` or `--query-project-state`
2. read only the few files named by the state
3. edit a focused slice
4. `--emit-project-state`
5. `--emit-state-plan`
6. `--run-state-plan`

This is the default maintenance recipe for Viper projects.
