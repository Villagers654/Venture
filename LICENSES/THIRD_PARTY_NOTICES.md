# Third-party notices

Venture's JIT (`src/jit`) is GPL-3.0-or-later like the rest of Venture. It
descends from Dynarmic; original Dynarmic portions retain 0BSD (notice in
`LICENSES/0BSD.txt`), and files keep their SPDX copyright lines. xbyak is
kept in `src/jit/xbyak` (BSD-3-Clause, `COPYRIGHT` there) because Venture adds
dual-mapped code buffer support to it. The JIT's other libraries (fmt, mcl,
robin-map and, for tests, Catch2 and oaknut) are unmodified and pinned by commit
and archive SHA256 in `cmake/Dependencies.cmake` like every other fetched
dependency; release packages ship their license files under `licenses/<name>`.

The launcher's controller pictures (`src/launcher/assets/controllers/*.png`)
and turnable models (`src/launcher/assets/controllers/*.glb`, the original
models reduced to their meshes, node tree and base color textures) are made
by `tools/controller_art/build_controller_art.py` from the glTF
models of the WebXR Input Profiles project
(`@webxr-input-profiles/assets` 1.0.20, https://github.com/immersive-web/webxr-input-profiles,
models pinned by SHA256 in the script): valve-index, meta-quest-touch-plus,
htc-vive, hp-mixed-reality, pico-4, htc-vive-cosmos, htc-vive-focus-3 and
generic-trigger-squeeze-thumbstick. They are the fallback: when the player has
SteamVR render models installed, the launcher draws those at run time from the
player's own installation and caches only the drawing; no SteamVR or vendor
model is copied into the repository or the package. The bundled pictures and
models are used under the MIT License:

    MIT License

    Copyright (c) 2019 Amazon

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is furnished
    to do so, subject to the following conditions:

    The above copyright notice and this permission notice (including the next
    paragraph) shall be included in all copies or substantial portions of the
    Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
    FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
    OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
    OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The models depict devices whose names and logos are trademarks of their
manufacturers (Valve, Meta, HTC, HP, Pico). The pictures only show which
physical input a binding uses; the license grants no trademark rights and
Venture is not endorsed by these companies.

The runtime's JIT block cache compresses its records with LZ4 (`lib/lz4.c`,
BSD-2-Clause, `lib/LICENSE` in the pinned archive).

## Lucide

The launcher's built-in icon font (`src/launcher/assets/venture-icons.ttf`,
built by `tools/make_icon_font.py` from lucide-static 1.66.0) holds icons from
[Lucide](https://lucide.dev), under the ISC license:

> ISC License
>
> Copyright (c) 2026 Lucide Icons and Contributors
>
> Permission to use, copy, modify, and/or distribute this software for any
> purpose with or without fee is hereby granted, provided that the above
> copyright notice and this permission notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
> WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
> ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
> WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
> ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
> OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
>
> ---
>
> The following Lucide icons are derived from the Feather project:
>
> airplay, alert-circle, alert-octagon, alert-triangle, aperture, arrow-down-circle, arrow-down-left, arrow-down-right, arrow-down, arrow-left-circle, arrow-left, arrow-right-circle, arrow-right, arrow-up-circle, arrow-up-left, arrow-up-right, arrow-up, at-sign, calendar, cast, check, chevron-down, chevron-left, chevron-right, chevron-up, chevrons-down, chevrons-left, chevrons-right, chevrons-up, circle, clipboard, clock, code, columns, command, compass, corner-down-left, corner-down-right, corner-left-down, corner-left-up, corner-right-down, corner-right-up, corner-up-left, corner-up-right, crosshair, database, divide-circle, divide-square, dollar-sign, download, external-link, feather, frown, hash, headphones, help-circle, info, italic, key, layout, life-buoy, link-2, link, loader, lock, log-in, log-out, maximize, meh, minimize, minimize-2, minus-circle, minus-square, minus, monitor, moon, more-horizontal, more-vertical, move, music, navigation-2, navigation, octagon, pause-circle, percent, plus-circle, plus-square, plus, power, radio, rss, search, server, share, shopping-bag, sidebar, smartphone, smile, square, table-2, tablet, target, terminal, trash-2, trash, triangle, tv, type, upload, x-circle, x-octagon, x-square, x, zoom-in, zoom-out
>
> The MIT License (MIT) (for the icons listed above)
>
> Copyright (c) 2013-present Cole Bemis
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
