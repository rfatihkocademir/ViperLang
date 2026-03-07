#!/usr/bin/env bash
set -e

echo "=== ViperLang Installer ==="
echo "Building ViperLang from source..."

# Build the project
make clean
make

# Define target paths
BIN_DIR="/usr/local/bin"
LIB_DIR="/usr/local/lib/viper"

echo "Installing binary to $BIN_DIR..."
sudo cp viper $BIN_DIR/viper
sudo chmod +x $BIN_DIR/viper

echo "Installing standard library to $LIB_DIR..."
sudo mkdir -p $LIB_DIR
# Copy the built standard libraries (.so and .vp files)
sudo cp -r lib/std $LIB_DIR/

echo ""
echo "ViperLang installed successfully! 🚀"
echo "Run 'viper' in your terminal to start."
