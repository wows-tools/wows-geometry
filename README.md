# wows-model-exporter

[![Ubuntu-x86_64](https://github.com/wows-tools/wows-model-exporter/actions/workflows/ubuntu-x86_64.yml/badge.svg)](https://github.com/wows-tools/wows-model-exporter/actions/workflows/ubuntu-x86_64.yml)
[![Documentation](https://github.com/wows-tools/wows-model-exporter/actions/workflows/doxygen.yml/badge.svg)](https://wows-tools.github.io/wows-model-exporter/)

Vibe coded parser and glTF/GLB exporter for World of Warships ship 3D models.

File formats and organization: [MODEL.md](MODEL.md) (ship mesh pipeline — `GameParams`, `assets.bin`, LOD, textures) and [GEOMETRY.md](GEOMETRY.md) (`.geometry` binary layout).

## Build

Install dependencies (Debian/Ubuntu):
```sh
sudo apt install cmake zlib1g-dev libpcre2-dev libmeshoptimizer-dev git clang \
                 libtinygltf-dev python3-dev
```

Clone code:
```sh
git clone --recurse-submodules https://github.com/wows-tools/wows-model-exporter.git
cd wows-model-exporter
```

Compile:
```sh
cmake .
make

./wows-gltf-exporter --help
```

## Tools

### wows-gltf-exporter: full ship GLB assembler

Reads the game files, in particular `GameParams.data` and `assets.bin` (via [wows-depack](https://github.com/wows-tools/wows-depack)),
and stitches the model parts (`.geometry` vertices, `.dds` textures) together to export to `.gltf`/`.glb` format.

```
wows-gltf-exporter -W <wows-dir> -s <ship> -o <output.glb> [options]

Required:
  -W DIR     Root game directory (--wows-dir)
  -s NAME    Ship name or pattern (case-insensitive, words joined as .*w1.*w2.*)
  -o FILE    Output .glb file

Optional:
  -H UPG     Hull upgrade name substring (default: latest)
  -t         Exclude turret / mounted-component models
  -T         Skip DDS texture application
  -Z N       Max texture dimension in pixels (default: 2048)
  -L N       LOD level (-1 = auto, default: -1)
  -D         Include damage/crack geometry (default: excluded)
  -v         Verbose progress output
```

Hidden options (not shown in `--help`, but supported): `-g` / `--gameparams` and `-a` /
`--assets-bin` to pass explicit paths to `GameParams.data` and `assets.bin`, this might be useful when
working on unpacked resources (e.g. after extracting game all game files with
[wows-depack](https://github.com/wows-tools/wows-depack)) instead of relying on auto-detection
under `-W`.

export Kongo with all defaults:
```sh
wows-gltf-exporter -W /path/to/World\ of\ Warships/ -s Kongo_1942 -o kongo.glb
```

export Kongo specific hull, no turrets, Level of Detail 2 (LOD is 0 to 4, with 0 being the highest level):
```sh
wows-gltf-exporter -W /path/to/World\ of\ Warships/ -s Kongo_1942 -H HullB -t -L 2 -o kongo_hullb_lod2.glb
```

### wows-list-ships: enumerate available ships

Lists all ships found in `GameParams.data`, with optional filtering by nation or type:

```
wows-list-ships [-W <wows-dir>] [-n nation] [-t type]

  -W DIR     Root game directory (--wows-dir; auto-detects GameParams.data)
  -n STR     Filter by nation (case-insensitive substring)
  -t STR     Filter by ship type (case-insensitive substring)
```

Can be combined to export all ships to gltf (requires ~40GB):
```sh
wows-list-ships -W /path/to/World\ of\ Warships  | sed 's/[[:space:]].*//' | \
while read line;
do
    echo "------ $line ----";
    wows-gltf-exporter -W /path/to/World\ of\ Warships -s $line -o $line.glb --verbose;
done

### wows-geometry-cli - single-file inspector / exporter

Parses one `.geometry` file, prints its structure, or exports it as GLB.

```
wows-geometry-cli -i <file.geometry> [-p] [-v] [-o output.glb] [-s 0,1,...]

  -i FILE    Input .geometry file
  -p         Print parsed structure
  -v         Print all vertex data (use with -p)
  -o FILE    Export submeshes to GLB (--output-glb)
  -s 0,1,…   Comma-separated vertex section indices to export (default: all)
```

## Library API

`wows-geometry` exposes a C++ API for embedding ship export into your own tools.
The main entry point is `wows_stitch_export_ship()` from `inc/wows-model-exporter.h`.

Full API reference: **[wows-tools.github.io/wows-model-exporter](https://wows-tools.github.io/wows-model-exporter/)**

### Quick start

```cpp
#include "wows-model-exporter.h"

wows_ship_export_options opts;
// all fields have sensible defaults; override only what you need:
// opts.hull_upgrade         = "HullB";   // empty = latest
// opts.with_turrets         = false;
// opts.with_textures        = false;
// opts.max_tex_size         = 1024;
// opts.lod_level            = 0;         // -1 = auto
// opts.exclude_damage       = false;
// opts.gameparams_path      = "/explicit/path/GameParams.data";  // auto-detected if empty
// opts.wows_assets_bin_path = "/explicit/path/assets.bin";       // auto-detected if empty

bool ok = wows_stitch_export_ship("/path/to/World\ of\ Warships/", "Kongo_1942", "kongo.glb", opts);
```

`wows_stitch_export_ship` handles everything: auto-detecting `GameParams.data` and
`assets.bin` under `game_dir`, resolving hull and turret models, loading HP_
transforms and BlendBone corrections, decoding geometry, merging, texturing, and
writing the GLB. Errors are printed to stderr; Python is initialised and finalised
internally.

### Export fields

| Field                  | Default | Description                                                         |
|------------------------|---------|---------------------------------------------------------------------|
| `gameparams_path`      | `""`    | Path to `GameParams.data`; auto-detected from `game_dir` if empty   |
| `wows_assets_bin_path` | `""`    | Path to `assets.bin`; auto-detected from `game_dir` if empty        |
| `hull_upgrade`         | `""`    | Hull upgrade name substring (e.g. `"HullB"`); empty = latest        |
| `with_turrets`         | `true`  | Include turret / mount geometry                                     |
| `with_textures`        | `true`  | Apply DDS albedo textures                                           |
| `max_tex_size`         | `2048`  | Maximum texture dimension in pixels                                 |
| `lod_level`            | `-1`    | LOD level; `-1` = auto-select highest triangle count                |
| `exclude_damage`       | `true`  | Strip damage/crack render sets                                      |

### Verbose output

```cpp
wows_stitch_verbose = true;   // prints progress to stderr
```

### Linking

```cmake
target_link_libraries(my_tool wows-geometry)
```

Python 3 and all other dependencies are already linked into `wows-geometry`.

### Lower-level API

If you need finer control (custom merge logic, streaming parts, etc.) the
building-block functions used internally by `wows_stitch_export_ship` are also public:
`wows_stitch_find_hull_geoms`, `wows_stitch_geom_to_model`,
`wows_stitch_merge_parts`, `wows_stitch_apply_textures`, and the `wows_assets_bin_*`
helpers in `inc/wows-assets-bin.h`.

# Unit Tests

```sh
sudo apt install libcunit1-dev
```

To build and run tests with coverage (requires GCC):

```sh
cmake -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCOVERAGE=ON -DBUILD_TESTS=ON .
make
make test
make coverage
```
