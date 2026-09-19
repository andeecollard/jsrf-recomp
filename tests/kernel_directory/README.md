# Directory query ABI regression

This asset-free Windows x64 test dispatches the real kernel thunk for ordinal 207 through a synthetic guest stack. It checks ten-argument stack cleanup, ANSI search-mask translation, continuation, restart, exclusion of dot directories, and rejection of unsupported information classes.

Build and run from the repository root:

```powershell
cmake -S tests/kernel_directory -B build/kernel-directory -A x64
cmake --build build/kernel-directory --config Release
ctest --test-dir build/kernel-directory -C Release --output-on-failure
```

The declaration is independently specified by nxdk's lib/xboxkrnl/xboxkrnl.h (NtQueryDirectoryFile) and Cxbx-Reloaded's src/core/kernel/exports/EmuKrnlNt.cpp. This test does not launch a title or require game assets.
