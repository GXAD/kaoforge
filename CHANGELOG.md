# Changelog

## 0.1.0 – first public version
- Loader: proxy `glut32.dll`, mods as DLLs from `<game>/mods/`, Forge API v4 (`include/kaoforge.h`):
  inline hooks, vtable/IAT/call hooks, byte patches with undo, calling game functions, scene walking, hero control,
  events (level loaded, keys), 2D and 3D drawing, world-to-screen, in-game console with autocomplete, config file, hot-reload (`reload`).
- Built-in extras: mouse look + free orbit camera, fly mode (look-direction flight), speed/jump sliders, soft shadows, FXAA, level selector, prop export.
- Map builder: JSON -> level (scene + PAK) with collision, props exported from the game, example maps.
- Example mods: hello, demo3, showcase.
- Only the Steam build with `kao2.exe` of 8,423,568 bytes is supported; other builds keep the loader inactive.
