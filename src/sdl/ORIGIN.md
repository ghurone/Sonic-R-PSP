# Origin of src/sdl

This folder is the game's code from jnmartin84's Sonic R decompilation,
<https://github.com/jnmartin84/sonic-r>, folder `source/sdl/src`, as of
2026-09-18, under the decompilation's own license (LICENSE here: it may be
downloaded, built and changed, never sold in any form).

The changes made to it for this port are, in short:

- `#ifdef SONICR_PSP` hooks for the PSP port (src/psp): the 60 fps halfway
  frame, the Graphics page options, the GE renderers' entry points, the
  16-bit character lighting table, faster loads, faster menu cursors, a
  main menu without the multiplayer modes (split screen and network);
- fixes that hold on every platform, matched against the 1998 PC
  SONICR.EXE: the menus keep the chosen character and course, Radiant Emerald's start sign hangs over the track, the
  robots' exhaust flame flickers to the end of the cone.
