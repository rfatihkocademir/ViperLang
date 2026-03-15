#!/usr/bin/env bash
set -e

echo "=== ViperLang Installer ==="
echo "Features: semantic state handoff, focused resume/query, typed ABI, effect contracts"
echo ""

# Check for required dependencies
command -v gcc >/dev/null 2>&1 || { echo "Error: gcc is required to build ViperLang. Install it first."; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Error: make is required. Install it first."; exit 1; }

# Check for libffi
if ! ldconfig -p 2>/dev/null | grep -q libffi || ! find /usr/lib* /usr/local/lib* -name 'ffi.h' 2>/dev/null | head -1 | grep -q .; then
    echo "Warning: libffi-dev may not be installed."
    echo "  Debian/Ubuntu: sudo apt install libffi-dev"
    echo "  Fedora:        sudo dnf install libffi-devel"
    echo ""
fi

# Check for sqlite3 headers/libs used by the runtime.
if ! ldconfig -p 2>/dev/null | grep -q sqlite3 || ! find /usr/include* /usr/local/include* -name 'sqlite3.h' 2>/dev/null | head -1 | grep -q .; then
    echo "Warning: sqlite3 development files may not be installed."
    echo "  Debian/Ubuntu: sudo apt install libsqlite3-dev"
    echo "  Fedora:        sudo dnf install sqlite-devel"
    echo ""
fi

echo "Building ViperLang from source..."
make clean
make

BIN_DIR="/usr/local/bin"
VIPER_HOME="/usr/local/lib/viper"
STD_DIR="${VIPER_HOME}/std"
LLM_DIR="${VIPER_HOME}/docs/llm"

echo ""
echo "Installing binary to ${BIN_DIR}/viper..."
sudo mkdir -p "${BIN_DIR}"
sudo cp viper "${BIN_DIR}/viper"
sudo chmod +x "${BIN_DIR}/viper"

echo "Installing standard library to ${STD_DIR}..."
sudo mkdir -p "${STD_DIR}"
sudo cp lib/std/*.vp "${STD_DIR}/"
sudo cp lib/std/*.so "${STD_DIR}/"

echo "Installing LLM onboarding pack to ${LLM_DIR}..."
sudo mkdir -p "${LLM_DIR}"
sudo cp docs/llm/*.md "${LLM_DIR}/"

echo ""
echo "✅ ViperLang installed successfully!"
echo ""
echo "Installed components:"
echo " - Binary: ${BIN_DIR}/viper"
echo " - Standard library: ${STD_DIR}"
echo " - LLM onboarding pack: ${LLM_DIR}"
echo ""
echo "Quick start:"
echo "  viper my_script.vp"
echo "  viper pkg init my_project"
echo ""
echo "LLM-native workflow:"
echo "  viper --emit-project-state --focus=<symbol> --impact <entry.vp>"
echo "  viper --resume-project-state=<state.vstate> --brief"
