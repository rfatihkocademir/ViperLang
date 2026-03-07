#ifndef VIPER_PKG_H
#define VIPER_PKG_H

// Handles `viper pkg ...` commands.
// Returns an exit code compatible with `main`.
int vpm_handle_cli(int argc, const char* argv[]);

#endif // VIPER_PKG_H
