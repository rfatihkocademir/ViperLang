# 🐍 ViperLang: The AI-Native Powerhouse

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Status: All Phases Complete](https://img.shields.io/badge/Status-All_Phases_Complete-brightgreen.svg)]()

> "Code should be written for machines to execute, but designed for AI to master."

**ViperLang** is not just another language. It is a precision-engineered, high-performance programming environment designed for the age of **Billion-Line Codebases** and **AI-Driven Engineering**. Every byte is optimized, every keyword is compressed, and every instruction is deterministic.

---

## 🚀 Getting Started

You don't need to be a C-compiler expert to use ViperLang! Just follow these exact steps to install the language on your computer and start writing your first program.

### 1️⃣ Installation

Open your terminal and paste these commands. This will download and install the `viper` engine to your system:

```bash
git clone https://github.com/rfatihkocademir/ViperLang.git
cd ViperLang
./install.sh
```

That's it! You can now type `viper` anywhere in your terminal.

---

## 📦 Creating Your First Project

ViperLang comes with **VPM (Viper Package Manager)** built right in. Let's create your first app:

```bash
# 1. Create a folder for your project
mkdir my_first_app
cd my_first_app

# 2. Initialize it as a Viper project
viper pkg init
```

### 3️⃣ Writing Code

Create a file named `main.vp` and drop this code inside:

```viper
// main.vp
use "@std/math" as math

pr("Welcome to ViperLang! 🐍")
pr("Pi is: ", math.pi())
pr("sqrt(144) = ", math.sqrt(144.0))
```

### 4️⃣ Running Your Code

Run it instantly with:
```bash
viper main.vp
```

---

## 🌐 Community Packages (VPM)

Want to do something complex, like build a Web Server or 3D Game? You don't have to reinvent the wheel. You can download code written by other developers straight from GitHub!

Simply run:
```bash
viper pkg add github.com/username/cool-viper-package
```

VPM will automatically download the package, and you can instantly `use` it in your `.vp` files.

---

## 📚 Standard Library (`@std`)

ViperLang comes fully equipped with the tools you need right out of the box.

* **`@std/io`**: Read and write files easily.
* **`@std/os`**: Interact with your operating system, read environment variables, or execute bash commands.
* **`@std/net`**: Build HTTP servers and TCP socket applications.
* **`@std/math`**: Advanced math functions — `sin`, `cos`, `sqrt`, `pow`, `ceil`, `floor`, `abs`, and more.

---

## 🎮 Hardware & Graphics (FFI Examples)

ViperLang's **True Dynamic FFI** can call **any** C library directly — no glue code needed. Check the `examples/` folder for ready-to-run scripts:

* **`examples/raylib_demo.vp`** — Open a game window and draw shapes with Raylib.
* **`examples/sdl2_demo.vp`** — Create an SDL2 window and manage events.

---

## 🛠️ VS Code Support

To make coding even better, install our official VS Code extension for syntax highlighting.
Inside the `ViperLang` folder you downloaded, you will find a `vscode-viperlang` folder. Install its contents in your editor to get beautiful syntax colors!

---

## 🗺️ Roadmap

- [x] **Phase 1-4**: Core VM, Lexer, Parser, and Register Allocation.
- [x] **Phase 5**: Full Function Call Stack & `@contract` Engine.
- [x] **Phase 6**: User-Defined Structs (`st`) & Module System (`use`).
- [x] **Phase 7**: Native C-FFI Bridge (Hardware & Graphics).
- [x] **Phase 8**: Standard Library (`@std/math`, `@std/io`, `@std/os`, `@std/net`).
- [ ] **The Horizon**: AOT/JIT Compilation & AI Model Integration.

---

## 🤝 Contributing

Love C99 and Virtual Machines? Want to help make ViperLang even faster?
Check out our `CONTRIBUTING.md` guide and drop a Pull Request! We are completely open source (MIT License).

