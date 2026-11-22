# Building KaoForge

KaoForge ships as source only: you compile the loader and the mods yourself. It takes about a minute.
The game is a **32-bit** program, so everything must be built with a **32-bit (i686) MinGW-w64 compiler**, even on a 64-bit system.

## 1. Install the tools

| System | Command | Status |
|---|---|---|
| **Ubuntu / Debian** | `sudo apt install gcc-mingw-w64-i686 python3` | tested (Ubuntu, GCC 13) |
| Arch / Manjaro | `sudo pacman -S mingw-w64-gcc python` | not tested |
| Fedora | `sudo dnf install mingw32-gcc python3` | not tested |
| Windows | install [MSYS2](https://www.msys2.org/), open the **MINGW32** shell, then `pacman -S mingw-w64-i686-gcc python git` | not tested |

Check that the compiler is there and targets 32-bit Windows:
```bash
i686-w64-mingw32-gcc --version
```
On Ubuntu/Debian the compiler comes in two flavours (`posix` and `win32`); either works, you do not need to choose.
KaoForge only needs the compiler and Python's standard library — no other packages.

## 2. Get the source
```bash
git clone https://github.com/GXAD/kaoforge
cd kaoforge
```

## 3. Build and install (the easy way)
```bash
python3 kaoforge.py doctor     # finds the game and checks the compiler; fix anything it reports first
python3 kaoforge.py install    # builds the loader + all mods and copies them into the game folder
```
If the game is not found automatically, give its folder (the one that contains `kao2.exe`):
```bash
python3 kaoforge.py --game "/path/to/Kao the Kangaroo Round 2" install
```
(or set the environment variable `KAOFORGE_GAME`). Afterwards, on **Linux/Proton only**, set the Steam launch option from the [README](../README.md#install).

To rebuild only the mods (the loader stays as it is), use `python3 kaoforge.py mods`, then type `reload` in the in-game console.

## 4. Build by hand (what the script runs)
Useful if you want to see exactly what happens or build somewhere else. Run from the repository root.

**The loader** (replaces the game's `glut32.dll`; `-Wl,--kill-at` is required so the exports keep their plain names):
```bash
mkdir -p build
(cd loader && i686-w64-mingw32-gcc -O2 -Wall -shared -o ../build/glut32.dll kaoforge.c glut32.def \
    -static-libgcc -Wl,--kill-at -lopengl32 -lgdi32)
```

**A mod** (here `hello`; the folder name is the DLL name):
```bash
i686-w64-mingw32-gcc -O2 -shared -I include -o build/hello.dll mods/hello/hello.c \
    -static-libgcc -lopengl32 -lgdi32
```

**Check the result** — both files must be 32-bit Windows DLLs:
```bash
file build/glut32.dll build/hello.dll
# glut32.dll: PE32 executable for MS Windows 4.00 (DLL), Intel i386 ...
```

**Install by hand:** in the game folder copy the original `glut32.dll` to `glut32_real.dll` (once), put your built `glut32.dll` in its place,
and put the mod DLLs into a `mods/` folder there. The script does the same, plus it replaces files through a temporary file so a running game is not disturbed.

## Problems
| Message | Cause / fix |
|---|---|
| `i686-w64-mingw32-gcc: command not found` | the compiler is not installed, or its name differs on your system (`ls /usr/bin/*mingw32-gcc*`). |
| `cannot find -lopengl32` / `-lgdi32` | an incomplete MinGW installation; reinstall the MinGW-w64 package (the import libraries come with it). |
| the build works but the game ignores the loader | you built with a 64-bit compiler (`x86_64-w64-mingw32-gcc`) — `file` must say *Intel i386*. |
| `doctor` says the game is not found | pass `--game DIR` or set `KAOFORGE_GAME`. |
| the log says `UNSUPPORTED BUILD` | your `kao2.exe` is not the supported 8,423,568-byte build; the loader deliberately does nothing. |
| warnings about `format-truncation` | harmless. |
