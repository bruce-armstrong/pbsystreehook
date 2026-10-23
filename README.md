# pbsystreehook

Adds your own items to the **System Tree right-click menu** in the PowerBuilder
2025 R2 (PB250) IDE. When you right-click an object in the System Tree, pbsystreehook
appends configurable menu items; choosing one runs a command of your choice with
the clicked object's **name** and **library** filled in.

The menu items, their actions, and logging are all driven by a plain
`pbsystreehook.ini` file — no rebuild needed to change them.

> **Note:** The PowerBuilder IDE has no official add-in API, so pbsystreehook works by
> injecting a small DLL into the IDE process using documented Windows hooks. This
> is unsupported by Appeon. See [Caveats](#caveats).

---

## Files

| File         | Purpose                                                        |
|--------------|---------------------------------------------------------------|
| `pbsystreehook.dll` | The hook. Injected into the IDE; draws the menu items.         |
| `pbsystreehookload.exe` | Loader. Installs the hook into the running IDE.                |
| `pbsystreehook.ini` | Configuration: your menu items, actions, and logging.         |
| `pbsystreehook.c`   | Source for the DLL.                                            |
| `pbsystreehookload.c`   | Source for the loader.                                         |
| `build.cmd`  | Rebuilds `pbsystreehook.dll` and `pbsystreehookload.exe`.                        |
| `LICENSE`    | MIT License.                                                   |

## Requirements

- **Windows** with the **PowerBuilder 2025 R2 IDE** (`PB250.exe`).
- The hook and loader are **32-bit (x86)** because the IDE is a 32-bit process.
  The prebuilt binaries are ready to use; you only need a compiler to rebuild.

## Usage

1. Keep `pbsystreehook.dll` and `pbsystreehook.ini` together in the same folder.
2. Start the PowerBuilder IDE and open a workspace.
3. Run `pbsystreehookload.exe`. A message box says **"Hook installed."**
   **Leave it open** — the hook is active only while the loader is running.
4. Right-click an object in the System Tree. Your items appear at the bottom of
   the menu.
5. To remove the hook, click **OK** on the loader's message box *before* closing
   the IDE.

## Configuration (`pbsystreehook.ini`)

`pbsystreehook.ini` must sit next to `pbsystreehook.dll`. It is re-read on **every**
right-click, so edits take effect immediately — no need to restart the loader or
the IDE.

```ini
[pbsystreehook]
Items=3
Log=0
; LogFile=C:\Users\me\Downloads\pbsystreehook\pbsystreehook.log

[Item1]
Label=Run my tool on "%o"
Command=C:\Tools\mytool.exe
Args="%o" "%L"
; Dir=C:\Tools

[Item2]
Label=Copy object name
Command=:clipboard
Args=%o

[Item3]
Label=Show object + library
Command=:msgbox
Args=Object:  %o%nLibrary: %L
```

### `[pbsystreehook]` section

| Key       | Meaning                                                              |
|-----------|---------------------------------------------------------------------|
| `Items`   | Number of menu items to read (`[Item1]` … `[ItemN]`).               |
| `Log`     | `1` turns on the debug log, `0` off.                                 |
| `LogFile` | Optional log path. If omitted, `pbsystreehook.log` is written by the DLL.   |

If `Items=0` or the file is missing, the menu shows a disabled
*"(edit pbsystreehook.ini to add menu items)"* hint.

### `[ItemN]` sections

| Key       | Meaning                                                                      |
|-----------|------------------------------------------------------------------------------|
| `Label`   | The text shown in the menu.                                                   |
| `Command` | A program to run, **or** a built-in action (see below).                       |
| `Args`    | Command-line arguments passed to the program.                                 |
| `Dir`     | Optional working directory for the program.                                   |

**Built-in actions** (use as `Command`):

| Command      | Effect                                  |
|--------------|-----------------------------------------|
| `:clipboard` | Copies `Args` to the Windows clipboard. |
| `:msgbox`    | Shows `Args` in a message box.          |

Anything else in `Command` is launched with `ShellExecute` (`.exe`, `.bat`,
`.cmd`, a document, etc.).

### Placeholders

Usable in `Label` and `Args`:

| Token | Expands to                                                      |
|-------|-----------------------------------------------------------------|
| `%o`  | Object name, e.g. `pfc_n_cst_filesrv`                            |
| `%l`  | Library as shown, e.g. `pfcapsrv.pbl (C:\...\pfc libs)`          |
| `%L`  | Full `.pbl` path, e.g. `C:\...\pfc libs\pfcapsrv.pbl`            |
| `%n`  | Newline (handy in `:msgbox`)                                     |
| `%%`  | A literal percent sign                                          |

> Save `pbsystreehook.ini` as **ANSI** or **UTF-16** (Notepad's default is fine).

## Logging

Set `Log=1` in `[pbsystreehook]` to trace what the hook does — config loads, each
right-click (object + library), and every action fired (including
`ShellExecute` failures). The log defaults to `pbsystreehook.log` next to the DLL, or
set `LogFile=` to another path. Set `Log=0` to turn it off. No rebuild needed.

## Building

The binaries ship ready to use. To rebuild, install a 32-bit MinGW-w64 toolchain
and run `build.cmd`.

1. Install [MSYS2](https://www.msys2.org/), then from the **MSYS2 MINGW32**
   shell:
   ```
   pacman -S --needed mingw-w64-i686-gcc
   ```
2. Put `C:\msys64\mingw32\bin` on your `PATH` (or build from that shell).
3. From this folder:
   ```
   build.cmd
   ```

`pbsystreehook.dll` must not be loaded while building — close the loader first, or the
build will fail with "Permission denied" on the DLL.

## Extending

Replace or add `[ItemN]` entries in `pbsystreehook.ini`. To add new **built-in**
actions (beyond `:clipboard` / `:msgbox`) or change how the IDE is read, edit
`RunItem()` / the capture logic in `pbsystreehook.c` and rebuild.

## Caveats

- **Unsupported.** PowerBuilder has no public IDE add-in API. pbsystreehook uses Windows
  message hooks and injection; it may need re-checking after any PowerBuilder
  update.
- **Antivirus.** DLL injection is exactly what antivirus software watches for.
  You will likely need to allow the files or add an exclusion, and to
  **code-sign** the DLL if you distribute it to other machines.
- **Bitness.** The IDE is 32-bit, so the DLL and loader must be 32-bit.
- **Menu command IDs** use the `0x9100` range to avoid colliding with the IDE's
  own commands.

## License

Copyright (c) 2026 Bruce Armstrong

Released under the MIT License. See [LICENSE](LICENSE) for the full text.
