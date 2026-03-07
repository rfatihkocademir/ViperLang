# 🐍 ViperLang : The AI-Native Powerhouse

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Status: Phase 6 Complete](https://img.shields.io/badge/Status-Phase_6_Complete-brightgreen.svg)]()

> "Code should be written for machines to execute, but designed for AI to master."

**ViperLang** is not just another language. It is a precision-engineered, high-performance programming environment designed for the age of **Billion-Line Codebases** and **AI-Driven Engineering**. Every byte is optimized, every keyword is compressed, and every instruction is deterministic.

---

## 🚀 Why ViperLang?

### 🤖 AI-Native DNA
ViperLang eliminates the "Token Tax." By using hyper-compressed keywords (`v`, `fn`, `st`, `m`) and the revolutionary `@contract` system, we provide LLMs with a 10x clearer view of your codebase while using 50% fewer tokens.

### ⚡ Blazing Fast ViperVM
A custom register-based virtual machine built in pure C99. With 256 general-purpose registers and a 32-bit instruction word, ViperVM delivers speed that rivals native performance for scripted logic.

### 💎 Deterministic Memory (Zero-GC)
No more GC pauses. ViperLang uses a high-performance Reference Counting (RC) system with deterministic cleanup. Your memory is freed the millisecond it's no longer needed.

### 📐 Mathematical Precision
Native support for non-standard arithmetic. Use `+~` for wrapping additions and `^+` for saturating math — critical for game engines, cryptography, and low-level systems.

---

## ✨ Code at a Glance

### Structs & Field Access
```viper
// Define sleek data structures
st Player { id hp score }

v hero = Player()
hero.id = 1
hero.hp = 100
hero.score = 50.5

pr("Hero Health: ", hero.hp)
```

### High-Power Math & Logic
```viper
v level = 255
v next  = level ^+ 1  // Saturated: Still 255
v jump  = level +~ 1  // Wrapped: 0

i score > 90 {
    pr("Legendary!")
} ei score > 60 {
    pr("Warrior.")
} e {
    pr("Recruit.")
}
```

### Modular Mastery
```viper
use "math.vp"
use "physics.vp"

v force = calculate_force(mass, acceleration)
pr("Resultant Force: ", force)
```

---

## 🛠️ Getting Started in 30 Seconds

1. **Build the Engine:**
   ```bash
   make
   ```
2. **Run Your First Script:**
   ```bash
   ./viper tests/scripts/st_test.vp
   ```

---

## 🗺️ Roadmap: The Path to Absolute Mastery

- [x] **Phase 1-4**: Core VM, Lexer, Parser, and Register Allocation.
- [x] **Phase 5**: Full Function Call Stack & `@contract` Engine.
- [x] **Phase 6**: User-Defined Structs (`st`) & Module System (`use`).
- [ ] **Phase 7**: Native C-FFI Bridge (Hardware & Graphics).
- [ ] **Phase 8**: Standard Library (@math, @io, @sys, @net).
- [ ] **The Horizon**: AOT/JIT Compilation & AI Model Integration.

---

## 🤝 Contributing
ViperLang is an open-source movement to rethink how languages interact with AI. Whether you are a compiler nerd, a VM wizard, or an AI engineer, we want your hands on the code.

**MIT License** | Built with passion by humans and AI.
---
