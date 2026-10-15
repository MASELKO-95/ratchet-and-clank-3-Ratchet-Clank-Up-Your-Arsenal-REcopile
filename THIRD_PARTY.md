# Narzędzia i wersje

- PS2Recomp: https://github.com/ran-j/PS2Recomp
  - commit: `2c5fbb9389e11dd95693385969490c9e8e6f57b4`
  - licencja: GPL-3.0 (patrz plik LICENSE w upstream)
  - lokalna poprawka: `patches/ps2recomp-iop-entry-arguments.patch`:
    argumenty startowe IRX, odczyt modułu z pamięci IOP oraz błąd po wyczerpaniu limitu instrukcji.
- raylib: https://github.com/raysan5/raylib
  - tag: `5.5`, commit: `c1ab645ca298a2801097931d1079b10ff7eb9df8`
  - licencja: zlib
- AMD FidelityFX FSR1: https://github.com/GPUOpen-Effects/FidelityFX-FSR
  - tag: `v1.0.2`, commit: `a21ffb8f6c13233ba336352bdff293894c706575`
  - licencja: MIT, pełna treść w nagłówkach `ffx-fsr/ffx_a.h` i `ffx-fsr/ffx_fsr1.h`
  - Vulkan używa oryginalnych funkcji EASU oraz RCAS; wrappery są w `native/shaders/fsr.comp`.
- Vulkan: systemowe nagłówki i loader (sprawdzono 1.4.357); backend wymaga Vulkan 1.1.
- SDL2: systemowa biblioteka, licencja zlib; okno Vulkan i obsługa klawiatury/pada.
- Wrench: https://github.com/chaoticgd/wrench
  - commit: `75f6d86ad055dad5955cf9d771dd8146cbf7d6f6`
  - dekompresor i układ sekcji programu: GPL-3.0-or-later
  - `tools/unpack-wad.cpp` łączy się bezpośrednio z dekompresorem Wrench.
  - `tools/prepare-boot.py` używa opisu `DONOR_UYA_BOOT_ELF_HEADERS`
    z `src/core/elf.cpp`. Wygenerowany ELF ma osobne segmenty dla sekcji.

Pliki gry, ISO i wygenerowany z nich kod pozostają lokalnie w `.local/`.
Źródła narzędzi są lokalnie w `third_party/`. Te katalogi są ignorowane przez Git.

Dodatkowa referencja badawcza:
- https://github.com/OpenRAC/rac3-uya-decomp
  - commit: `7ec854d0d10d05c731f495736e4d3573f4a2a77f`
  - dotyczy NTSC-U `SCUS-97353`; adresy nie są automatycznie przenoszone do PAL.
  - rozdzielone licencje: zobacz LICENSE upstream; nie łączono źródeł z tym projektem.
