# Hookshot Windows SDL runtime packaging fix

Upstream v2.0.3, released on 2026-11-08, includes this dependency in the final
installation. The subsequent v2.0.4 retains it and reverts a v2.0.3 shader
translation change associated with dark characters and incorrect lighting.
Prefer that release over applying this v2.0.2 companion patch:
https://github.com/NotRexed/Hookshot/releases/tag/v2.0.4.

This companion patch applies to **Hookshot**, not Lichii. It changes no emulator
rendering behavior. Upstream source: https://github.com/NotRexed/Hookshot at
v2.0.2, 5c9b0bf34a7edd740c13062937b2a049f98c7440, GPL-3.0.

The official v2.0.2 builder bundles sdl2-compat 2.32.74 (`SDL2.dll`) and SDL
3.4.20 (`SDL3.dll`). `nx2/build.py` stages both into its build directory, but
`nx2/make_game.py` includes only SDL2.dll in its Windows final-install list.
Loading the compatibility wrapper then reports `Failed loading SDL3 library.`
The game falls back to XInput, losing support for the connected DualSense.

Apply `hookshot-sdl3-runtime.patch` in a Hookshot source checkout. The one-entry
change includes SDL3.dll in both installation staging and the existing obsolete
root-runtime cleanup when the builder uses a bin directory.

For an already-built local game, copy **the matching SDL3.dll from that builder's
third_party/sdl2 directory** beside the actual game executable (bin/hookshot.exe
when a launcher is present). Do not download arbitrary DLLs. Retain the bundled
SDL license. This does not need a game recompilation.

Validated on Windows with the official builder-app archive
cee5c4d0e4bd2ebf873502365ad6a8f98a9691ecc70b23f31a18b0c60171f1aa:
the previously missing runtime has SHA256
66844F10251CC1C10619ED5D8E4E3224F564900EE6D69BA87D63DE782ADDAE40.
After restart, the runtime reports SDL controller support ready and connects
`DualSense Wireless Controller` through SDL. This establishes the dependency fix;
it does not establish gameplay performance or complete visual correctness.
