# Building Straw_SamExtensions.dll

## 1. Install tools

Install:

- Visual Studio with the C++ desktop workload
- CMake
- Fallout 4 Script Extender source/SDK files

The supplied `build_plugin.bat` currently uses the Visual Studio 2026 generator used during project development:

```bat
set "GENERATOR=Visual Studio 18 2026"
```

If your local Visual Studio version differs, edit the generator line to the name listed by:

```bat
cmake --help
```

For example, a Visual Studio 2022 setup commonly uses:

```bat
set "GENERATOR=Visual Studio 17 2022"
```

## 2. Point the project at the F4SE SDK

Preferred project-local layout:

```text
external\f4se_sdk\f4se\PluginAPI.h
external\f4se_sdk\f4se_common\Relocation.h
```

Alternative environment variable:

```bat
set F4SE_SDK=D:\Steam\steamapps\common\Fallout 4\src
```

The CMake script searches for the expected F4SE and common headers/sources below `F4SE_SDK`.

## 3. Build

From the repository root:

```bat
build_plugin.bat
```

The script uses a short temporary build folder:

```text
%TEMP%\Straw_SamExtensions_build_vs2026_x64
```

This is intentional and helps avoid long-path MSBuild/FileTracker problems.

## 4. Output

The script writes the built DLL to:

```text
dist\Data\F4SE\Plugins\Straw_SamExtensions.dll
```

If `FALLOUT4_DIR` is set, or if the default local Fallout 4 path exists, the script may also copy the DLL directly into the game install.

## 5. Keep generated files out of Git

Do not commit:

```text
dist\
build\
CMakeFiles\
*.dll
*.pdb
*.obj
*.lib
*.exp
external\f4se_sdk\
```

These are ignored by `.gitignore`.
