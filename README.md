# wows-model-exporter

[![Ubuntu-x86_64](https://github.com/wows-tools/wows-model-exporter/actions/workflows/ubuntu-x86_64.yml/badge.svg)](https://github.com/wows-tools/wows-model-exporter/actions/workflows/ubuntu-x86_64.yml)
[![Coverage Status](https://coveralls.io/repos/github/wows-tools/wows-model-exporter/badge.svg?branch=main)](https://coveralls.io/github/wows-tools/wows-model-exporter?branch=main)
[![Documentation](https://github.com/wows-tools/wows-model-exporter/actions/workflows/doxygen.yml/badge.svg)](https://wows-tools.github.io/wows-model-exporter/)

Parser and GLB exporter for World of Warships `.geometry` 3D model files.

## Format

The `.geometry` format is documented in [FORMAT.md](FORMAT.md).

In brief: vertex and index data are meshoptimizer-compressed (ENCD blocks).
Ship geometry is split across multiple part files (`_Bow`, `_MidFront`, etc.)
that share a common coordinate space. LOD levels, render sets, HP_ hardpoint
transforms, and material paths are stored separately in `assets.bin`.

## Tools

### `wows-geometry-cli` — single-file inspector / exporter

Parses one `.geometry` file, prints its structure, or exports it as GLB.

```
wows-geometry-cli -i <file.geometry> [-p] [-v] [-g output.glb] [-s 0,1,...]

  -i FILE    Input .geometry file
  -p         Print parsed structure
  -v         Print all vertex data (use with -p)
  -g FILE    Export submeshes to GLB
  -s 0,1,…   Comma-separated vertex section indices to export (default: all)
```

### `wows-gltf-exporter` — full ship GLB assembler

Reads `GameParams.data` to find hull and mount-point models, loads HP_ transforms
and BlendBone corrections from `assets.bin`, decodes DDS textures, and writes a
single textured GLB containing the hull and all turret/weapon mounts.

`GameParams.data` and `assets.bin` are auto-detected under `-d` if not supplied
explicitly (recursive scan, up to 3 levels, newest version wins).

```
wows-gltf-exporter -d <game-dir> -s <ship> -o <output.glb> [options]

Required:
  -d DIR     Root game directory
  -s NAME    Ship name or pattern (case-insensitive, words joined as .*w1.*w2.*)
  -o FILE    Output .glb file

Optional:
  -g FILE    GameParams.data (auto-detected from -d if omitted)
  -a FILE    assets.bin      (auto-detected from -d if omitted)
  -H UPG     Hull upgrade name substring (default: latest)
  -t         Exclude turret / mounted-component models
  -T         Skip DDS texture application
  -Z N       Max texture dimension in pixels (default: 2048)
  -L N       LOD level (-1 = auto, default: -1)
  -D         Include damage/crack geometry (default: excluded)
  -v         Verbose progress output
```

**Example — export Kongo with all defaults:**
```sh
wows-gltf-exporter -d /opt/wows -s Kongo -o kongo.glb
```

**Example — specific hull, no turrets, LOD 0:**
```sh
wows-gltf-exporter -d /opt/wows -s Kongo -H HullB -t -L 0 -o kongo_hullb.glb
```

## Library API

`wows-geometry` exposes a C++ API for embedding ship export into your own tools.
The main entry point is `stitch_export_ship()` from `inc/stitch.h`.

Full API reference: **[wows-tools.github.io/wows-model-exporter](https://wows-tools.github.io/wows-model-exporter/)**

### Quick start

```cpp
#include "stitch.h"

ShipExportOptions opts;
// all fields have sensible defaults; override only what you need:
// opts.hull_upgrade    = "HullB";   // empty = latest
// opts.with_turrets    = false;
// opts.with_textures   = false;
// opts.max_tex_size    = 1024;
// opts.lod_level       = 0;         // -1 = auto
// opts.exclude_damage  = false;
// opts.gameparams_path = "/explicit/path/GameParams.data";  // auto-detected if empty
// opts.assets_bin_path = "/explicit/path/assets.bin";       // auto-detected if empty

bool ok = stitch_export_ship("/opt/wows", "Kongo", "kongo.glb", opts);
```

`stitch_export_ship` handles everything: auto-detecting `GameParams.data` and
`assets.bin` under `game_dir`, resolving hull and turret models, loading HP_
transforms and BlendBone corrections, decoding geometry, merging, texturing, and
writing the GLB. Errors are printed to stderr; Python is initialised and finalised
internally.

### `ShipExportOptions` fields

| Field | Default | Description |
|-------|---------|-------------|
| `gameparams_path` | `""` | Path to `GameParams.data`; auto-detected from `game_dir` if empty |
| `assets_bin_path` | `""` | Path to `assets.bin`; auto-detected from `game_dir` if empty |
| `hull_upgrade` | `""` | Hull upgrade name substring (e.g. `"HullB"`); empty = latest |
| `with_turrets` | `true` | Include turret / mount geometry |
| `with_textures` | `true` | Apply DDS albedo textures |
| `max_tex_size` | `2048` | Maximum texture dimension in pixels |
| `lod_level` | `-1` | LOD level; `-1` = auto-select highest triangle count |
| `exclude_damage` | `true` | Strip damage/crack render sets |

### Verbose output

```cpp
g_stitch_verbose = true;   // prints progress to stderr
```

### Linking

```cmake
target_link_libraries(my_tool wows-geometry)
```

Python 3 and all other dependencies are already linked into `wows-geometry`.

### Lower-level API

If you need finer control (custom merge logic, streaming parts, etc.) the
building-block functions used internally by `stitch_export_ship` are also public:
`load_hull_info`, `stitch_find_hull_geoms`, `stitch_geom_to_model`,
`stitch_merge_parts`, `stitch_apply_textures`, and the `assets_bin_*` helpers
in `inc/assets_bin.h`.

---

## Build

```sh
# Dependencies (Debian/Ubuntu)
sudo apt install cmake zlib1g-dev libmeshoptimizer-dev libtinygltf-dev \
                 python3-dev libcunit1-dev

cmake .
make
```
