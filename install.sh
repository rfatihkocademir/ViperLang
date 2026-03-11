#!/usr/bin/env bash
set -e

echo "=== ViperLang v3 Installer ==="
echo "Features: M:N Scheduler, Tiered JIT, Real Concurrency, Zero-Copy I/O"
echo ""

# Check for required dependencies
command -v gcc >/dev/null 2>&1 || { echo "Error: gcc is required for the JIT compiler. Install it first."; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Error: make is required. Install it first."; exit 1; }

# Check for libffi
if ! ldconfig -p 2>/dev/null | grep -q libffi || ! find /usr/lib* /usr/local/lib* -name 'ffi.h' 2>/dev/null | head -1 | grep -q .; then
    echo "Warning: libffi-dev may not be installed."
    echo "  Debian/Ubuntu: sudo apt install libffi-dev"
    echo "  Fedora:        sudo dnf install libffi-devel"
    echo ""
fi

echo "Building ViperLang v3 from source..."
make clean
make

# Define target paths
BIN_DIR="/usr/local/bin"
LIB_DIR="/usr/local/lib/viper/std"

echo ""
echo "Installing binary to $BIN_DIR/viper..."
sudo mkdir -p $BIN_DIR
sudo cp viper $BIN_DIR/viper
sudo chmod +x $BIN_DIR/viper

echo "Installing standard library to $LIB_DIR..."
sudo mkdir -p $LIB_DIR

# Copy standard library modules (.vp files)
sudo cp lib/std/*.vp $LIB_DIR/

# Copy compiled shared libraries (.so files)
sudo cp lib/std/*.so $LIB_DIR/

# Copy LLM Reference Guide (used by viper pkg init)
echo "Installing LLM Reference Guide..."
sudo mkdir -p /usr/local/lib/viper
if [ -f docs/LLM_REFERENCE.md ]; then
    sudo cp docs/LLM_REFERENCE.md /usr/local/lib/viper/LLM_REFERENCE.md
fi

echo ""
echo "✅ ViperLang v3 installed successfully!"
echo ""
echo "New in v3:"
echo " - Real Concurrency (Fibers + M:N Scheduler)"
echo " - Tiered JIT Compilation"
echo " - Advanced Memory Profiler (run with pr_profile())"
echo " - Zero-Copy Asynchronous I/O"
echo ""
echo "You can now run 'viper' from anywhere:"
echo "  viper my_script.vp"
echo ""
echo "Standard library available at: $LIB_DIR"
echo "Binary installed at: $BIN_DIR/viper"
