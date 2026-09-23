# Native Windows build with MSYS2 and xwin

This setup uses native Windows executables only. No WSL, Wine, or Visual Studio installation is required.

## Installed layout

Toolchain root: `E:\hjs-tc`.

- Existing MSYS2 package manager: `C:\Users\funny\scoop\apps\msys2\current\usr\bin\pacman.exe`.
- Native tools: `E:\hjs-tc\packages\clang64\bin` (Clang/LLD 22.1.8, CMake 4.4.3, Ninja 1.13.2).
- xwin 0.10.0: `E:\hjs-tc\xwin\xwin-0.10.0-x86_64-pc-windows-msvc\xwin.exe`.
- xwin SDK: `E:\hjs-tc\sdk`, MSVC headers/libraries 14.44.17.14, Windows SDK 10.0.26100, x64 desktop only.
- Package database/cache: `E:\hjs-tc\pacman-db`, `E:\hjs-tc\pacman-cache`.
- CMake dependency cache: `E:\hjs-tc\fetch`.
- Build directory: `E:\hjs-tc\build\hachishifter-release`.
- Temporary files: `E:\hjs-tc\tmp`.

`clang-cl.exe` is a hard link to the packaged `clang.exe`, selecting CL driver mode. The toolchain explicitly targets `x86_64-pc-windows-msvc`, uses Microsoft SDK/CRT headers and libraries, and links using LLD. The installed MSYS2 package's default GNU/MinGW target must not be used for JUCE.

No global PATH or machine environment change was made. Optional shell setup:

```powershell
. E:\hjs-tc\env.ps1
```

## Build

From the repository directory:

```powershell
.\build-windows-native.ps1
# Configure only:
.\build-windows-native.ps1 -ConfigureOnly
```

Or from the DSH workspace:

```powershell
.\hjs\HachiShifter\build-windows-native.ps1 -Jobs 4
```

Close the built application before linking a new version. The build script uses `juce/cmake/windows-msys2-xwin-native.cmake`; it keeps generated files and dependency downloads on E:. It enables ONNX Runtime/DirectML. Models are not downloaded or bundled.

Output:

```text
E:\hjs-tc\build\hachishifter-release\HachiShifterNext_artefacts\Release\HachiShifter Next.exe
```

Keep `onnxruntime.dll`, `onnxruntime_providers_shared.dll`, and `DirectML.dll` alongside the EXE.

## Compatibility fixes

JUCE 8.0.8's helper-tool bootstrap does not normally pass the parent toolchain to its child configure. CMake applies a local dependency patch so the helper is built with the same Clang-CL/xwin setup. Clang-CL 22 also exposes an invalid array-layout forwarding constructor; the dependency patch preserves array bounds and restores a matching AudioProcessor forwarding overload. Neither patch changes the application ABI.

The separate Linux cross-compilation file `windows-msvc-xwin.cmake` remains available for GitLab. This native build uses its own toolchain file.

## Disk footprint and caches

Measured logical file sizes after removing the xwin extraction/download cache and pacman package downloads:

| Component | MiB |
| --- | ---: |
| MSYS2 native tools and dependencies | 1083.5 |
| xwin SDK/CRT | 617.7 |
| JUCE/ONNX/DirectML dependency cache including helper | 706.1 |
| Windows build including EXE and DLLs | 100.9 |
| xwin executable | 7.1 |
| package database and xwin archive | 7.4 |

Total approximately 2.46 GiB (logical file size; allocated disk size and hard links can differ). Existing MSYS2 and source checkout are not included. Cleaned caches released approximately 1.19 GiB. Keep `sdk`, `packages`, and `pacman-db` for future builds. CMake's `fetch` and the build folder support incremental builds and should normally be retained.

## Validation

- xwin archive SHA256 verified against release checksum:
  `96E83665EF5C1406AABD0BE603A8D08E67E604F36FDA630B6807EE948A614E26`.
- Windows C and C++ compiler/link tests passed.
- Full native Windows HachiShifter build passed with ONNX/DirectML enabled.
- Reconfiguration passed.
- Native GUI launched on this device; startup log reports `UI renderer: Direct2D; requested=gpu`, visible window, responsive message loop, and menu events.
- GPU startup evidence is not a full audio or editing regression test.

The UI settings page includes a software-rendering fallback. Emergency fallback before launch:

```powershell
$env:HACHI_SOFTWARE_RENDERING = '1'
& 'E:\hjs-tc\build\hachishifter-release\HachiShifterNext_artefacts\Release\HachiShifter Next.exe'
```

Remove that environment variable to allow the saved setting/default GPU mode again.
