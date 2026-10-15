# Ratchet & Clank: Up Your Arsenal (Ratchet & Clank 3) - RE:COPILE Edition

Experimental Linux recompilation research and RE:COPILE integration by
[MASELKO-95](https://github.com/MASELKO-95), targeting the player's own PAL
**SCES-52456** edition of Ratchet & Clank 3 / Up Your Arsenal.

**Development source release. Full gameplay is not verified.** The existing local
experiment reaches the introduction and title screen. An incomplete menu has
been observed with Vulkan; levels, audio, complete controls and a playable
new game still require work. Windows has not been validated. Downloading this
repository does not provide a built or playable game.

RE:COPILE Edition identifies the integration and experimental runtime build.
It is not an official edition of Ratchet & Clank. Original game authors and
rightsholders retain their rights. This project is not affiliated with Sony
or Insomniac Games.

## What is included

- Original project tooling and metadata for the PAL boot program.
- Reviewed PS2Recomp runtime patches, a Vulkan GS renderer, FSR1 integration
  and native runtime tests.
- Build instructions and a direct entry point for the
  [RE:COPILE library](https://github.com/MASELKO-95/recopile).

No ISO, BIOS, extracted game data, generated guest C++, boot ELF, screenshots,
saves or compiled game executable is included. `third_party/`, `.local/` and
`build/` are local workspaces and are excluded from publication.

## Requirements and build

Linux x86-64 with AVX2, a hardware Vulkan 1.1 driver, SDL2/Vulkan development
headers, `glslangValidator`, Python 3, Git, CMake, a C++ compiler and 7-Zip.
CPU fallback is not used by the Vulkan renderer. FSR1 uses AMD's EASU/RCAS code.

1. Run `python3 tools/fetch-dependencies.py` to obtain the exact upstream revisions
   listed in [THIRD_PARTY.md](THIRD_PARTY.md). Existing directories are checked,
   never reset or overwritten.
2. Run `bash tools/build-toolchain.sh`.
3. Configure your own complete PAL disc image:

   ```sh
   python3 tools/recopile.py configure --iso /path/to/your/ratchet3.iso --owned
   python3 tools/recopile.py prepare
   ```

   This links your ISO locally, extracts files with 7-Zip and recovers the boot
   ELF using the project's tools. The original image remains in place.
4. Run `python3 tools/recompile-boot.py`, then `bash tools/build-native.sh`.
5. Run `python3 tools/recopile.py run`, or connect this checkout in the
   RE:COPILE game details. Generated code and all game files remain local.

The helper does not turn an unsupported disc or a different region into a
working port. Do not distribute any output derived from your game files.

## RE:COPILE integration

In the Ratchet 3 game details, choose the **RE:COPILE Edition** source, connect
this local project folder after building, and select **Run experiment**. The
library tracks the process; the port opens directly, without another launcher.
An existing local research checkout can also be connected.

Enter = Start; X / Space = Cross; arrows = D-pad; C = Circle. These are runtime
mappings, not confirmation that every game screen accepts them.

## Validation

```sh
bash tools/test-iop-runtime.sh
bash tools/test-native-runtime.sh
bash tools/test-vulkan-runtime.sh
python3 tools/smoke-native.py --seconds 10 --name local-smoke
```

Runtime unit tests do not establish working gameplay. Real game validation
requires the player's own data and must record the exact boundary reached.
The boot uses static EE recompilation; IOP/VU and hardware behavior still
include runtime interpretation. Code for the game's level overlays is not yet
linked into the native game.

## Authors and licenses

RE:COPILE integration: MASELKO-95. PS2Recomp: ran-j and contributors. Wrench:
chaoticgd and contributors. raylib: raysan5 and contributors. FSR1: AMD.
Original RE:COPILE work in this repository is licensed under GNU GPLv3
(`GPL-3.0-only`); see [LICENSE](LICENSE) and [NOTICE](NOTICE). Preserve author
and license notices, mark distributed modifications with their date and supply
Corresponding Source as required by GPLv3. Commercial use is allowed. Wrench
code retains its GPL-3.0-or-later terms. Other dependencies keep their own
licenses; see [THIRD_PARTY.md](THIRD_PARTY.md) for sources and pinned revisions.

Optional support: [MASELKO-95 on GitHub Sponsors](https://github.com/sponsors/MASELKO-95).
