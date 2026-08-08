# darktable (r4debox fork)

a darktable fork with reverse-engineered Hasselblad Phocus color science baked in.

this isn't a theme pack or a config tweak. this is actual in-tree IOP modules compiled into darktable itself — binary patches, code caves, custom .cube LUT loading, steganographic watermarking, and the full Phocus 4.1 color pipeline extracted from a Phocus install and turned into a loadable 33^3 trilinear lookup table.

## what's in here

### phocus_color
the big one. reverse-engineered the Phocus color pipeline — color matrix, CbCr chroma correction, film curve — all of it — and baked it into a single 3D LUT per sensor. 22 Hasselblad sensor types supported, from the old 22mp CCD to the 100MP3 CMOS. LUTs load at runtime from `~/.config/darktable/luts/`. trilinear interpolation with OpenMP. strength slider for blend control.

sensors supported:
- 22mpc, 31mp, 31mpc, 39mp, 39mpc
- 40mp, 40mp5, 50mp, 50mpc
- 60mp, 60mpc, 100mp, 100mp2, 100mp3
- 20mp1inch, 22mp (old CCD line)
- 50mp2, 80mp, 80mp52
- ixpress, pro-v, tz

### phocus_layers
Phocus-style local adjustment layers with masks. in-tree IOP module.

### stegtag
DCT-domain steganographic watermarking with BCH(15,7) error correction. embeds artist/copyright tags into exported images. survives JPEG compression down to quality 60. spread spectrum, pseudorandom block selection, adaptive strength based on local variance.

### code patches
- binary patches to the Phocus 4.1 console (code caves, API hooks)
- .NET CLR hosting from native DLL injection
- WinDBG-compatible debugging stack for Phocus VM reverse engineering
- KVM passthrough workflow (QMP + GDB stub on the Windows guest)

## building

standard darktable build, but use `/usr/local` as the install prefix. do NOT use `/usr` — it will clobber the system darktable and leave orphaned .so files that crash the stock version.

```
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

run from `build/bin/darktable`. do not `cmake --install` to system unless you know what you're doing.

## the LUT files

the `.cube` files are the ground truth. they were extracted from the Phocus Colormaps — every `Colormaps/*.xml` sensor file parsed, gamma 2.2 decoded, response curves baked into standard 33^3 .cube format. do not regenerate them from the XMLs unless you know the exact encoding chain. the originals are in `~/.config/darktable/luts/`.

## what this is not

this is not a production darktable release. this is a reverse engineering project. the color science is real and tested against Phocus output, but the integration is a fork — custom IOP modules, patched pipeline order, the whole nine yards. if you want stable darktable, use upstream.

## origin

built by jane in denver. locksmith, welder, hardware hacker. reverse engineered the Phocus 4.1 color pipeline and debug key system because hasselblad doesn't document it and i needed it to work.

original darktable: [https://www.darktable.org/](https://www.darktable.org/)
upstream source: [https://github.com/darktable-org/darktable](https://github.com/darktable-org/darktable)
