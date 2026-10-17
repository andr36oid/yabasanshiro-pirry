# Third-party components

Yaba Sanshiro Pirry Edition is distributed under the GNU General Public License
version 2 or later (see `LICENSE`). The binary for the R36S contains or uses the
following components. Their license texts are in the source tree at the paths given.

| Component | Use | License | Source / path |
|---|---|---|---|
| Yaba Sanshiro (devMiyax) | emulator, base of this fork | GPL-2.0-or-later | https://github.com/devmiyax/yabause |
| Yabause | emulator core | GPL-2.0-or-later | https://yabause.org, `yabause/` |
| Musashi (Karl Stenerud) | 68000 CPU core of the sound CPU | MIT | `yabause/src/musashi/readme.txt` |
| rcheevos | RetroAchievements client | MIT | https://github.com/RetroAchievements/rcheevos, `yabause/src/retro_arena/rcheevos/LICENSE` |
| nanogui-sdl (Wenzel Jakob, SDL port) | menu | BSD-3-Clause | `yabause/src/retro_arena/nanogui-sdl/LICENSE.txt` |
| NanoVG and fontstash (Mikko Mononen) | menu and on-screen text rendering | zlib | license in the header of `yabause/src/nanovg/nanovg.h`, `fontstash.h` |
| Eigen | used by nanogui (git submodule) | MPL-2.0 | https://gitlab.com/libeigen/eigen, `yabause/src/retro_arena/nanogui-sdl/ext/eigen` |
| MD5 (L. Peter Deutsch) | disc hash for RetroAchievements, inside rcheevos | zlib-style (see file header) | `yabause/src/retro_arena/rcheevos/src/rhash/md5.c` |
| stb_image, stb_truetype (Sean Barrett) | image and font loading inside NanoVG | public domain / MIT | `yabause/src/retro_arena/nanogui-sdl/ext/nanovg` |
| Roboto font | menu font | Apache-2.0 | `yabause/src/retro_arena/nanogui-sdl/resources` |
| Entypo icon font (Daniel Bruce) | menu icons | CC BY-SA 4.0 | `yabause/src/retro_arena/nanogui-sdl/resources/entypo.ttf`, http://www.entypo.com |
| pugixml | XML parsing | MIT | `yabause/src/retro_arena/pugixml/pugixml_license.txt` |
| nlohmann/json | JSON (input configuration) | MIT | https://github.com/nlohmann/json (fetched at build time) |
| JsonCpp | JSON | MIT / public domain | `yabause/src/json/json.h` |
| libchdr (devMiyax fork) | CHD disc images; bundles LZMA (public domain) and FLAC (BSD-3-Clause) decoders | BSD-3-Clause | https://github.com/devmiyax/libchdr (fetched at build time) |
| zlib | compression | zlib | fetched at build time |
| libpng | PNG images | libpng license | fetched at build time |
| SDL2 | input, window (shared system library) | zlib | system library |
| Boost | filesystem, locale (shared system library) | BSL-1.0 | system library |
| libcurl | HTTPS for RetroAchievements, loaded at run time if installed | curl license | system library |

The libretro core (`yabause/src/libretro`, built for Android by andr36oid) does not use
the menu, RetroAchievements, SDL2, Boost, libcurl or libpng. Instead of the fetched
libchdr and zlib it builds the copy in `yabause/src/libchdr` (taken from the
libretro/yabause `yabasanshiro` branch), and it contains parts of libretro-common:

| Component | License | Path |
|---|---|---|
| libchdr (Romain Tisserand) | BSD-3-Clause | `yabause/src/libchdr/LICENSE.txt` |
| FLAC decoder (Xiph.Org) | BSD-3-Clause | `yabause/src/libchdr/deps/flac-1.3.3/COPYING.Xiph` |
| LZMA SDK (Igor Pavlov) | public domain | `yabause/src/libchdr/deps/lzma-19.00/LICENSE` |
| zlib (Jean-loup Gailly, Mark Adler) | zlib | `yabause/src/libchdr/deps/zlib-1.2.11/README` |
| libretro-common | MIT | license in the file headers, `yabause/src/libretro/libretro-common` |
