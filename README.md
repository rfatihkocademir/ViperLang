# ViperLang v3

> A programming language designed from the ground up for **AI-Native development** — extreme token compression, zero-ambiguity syntax, and a blazing-fast register-based VM (ViperVM).

## What is ViperLang?

ViperLang is a statically-typed, compiled language that runs on its own bytecode virtual machine (**ViperVM**). It was designed with one goal: **eliminate every token of ambiguity so that AI agents can write and read code with zero hallucinations**.

Every keyword is as short as it can possibly be. Every operator encodes its meaning unambiguously. No symbol overloading. No implicit behavior. No hidden state.

## Quick Start

### Requirements
- GCC (or any C99-compatible compiler)
- `make`

### Build
```bash
git clone https://github.com/you/viperlang
cd viperlang
make
```

### Run a Script
```bash
./viper my_script.vp
```

## Language Syntax (v3)

### Variables
```viper
v name = "ViperLang"       // string
v price:i = 1500           // typed integer
v ratio = 3.14             // float
```

### Print
```viper
pr("Hello, World!")
pr(price)
```

### Arithmetic & Operators
```viper
v total = price + 200
v wrapped = num1 +~ 255     // Wrapping add (never overflows)
v saturated = num1 ^+ 255   // Saturating add (caps at max)
```

### If / Else If / Else
```viper
i score > 90 {
    pr("Excellent!")
} ei score > 60 {
    pr("Passed.")
} e {
    pr("Failed.")
}
```

### While Loop
```viper
v counter = 0
m counter < 10 {
    pr(counter)
    counter = counter + 1
}
```

### Functions
```viper
fn add(x, y) {
    ret x + y
}

v res = add(10, 20)
pr(res) // 30
```

### Structs (st)
```viper
st Vec2 { x y }
v pos = Vec2()
pos.x = 10
pos.y = 20
```

### Modules (use)
```viper
use "math.vp"
v res = multiply(6, 7)
```

### Block Scopes
```viper
{
    v temp = 42
    pr(temp)  // 42
}
// temp not accessible here
```

## ViperVM Architecture

ViperLang compiles to **register-based bytecode** that runs on the **ViperVM** — a custom virtual machine inspired by LuaJIT and Android Dalvik.

- **32-bit instruction word:** `[8-bit OpCode | 8-bit Dest | 8-bit Arg1 | 8-bit Arg2]`
- **256 general-purpose registers**
- **Zero Garbage Collector pauses** — Reference Counting memory management
- **Native String objects** with FNV-1a hashing for intern optimization

## Project Structure
```
viperlang/
├── include/
│   ├── lexer.h      — Token definitions
│   ├── ast.h        — AST node types
│   ├── parser.h     — Parser interface
│   ├── compiler.h   — Bytecode compiler
│   ├── vm.h         — ViperVM opcodes & runtime
│   └── native.h     — Standard library & memory
├── src/
│   ├── lexer.c      — Tokenizer
│   ├── ast.c        — AST node constructors
│   ├── parser.c     — Recursive descent parser
│   ├── compiler.c   — AST → Bytecode compiler
│   ├── vm.c         — ViperVM execution engine
│   ├── native.c     — String, Object, RC allocator
│   └── main.c       — CLI entry point
├── Makefile
└── README.md
```

## Example Scripts
| File | What it tests |
|------|--------------|
| `calc.vp` | Variables and arithmetic |
| `string_test.vp` | String allocation and printing |
| `flow_test.vp` | If/Else branching and scopes |
| `loop_test.vp` | While loops and counter increments |

## Roadmap

- [x] Lexer (token-compressed v3 syntax)
- [x] Recursive descent Parser
- [x] AST generation
- [x] Register-based Bytecode Compiler
- [x] ViperVM Runtime
- [x] String Object memory management (RC)
- [x] If / Else / Else-If control flow
- [x] While loops (`m`)
- [x] Block scopes
- [x] Built-in `pr()` print
- [x] Full `fn` function call stack (Phase 5)
- [x] `st` Struct support and field access (Phase 6)
- [x] `use` module system (Phase 6)
- [ ] Native C-FFI for hardware and game engines (Phase 7)
- [ ] Standard library: `@math`, `@io`, `@net`
- [ ] High-performance JIT compiler backend

## License
MIT — Use freely, build wildly.

---
*ViperLang: Built by human and AI together.*
