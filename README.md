# 🐍 ViperLang: The AI-Native Powerhouse

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Status: Phase 6 Complete](https://img.shields.io/badge/Status-Phase_6_Complete-brightgreen.svg)]()

> "Code should be written for machines to execute, but designed for AI to master."

**ViperLang** is not just another language. It is a precision-engineered, high-performance programming environment designed for the age of **Billion-Line Codebases** and **AI-Driven Engineering**. Every byte is optimized, every keyword is compressed, and every instruction is deterministic.

---

## 🚀 Getting Started

You don't need to be a C-compiler expert to use ViperLang! Just follow these exact steps to install the language on your computer and start writing your first program.

### 1️⃣ Installation

Open your terminal and paste these commands. This will download and install the `viper` engine to your system:

```bash
git clone https://github.com/YOUR_USERNAME/ViperLang.git
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
use "@std/net" as net
use "@std/io" as io

pr("Welcome to ViperLang! 🐍")
pr("Let me do some math for you:")

v result = 50 * 2
pr("50 times 2 is: ", result)
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
* **`@std/net`**: Need to build an API? We have a built-in asynchronous HTTP TCP server library ready to go!

---

## 🛠️ VS Code Support

To make coding even better, install our official VS Code extension for syntax highlighting. 
Inside the `ViperLang` folder you downloaded, you will find a `vscode-viperlang` folder. Install its contents in your editor to get beautiful syntax colors!

---

## 🤝 Contributing

Love C99 and Virtual Machines? Want to help make ViperLang even faster?
Check out our `CONTRIBUTING.md` guide and drop a Pull Request! We are completely open source (MIT License).
