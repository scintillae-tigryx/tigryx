# Windows MinGW-w64 x86 Build Configuration

This platform configuration targets Windows 2000/XP+ using MinGW-w64 (GCC) for maximum compatibility with older Windows versions while supporting C99/C11 features.

## Requirements

- MSYS2 with MinGW-w64 (https://www.msys2.org/)
- GCC compiler (mingw-w64-i686-gcc)
- CMake 3.16 or later
- Third-party headers/libraries listed in the top-level README.md

## Installation

1. Download and install MSYS2 from https://www.msys2.org/
2. Open the **MINGW32** shell (not UCRT64 or MSYS)
3. Install GCC: `pacman -S mingw-w64-i686-gcc`
4. Verify: `gcc --version`

## Building

### Basic Build (Windows XP compatibility)
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1
```

### Specify MinGW Path
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1 -MinGWPath "C:\msys64\mingw32"
```

### Target Windows 2000
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1 -WindowsVersion 5.0
```

### Target Windows Vista+
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1 -WindowsVersion 6.0
```

### Debug Build
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1 -Config Debug
```

### Clean Build
```powershell
.\platforms\windows-opengl-mingw-x86\build.ps1 -Clean
```

## Compatibility Features

- **C99/C11 support** - Full support for modern C features
- **Static linking** - Reduces runtime dependencies
- **Windows 2000/XP compatible** - Targets older Windows versions
- **x86 JIT** - Enabled for 32-bit code generation

## Output

Binaries are placed in `bin/windows-opengl-mingw-x86/`:
- `tigryx.exe` - Dispatcher
- `tigryxw.exe` - GUI-subsystem twin of tigryx
- `tigryx-ri.exe` - Interpreter / bytecode compiler
- `tigryx-pp.exe` - Preprocessor

## Notes

- This configuration produces 32-bit binaries that run on Windows 2000 and later
- The x86 JIT is enabled by default for this platform
