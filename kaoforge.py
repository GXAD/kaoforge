#!/usr/bin/env python3
"""KaoForge – mod loader and map toolkit for "Kao the Kangaroo: Round 2" (Steam, runs under Proton/Wine).

  kaoforge.py doctor                   find the game, check the exe and installed pieces
  kaoforge.py install                  build + install the loader (glut32.dll proxy) and all mods from mods/
  kaoforge.py uninstall                restore the original glut32.dll, remove mods and custom levels
  kaoforge.py mods                     rebuild + install only the mods (loader untouched; then use `reload` in the in-game console)
  kaoforge.py catalog [--json]         list props exported from the game (needs an in-game export, see docs/MAPS.md)
  kaoforge.py new MAP.json             write a map template
  kaoforge.py build MAP.json [--level NAME] [--title "Title"] [--no-install]
                                       build a custom level (JSON -> scene -> PAK) and register it in levels.ini
  kaoforge.py levels                   list the levels from levels.ini
  kaoforge.py log [N]                  tail kaoforge.log

Game folder: --game DIR, or the KAOFORGE_GAME environment variable, or auto-detected in the usual Steam locations.
Files are always replaced through a temporary file + rename (a running game keeps its files open; overwriting in place crashes it).
You need your own copy of the game. KaoForge ships no game files."""
import json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
BASE_PAK = '05_wielkie_drzewa'     # donor PAK: generic hero script + resources for custom levels (the game's own file, never copied into this repo)
KNOWN_EXE_SIZE = 8423568           # kao2.exe of the tested Steam build
INI_KEY = b"7ACAISPJAnzxjh8**(!@*)(_))_!C*H&U#J~~~~~~~((@())FFF#"   # XOR obfuscation of the game's *.ini files


def find_game():
    args = sys.argv
    if '--game' in args:
        i = args.index('--game'); g = args[i + 1]; del args[i:i + 2]; return g
    if os.environ.get('KAOFORGE_GAME'): return os.environ['KAOFORGE_GAME']
    home = os.path.expanduser('~'); name = 'Kao the Kangaroo Round 2'
    roots = [f'{home}/.steam/steam', f'{home}/.local/share/Steam', f'{home}/.var/app/com.valvesoftware.Steam/.local/share/Steam',
             'C:/Program Files (x86)/Steam', 'C:/Program Files/Steam']
    libs = []
    for r in roots:                                            # extra Steam libraries
        vdf = os.path.join(r, 'steamapps', 'libraryfolders.vdf')
        if os.path.exists(vdf):
            for line in open(vdf, errors='ignore'):
                if '"path"' in line: libs.append(line.split('"')[3])
    for r in roots + libs:
        p = os.path.join(r, 'steamapps', 'common', name)
        if os.path.exists(os.path.join(p, 'kao2.exe')): return p
    return None


GAME = find_game()


def need_game():
    if not GAME or not os.path.exists(os.path.join(GAME, 'kao2.exe')):
        sys.exit('Game not found. Use --game DIR or set KAOFORGE_GAME to the folder containing kao2.exe.')
    return GAME


def media(): return os.path.join(need_game(), 'media')
def xor(d): return bytes(b ^ INI_KEY[i % len(INI_KEY)] for i, b in enumerate(d))


def install_file(src, dst):
    tmp = dst + '.new'
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


def levels_ini(): return xor(open(os.path.join(media(), 'levels.ini'), 'rb').read()).decode('latin1')


def ensure_level(name, title, num):
    txt = levels_ini()
    for line in txt.splitlines():
        if line.startswith('+level:"') and line[8:].split(';')[0].strip() == name: return False
    bak = os.path.join(media(), 'levels.ini.orig')
    if not os.path.exists(bak): shutil.copyfile(os.path.join(media(), 'levels.ini'), bak)
    txt = txt.rstrip('\r\n') + f'\r\n+level:"{name};  {title};  {num}; {name}"\r\n'
    tmp = os.path.join(media(), 'levels.ini.new')
    open(tmp, 'wb').write(xor(txt.encode('latin1')))
    os.replace(tmp, os.path.join(media(), 'levels.ini'))
    return True


