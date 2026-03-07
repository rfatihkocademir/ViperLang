# Contributing to ViperLang

We are thrilled you want to contribute to the ViperLang revolution! 

## Getting Started
ViperLang is written in C99. You will need `gcc`, `make`, and `libffi-dev` installed.

### Build Instructions
```bash
git clone https://github.com/viperlang/viper
cd viper
make
```

### Running Tests
Before opening a Pull Request, ensure all tests pass:
```bash
make test
```

## Community Packages
We encourage you to write community packages! If you build a package, host it on a public repository (like GitHub) so others can install it via:
```bash
viper pkg add github.com/your-username/viper-pkg
```

### Pull Requests
- Ensure your code follows the standard C99 formatting used across `src/` files.
- If you're adding a feature, please include a test script in `tests/scripts/`.

*"AI-Native doesn't mean AI replaces you; it means the language speaks your mind."*
