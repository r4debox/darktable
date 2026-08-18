# darktable (r4debox fork)

Hasselblad won't give you their color science unless you use their software. their software is abandonware.

This is a darktable fork with custom IOP modules that give you Phocus color, adjustment layers, and steganographic watermarking all in-tree, all compiled into darktable itself.

## what's in here

### phocus_color

reverse-engineered the Phocus 4.1 color pipeline. every color matrix, CbCr correction, film curve decoded from the proprietary Colormaps/*.xml sensor files, gamma 2.2 baked into 33^3 trilinear .cube LUTs. one per sensor, 22 supported. loads at runtime from `~/.config/darktable/luts/`. strength slider, OpenMP-accelerated interpolation.

Supported sensors:
22mpc, 31mp, 31mpc, 39mp, 39mpc, 40mp, 40mp5, 50mp, 50mpc, 60mp, 60mpc, 100mp, 100mp2, 100mp3, 20mp1inch, 22mp, 50mp2, 80mp, 80mp52, ixpress, pro-v, tz

the .cube files are ground truth. do not regenerate them.

### phocus_layers

local adjustment layers with masks. in-tree IOP module.

### stegtag

DCT-domain steganographic watermarking. BCH(15,7) error correction, spread spectrum, pseudorandom block selection. survives JPEG quality 60.

modes: artist (from metadata), copyright (from metadata), custom text, disabled

settings:
- strength 0.01-1.0 (0.3 default)
- seed (0 = auto from image hash)
- adaptive strength based on local variance (default on)
- channel: luma / green / blue
- **save as defaults** saves current settings to darktable conf. next stegtag instance you add loads those instead of the hardcoded defaults.


## building

use `/usr/local`. NOT `/usr`. that clobbers system darktable unless that what you are going for lol.

```
git submodule init && git submodule update
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

run from `build/bin/darktable`. if you break system darktable: `pacman -S darktable` and rm any orphaned `libphocus_*.so` from `/usr/lib/darktable/plugins/`.

## why

because hasselblad thinks their color science is a trade secret you shouldn't be allowed to use outside of their shitty windows only dead software, it's math here it is fuck you. also 300$ for a new battery is ridiculous.