def find_cc():
    """A 32-bit MinGW-w64 GCC: $KAOFORGE_CC, `i686-w64-mingw32-gcc`, or a plain `gcc` whose target is i686 MinGW
    (this is how the portable Windows toolchains such as w64devkit / WinLibs expose it)."""
    cands = [os.environ.get('KAOFORGE_CC')] + ['i686-w64-mingw32-gcc', 'gcc']
    for c in filter(None, cands):
        path = shutil.which(c)
        if not path: continue
        try: target = subprocess.run([path, '-dumpmachine'], capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception: continue
        if target.startswith('i686') and 'mingw' in target: return path
        if c == os.environ.get('KAOFORGE_CC'): print(f'WARNING: KAOFORGE_CC target is "{target}", expected i686 MinGW; using it anyway'); return path
    return None


def cc_cmd():
    cc = find_cc()
    if not cc: sys.exit('No 32-bit MinGW-w64 GCC found (need target i686-w64-mingw32). See docs/BUILDING.md, or set KAOFORGE_CC to the compiler.')
    return [cc, '-O2', '-Wall', '-Wno-misleading-indentation', '-Wno-unused-function', '-shared']


def build_loader():
    out = os.path.join(ROOT, 'build', 'glut32.dll'); os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.check_call(cc_cmd() + ['-o', out, 'kaoforge.c', 'glut32.def', '-static-libgcc', '-Wl,--kill-at', '-lopengl32', '-lgdi32'],
                          cwd=os.path.join(ROOT, 'loader'))
    return out


def cmd_doctor(a):
    g = need_game(); print('game folder :', g)
    exe = os.path.join(g, 'kao2.exe'); size = os.path.getsize(exe)
    print('kao2.exe    :', size, 'bytes', '(tested build)' if size == KNOWN_EXE_SIZE else f'(UNTESTED build, expected {KNOWN_EXE_SIZE}; the loader will stay inactive)')
    real = os.path.exists(os.path.join(g, 'glut32_real.dll'))
    print('loader      :', 'installed' if real else 'not installed (run: kaoforge.py install)')
    mods = os.path.join(g, 'mods')
    print('mods        :', ', '.join(sorted(f[:-4] for f in os.listdir(mods) if f.endswith('.dll'))) if os.path.isdir(mods) else 'none')
    cc = find_cc()
    print('compiler    :', cc if cc else 'MISSING: a 32-bit MinGW-w64 GCC (target i686-w64-mingw32) - see docs/BUILDING.md')
    print('Linux/Proton only - Steam launch option: WINEDLLOVERRIDES="glut32=n,b" %command%   (not needed on Windows)')


def cmd_install(a):
    g = need_game()
    size = os.path.getsize(os.path.join(g, 'kao2.exe'))
    if size != KNOWN_EXE_SIZE: print(f'WARNING: kao2.exe is {size} bytes, tested build is {KNOWN_EXE_SIZE}. The loader will refuse to patch an unknown build.')
    dll = build_loader()
    real = os.path.join(g, 'glut32_real.dll')
    if not os.path.exists(real): shutil.copyfile(os.path.join(g, 'glut32.dll'), real)     # keep the game's own glut32
    install_file(dll, os.path.join(g, 'glut32.dll'))
    print('loader installed. Linux/Proton only: add to the Steam launch options  WINEDLLOVERRIDES="glut32=n,b" %command%  (not needed on Windows)')
    cmd_mods(a)


def cmd_mods(a):
    g = need_game(); mods = os.path.join(ROOT, 'mods'); dst = os.path.join(g, 'mods'); os.makedirs(dst, exist_ok=True)
    have = set()
    for m in sorted(os.listdir(mods)):
        d = os.path.join(mods, m); srcs = [f for f in os.listdir(d) if f.endswith('.c')] if os.path.isdir(d) else []
        if not srcs: continue
        have.add(m); out = os.path.join(d, m + '.dll')
        subprocess.check_call(cc_cmd() + ['-I', os.path.join(ROOT, 'include'), '-o', out] + [os.path.join(d, f) for f in srcs] +
                              ['-static-libgcc', '-lopengl32', '-lgdi32'])
        install_file(out, os.path.join(dst, m + '.dll')); print('mod', m, 'installed')
    for f in os.listdir(dst):
        if f.endswith('.dll') and f[:-4] not in have: os.remove(os.path.join(dst, f)); print('removed stale mod', f)
    print('mods are loaded at game start; in a running game type `reload` in the console (` or F10)')


def cmd_uninstall(a):
    g = need_game(); real = os.path.join(g, 'glut32_real.dll')
    if os.path.exists(real): os.replace(real, os.path.join(g, 'glut32.dll')); print('original glut32.dll restored')
    shutil.rmtree(os.path.join(g, 'mods'), ignore_errors=True)
    bak = os.path.join(media(), 'levels.ini.orig')
    if os.path.exists(bak): os.replace(bak, os.path.join(media(), 'levels.ini')); print('levels.ini restored')
    for f in os.listdir(media()):
        if f.endswith('.PAK') and os.path.exists(os.path.join(ROOT, 'build', f)): os.remove(os.path.join(media(), f)); print('removed', f)
    print('done (logs/configs: kaoforge.log, kaoforge.ini, kaoforge_mods.ini, kaoforge_export/ are left in the game folder)')


def cmd_levels(a):
    for line in levels_ini().splitlines():
        if line.startswith('+level:'): print(line[8:].rstrip('"'))


def cmd_catalog(a):
    import tools_props
    exp = os.path.join(need_game(), 'kaoforge_export')
    if not os.path.isdir(exp): sys.exit('No export yet: in the game open Home -> "Export props from ALL levels" (see docs/MAPS.md).')
    props, textures = tools_props.collect(exp)
    rows = []
    for k, (lvl, parts) in sorted(props.items()):
        ms, lo, hi = tools_props.prop_meshes(parts)
        if ms: rows.append({'prop': k, 'level': lvl, 'meshes': len(ms), 'size': [round(hi[i] - lo[i]) for i in range(3)]})
    out = os.path.join(ROOT, 'build', 'catalog.json'); os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(rows, open(out, 'w'), indent=1)
    if '--json' in a: print(json.dumps(rows, indent=1)); return
    for r in rows: print(f"{r['prop']:42s} {r['level']:26s} {r['size'][0]:6d} x {r['size'][1]:6d} x {r['size'][2]:6d}")
    print(f'{len(rows)} props, {len(textures)} textures  ->  {out}')


def cmd_new(a):
    spec = {'floor': {'tiles': 1, 'cells': 100, 'cell': 200.0, 'z': -200.0}, 'sky': [0.45, 0.62, 0.85], 'ambient': [0.42, 0.45, 0.52],
            'sun': {'color': [0.80, 0.74, 0.62], 'dir': [500.0, 350.0, 900.0]},
            'cubes': [{'name': 'block01', 'box': [600, -300, 0, 900, 300, 150]}], 'barrels': [[500, 0], [500, 300]], 'props': []}
    json.dump(spec, open(a[0], 'w'), indent=2); print('template written:', a[0], '(prop names: kaoforge.py catalog)')


def cmd_build(a):
    import tools_gen_grid, tools_ar
    level = a[a.index('--level') + 1] if '--level' in a else '99_grid'
    title = a[a.index('--title') + 1] if '--title' in a else level
    tools_gen_grid.GAME_DIR = need_game()
    spec = json.load(open(a[0])); build = os.path.join(ROOT, 'build'); os.makedirs(build, exist_ok=True)
    data = tools_gen_grid.build(spec); ar = os.path.join(build, level + '.ar'); open(ar, 'wb').write(data)
    tools_ar.parse(data)                                                   # validation: the scene must parse to the end
    print(f'scene {ar}: {len(data)} B, parses OK')
    pak = os.path.join(build, level + '.PAK')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'tools_pak_rename.py'),
                           os.path.join(media(), BASE_PAK + '.PAK'), pak, BASE_PAK, level, ar])
    if '--no-install' in a: print('PAK:', pak); return
    install_file(pak, os.path.join(media(), level + '.PAK'))
    if ensure_level(level, title, 98): print('added to levels.ini:', level)
    print(f'installed {level}.PAK - in game: Home menu -> Level selector -> Load')


def cmd_log(a):
    n = int(a[0]) if a else 40
    lines = open(os.path.join(need_game(), 'kaoforge.log'), encoding='utf-8', errors='replace').read().splitlines()
    print('\n'.join(lines[:1] + ['...'] + lines[-n:]))


CMDS = {'doctor': cmd_doctor, 'install': cmd_install, 'uninstall': cmd_uninstall, 'mods': cmd_mods, 'catalog': cmd_catalog, 'new': cmd_new,
        'build': cmd_build, 'levels': cmd_levels, 'log': cmd_log}

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        print(__doc__); sys.exit(1)
    CMDS[sys.argv[1]](sys.argv[2:])
