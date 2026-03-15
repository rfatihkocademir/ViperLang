# ViperLang Model Spec

## Purpose

ViperLang is not primarily trying to be the shortest source language for humans.

Its main value is:

- make code analyzable
- make semantics explicit
- make project slices resumable for LLMs
- make focused semantic handoff smaller than raw source

Core rule:

`In Viper, the most important artifact for an LLM is often not the source file itself, but the semantic state derived from it.`

## Companion Docs

Use these alongside this file:

- `STDLIB.md` for real module surface
- `TOOLING.md` for state/query/diff commands
- `BUILD.md` for app runtime and capabilities
- `PACKAGING.md` for `viper pkg` and ABI
- `TESTING.md` for `@covers`, plans, and proof sidecars
- `RECIPES.md` for common app shapes

## What A Model Should Optimize For

When generating Viper, prefer:

- small, explicit functions
- clear import boundaries
- visible effects
- stable return values
- direct control flow
- low magic
- source that compresses cleanly into semantic state

Do not optimize for:

- clever syntax
- dense operator tricks
- hidden side effects
- giant modules
- reflection-like behavior

## File-Level Structure

Typical order:

1. `use` imports
2. `st` struct declarations
3. helper functions
4. exported/public functions
5. top-level orchestration

## Canonical Syntax

### Imports

Use either relative modules or standard library aliases:

```viper
use "./impact_lib.vp" as user
use "@std/os" as os
use "@std/io" as io
```

Notes:

- `as alias` is preferred for clarity.
- Bare `use "lib.vp"` also works, but aliases are better for LLM readability.

### Variables

```viper
var total = 0
var name: str = "Ada"
var answer: int = lucky()
```

Rules:

- Use `var`.
- Type annotations are optional but preferred when stable.
- Favor annotation on exported/public-facing or semantically important values.

### Functions

```viper
fn multiply(a: int, b: int) -> int {
    ret a * b
}

pub fn render_user(user_id: str) -> str {
    ret "user:" + user_id
}
```

Rules:

- `fn` declares a function.
- `pub fn` exports a function.
- Return type uses `-> Type`.
- Use `ret`, not `return`.
- Parameter type annotations are supported and preferred on public or semantically important functions.
- Prefer typed params, typed returns, and typed structs when the contract is stable.

### Loops

Use either `while` or simple array iteration with `for ... in`.

```viper
var i = 0
while i < 3 {
    pr(i)
    i = i + 1
}

var items = [1, 2, 3]
for item in items {
    pr(item)
}
```

### Structs

```viper
st User {
    name
    age
}
```

Construction patterns:

```viper
var user = User("Ada", 37)
var user2 = User()
user2.name = "Ada"
user2.age = 37
```

Field access:

```viper
pr(user.name)
user.age = user.age + 1
```

### Control Flow

```viper
if total == 21 {
    pr("ok")
} else {
    pr("not ok")
}

while counter < limit {
    counter = counter + 1
}
```

Rules:

- Conditions do not use parentheses.
- Blocks always use braces.
- `else if` is supported, but plain nested `if` is often clearer for LLM generation.

### Arrays

```viper
var items = [1, 2, 3]
var first = items[0]
var size = arr_len(items)
```

Rules:

- Array literals use `[]`.
- Index access uses `[i]`.
- Length is `arr_len(items)`.

### Strings

Viper commonly builds JSON or text via concatenation:

```viper
ret "{\"ok\":true,\"count\":" + count + "}"
```

Prefer:

- direct concatenation
- explicit quotes
- simple helper functions when repeated

## Common Builtins

These are core helpers the model should recognize:

```viper
pr("hello")
panic("boom")
arr_len(items)

var lib = load_dl("libc.so.6")
var puts = lib.get_fn("puts", "s->i")
puts("hi")
```

Guidelines:

