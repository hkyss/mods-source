# hkyss mods source

Each folder holds the code of the version on the mod page.

- [hkyss - ENB INI Cache](skyrim-se/hkyss%20-%20ENB%20INI%20Cache/src), on [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/197597)
- [hkyss - Head Sway](skyrim-se/hkyss%20-%20Head%20Sway/src), on [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/195339)
- [hkyss - Nolvus Gamepad Controls](skyrim-se/hkyss%20-%20Nolvus%20Gamepad%20Controls/src), on [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/196000)
- [hkyss - OAR Scan Cache](skyrim-se/hkyss%20-%20OAR%20Scan%20Cache/src), on [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/197595)

## Building the Skyrim plugins

The plugins are built on [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR) by Ryan McKenzie, CharmedBaryon and alandtse. CMake fetches the version named in `skyrim-se/CMakeLists.txt`, 4.39.3, the last one under the MIT license.

You need Visual Studio 2022 Build Tools with the C++ workload, which brings CMake and Ninja, and [vcpkg](https://github.com/microsoft/vcpkg) for the libraries in `vcpkg.json`. From a Visual Studio x64 prompt in `skyrim-se`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
```

The DLLs land in `build\bin`.

## License

Copyright (C) 2026 hkyss

Everything here is free software under the GNU General Public License, version 3 or any later version, in the `LICENSE` file. The libraries the plugins are built on keep their own licenses, which sit beside each mod that ships them.
