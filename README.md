# PAYDAY 2 Canted Display Fix (VR)

A [SuperBLT](https://superblt.znix.xyz/) mod that fixes the misaligned eye views in PAYDAY 2 VR on the
**Valve Steam Frame**.

Without it, the left and right eye images don't line up and you have to go cross-eyed to fuse them.
The mismatch has a vertical component, which is why rolling your head 90 degrees makes the views fuse.

## Important: wake the headset before starting the game

The headset **must not be in sleep mode** when you launch PAYDAY 2. If it's asleep at startup, the
game crashes (`PAYDAY2.exe+1fd381`). This happens with or without mods installed, so it is not caused
by this mod. Put the headset on, or otherwise wake it, before you start the game.

## How it works

PAYDAY 2's Lua VR API has no per-eye transform or projection, so the fix lives in a small native
plugin (`canted_fix.dll`) that the mod loads with `blt.load_native` after VR has initialised.

The plugin takes the game's `IVRSystem` / `IVRCompositor` from the stock `openvr_api.dll` and patches
a few vtable slots. On each `Submit` it shifts the submitted texture bounds (`VRTextureBounds_t`) of
each eye by half the configured offset, in opposite directions, so the two views line up.

No game files are replaced. (Replacing `openvr_api.dll` with a proxy doesn't work: the game checks its
shipped DLLs and reports "Installation corrupted".)

## Requirements

- PAYDAY 2 with [SuperBLT](https://superblt.znix.xyz/) installed
- SteamVR

## Install

1. Download the latest zip from the
   [Releases tab](https://github.com/Lordmau5/pd2-canted-vr-fix/releases).
2. Extract it into `PAYDAY 2\mods\`, so you get a folder like `mods\pd2-canted-vr-fix\`
   that contains `mod.txt`, `load.lua` and `canted_fix.dll`.
3. Wake the headset, then start the game in VR.

To install from source instead, build the plugin (see [Building](#building)) and copy the `mod`
folder into `PAYDAY 2\mods\` under a name of your choice.

## Configuration

On first run the plugin writes `openvr_proxy.ini` next to `PAYDAY2.exe`:

```ini
[fix]
enabled=1
cant_scale=1.0
vertical_pct=-0.75
horizontal_pct=-0.75
hotkeys=1

[log]
calls_per_function=8
```

| Key | Meaning |
| --- | --- |
| `enabled` | Turn the fix on or off. |
| `vertical_pct` / `horizontal_pct` | Eye image offset in percent of the eye image. Each eye moves half of it, in opposite directions. The defaults are tuned in-headset on a Steam Frame. |
| `cant_scale` | Multiplier for the cant-angle compensation. The Steam Frame reports 0 degrees of cant, so this has no effect there. |
| `hotkeys` | Enable the in-game hotkeys. |
| `calls_per_function` | How many calls of each hooked OpenVR function to write to the log. |

### Hotkeys

Use these to tune the offsets in the headset. Changes are saved to the ini right away. Steps are
0.25 percent.

| Keys | Action |
| --- | --- |
| Ctrl+F9 | Toggle the fix |
| Ctrl+F10 / Ctrl+F11 | Vertical offset - / + |
| Ctrl+Shift+F10 / Ctrl+Shift+F11 | Horizontal offset - / + |
| Ctrl+Shift+F9 | Reset both offsets to 0 |
| Ctrl+F8 | Write a marker with the current state to the log |

The log is `openvr_proxy.log` next to `PAYDAY2.exe`.

## Building

Needs Visual Studio or the Visual Studio Build Tools with the **Desktop development with C++**
workload (x64 tools). `clang-cl` is used when it's on `PATH`, otherwise MSVC `cl`.

```bat
build.cmd
```

The script finds Visual Studio with `vswhere` and sets up the x64 environment itself. If you run it
from an "x64 Native Tools Command Prompt", it uses that environment instead. The output goes to
`build\` and is copied to `mod\canted_fix.dll`.

## Known issues

- The game crashes on startup when the headset is asleep. See above.

## Licence

This mod is licensed under the GNU General Public License, version 2 or (at your option) any later
version. See [LICENSE](LICENSE). SuperBLT requires native plugins to declare a GPL-compatible
licence, and the plugin declares this one.

`src/openvr.h` is from Valve's [OpenVR SDK](https://github.com/ValveSoftware/openvr) (2.15.6). It is
not covered by the GPL notice above: it keeps Valve's own BSD 3-Clause licence, included as
[src/LICENSE-openvr](src/LICENSE-openvr).

## AI Disclaimer
![AI](https://media1.tenor.com/m/mXFH6faI_a4AAAAC/artificial-intelligence-moistcritikal.gif)

This mod was fully written by Claude Opus 5.5 in combination with the [universal-modder](https://github.com/rehan-remade/universal-modder) repository.