- Prefer stdlib wrappers before raw FFI.
- Use `pr(...)` for simple output in scripts and tests.
- Use `panic(...)` only when the failure is intentional and part of the contract.
- Use `arr_len(...)` for array length.
- Keep `load_dl(...)` isolated in adapter modules or stdlib-like wrappers.

## Effects

Effects are a first-class semantic contract.

Example:

```viper
use "@std/os" as os

@effect(os, auth)
pub fn read_user(user_id) -> str {
    ret os.get_env("USER")
}
```

Guidelines:

- Use `@effect(...)` on public or semantically important functions.
- Effects should describe visible behavior, not implementation trivia.
- If a function touches OS, FS, DB, Web, FFI, panic, async, or dynamic behavior, declare it.

Current important effect labels:

- `os`
- `fs`
- `web`
- `db`
- `cache`
- `ai`
- `meta`
- `ffi`
- `panic`
- `async`
- `dynamic`

## FFI

Canonical pattern:

```viper
var libc = load_dl("libc.so.6")
var puts = libc.get_fn("puts", "s->i")
puts("hello")
```

Guidelines:

- Keep FFI isolated in small functions.
- Prefer wrapping FFI inside stdlib-like helpers.
- Do not spread `load_dl` calls across unrelated business logic.

## Standard Library Patterns

Examples:

```viper
use "@std/os" as os
use "@std/io" as io
use "@std/net" as net
```

Common calls:

```viper
os.get_env("USER")
os.exec("echo hi")
var file = io.open(path, "w")
if file != 0 {
    io.write_text(file, "hello")
    io.close(file)
}
net.serve(8080)
net.accept(server)
net.recv_string(client)
net.send(client, response)
```

## Semantic Tooling

The model should treat these as primary project understanding tools.

### Emit state from source

```bash
viper --emit-project-state=state.vstate --focus=build_report --impact path/to/main.vp
```

### Resume from state

```bash
viper --resume-project-state=state.vstate --focus=build_report --impact --brief
```

### Query state

```bash
viper --query-project-state=state.vstate --query-name=build_report --impact --brief
viper --query-project-state=state.vstate --query-effect=os --impact
viper --query-project-state=state.vstate --query-call=read_user
```

### Benchmark semantic handoff

```bash
viper --bench-project-state=state.vstate --focus=build_report --impact
```

### Plan changes

```bash
viper --emit-state-plan=before.vstate after.vstate
viper --run-state-plan=before.vstate after.vstate
```

## Current Safe Writing Style

Until parser/runtime coverage is hardened further, prefer this style:

- use explicit helper functions
- use `<` and `>` directly
- prefer `x < (n + 1)` over `x <= n`
- prefer `x > (n - 1)` over `x >= n`
- prefer `0 - 1000` over bare negative literals in important code paths
- keep nested conditions simple
- use loops and concatenation instead of dense expressions

These are not ideal forever, but they are a good model-writing profile today.

## Anti-Patterns

Avoid generating:

- giant all-in-one modules
- deeply nested conditionals when helper functions would do
- broad FFI use in business code
- unexplained side effects
- silent sentinel-style behavior without semantic annotation
- source shaped for human cleverness instead of machine clarity

## Authoring Checklist For LLMs

Before finalizing Viper code, check:

1. Are imports explicit and minimal?
2. Are important functions typed?
3. Are important effects declared?
4. Are structs simple and field-oriented?
5. Is control flow direct?
6. Can the main function be understood from function names alone?
7. Would `--focus` and `--impact` produce a small useful slice?

## Minimal Example

```viper
use "@std/os" as os

st User {
    name
    role
}

@effect(os)
fn read_user() -> User {
    ret User(os.get_env("USER"), "local")
}

fn build_message(user) -> str {
    ret "{\"name\":\"" + user.name + "\",\"role\":\"" + user.role + "\"}"
}

var user: User = read_user()
pr(build_message(user))
```

## One-Sentence Mental Model

`Write Viper so that a semantic slice can explain the module without reopening the whole source file.`
