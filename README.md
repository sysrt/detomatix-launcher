<h1 align="center">Detomatix</h1>

<p align="center">
  A small, fast BYOND Alt Launcher with support of modding<br>
</p>

<p align="center">
  <img width="1287" height="852" alt="image" src="https://github.com/user-attachments/assets/a8d7c064-f8e7-40fd-b41c-f0a405d7affd" />
</p>

## Requirements

* **WebView2 Runtime** — If the UI is blank, install the [Evergreen WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).
* **BYOND** installed (the launcher looks for `C:\Program Files (x86)\BYOND` by default).

## Building from source

Requirements:

* [CMake](https://cmake.org/) 3.16+
* **Git** (CMake fetches the [webview](https://github.com/webview/webview) library at configure time)
* A C++17 toolchain: **MinGW-w64** or MSVC

```bat

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

With Visual Studio instead:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Mods

A mod is a DLL that gets injected into the BYOND client. The launcher does not care what the DLL
does — it just loads the ones you enabled.

<img width="374" height="323" alt="image" src="https://github.com/user-attachments/assets/b37c2636-b4c4-4071-82d5-59bd3c425bd7" />


### Layout

Either a **folder** with a `.dll` and a metadata file:

```
mods/
  MyMod/
    MyMod.dll
    mod.json
```

or **loose files** directly in `mods/`: `MyMod.dll` plus an optional `MyMod.json`.

`mods/` is looked up next to the executable, or one level up (`../mods`).

### `mod.json`

```json
{
  "name": "My Mod",
  "version": "1.0.0",
  "author": "you",
  "description": "What it does.",
  "dll": "MyMod.dll"
}
```

* `dll` is optional — without it the first `.dll` in the folder is used.
* `name` falls back to the folder / file name.
* The folder name is the mod **id** used in `config.json`.

### Telling mods about each other

Before launching the client, the launcher publishes the semicolon-separated list of enabled mod
DLLs in the **`BYOND_MOD_PATHS`** environment variable (inherited by `dreamseeker.exe`) and mirrors
it to `HKCU\SOFTWARE\Detomatix`. Mods use it to discover their siblings.

Native mods that expose the `Mod_*` contract (`Mod_Name`, `Mod_Commands`, `Mod_Command`,
`Mod_Status`) get merged into whichever mod owns the console.

### Building mods

soon

<img width="1278" height="990" alt="image" src="https://github.com/user-attachments/assets/96435e1c-dc93-4bed-bbe0-66f5c783bae4" />



## Else

Use the launcher and mods at your own risk.
