# Straw_SamExtensions DLL Source

Native source package for `Straw_SamExtensions.dll`, the F4SE plugin used by the StrawSamExtensions for Fallout 4.

This repository is intended to make the native DLL source available for transparency, review, and local rebuilding. It does not contain the full mod release package. The full mod still needs the ESL, Papyrus scripts, SAM extension JSON files and INI from the normal release archive.

## Current source state

- Project line: Straw_SamExtensions v1.7.66
- Native plugin version: 86
- CMake project version: 0.1.86
- Main native source: `src/Plugin.cpp`
- Compatibility pre-include: `src/CompatPch.h`
- Build script: `build_plugin.bat`

## What is included

```text
CMakeLists.txt
build_plugin.bat
src/Plugin.cpp
src/CompatPch.h
external/README_put_f4se_sdk_here.txt
README.md
BUILDING.md
CHANGELOG.md
CREDITS.md
THIRD_PARTY_NOTICES.md
LICENSE
.gitignore
.gitattributes
```

## What is not included

```text
F4SE SDK / Fallout 4 headers
compiled DLL/PDB/output files
Papyrus compiled PEX files
Screen Archer Menu / SAF files
full Nexus release package files
```

The F4SE SDK and any other external dependency files should be obtained from their original sources and placed locally for building. They should not be committed into this repository.

## Build summary

Requirements:

- Windows
- Visual Studio with C++ desktop workload
- CMake
- Fallout 4 Script Extender source/SDK tree

Either set an environment variable:

```bat
set F4SE_SDK=D:\Steam\steamapps\common\Fallout 4\src
```

or place the SDK locally at:

```text
external\f4se_sdk\
```

Then run:

```bat
build_plugin.bat
```

See `BUILDING.md` for detailed notes.

## Relationship to the mod release

This repository only covers the native DLL source. A functional installed mod also requires the matching non-native files from the public mod release package, especially:

```text
Data\F4SE\Plugins\Straw_SamExtensions.ini
Data\F4SE\Plugins\SAM\Extensions\*.json
Data\Scripts\*.pex
Data\Scripts\Source\User\*.psc
Straw_SamExtensions.esl
```

## License

This native DLL source repository is open-source under the MIT License. See `LICENSE`.

The MIT License permits use, copying, modification, publishing, distribution, sublicensing, and selling copies of this source, provided the copyright and permission notice are included.

Only this repository's own source is covered by the MIT License. Third-party projects and dependencies such as F4SE, Fallout 4, Screen Archer Menu / SAF, LooksmenuPlayerRotation, CommonLibF4, and ConsoleUtilF4 remain under their own licenses and are not bundled here.

## Credits

See `CREDITS.md` and `THIRD_PARTY_NOTICES.md`.
