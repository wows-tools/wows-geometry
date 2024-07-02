# Reversing wows .geometry 3D models

# Existing reversing & tools

I'm not the first on trying to reverse the geometry files.

This [Blender plugin](https://github.com/ShadowyBandit/.geometry-converter) was an early attempts from ShadowBandit, but with some [critical bugs apperently](https://github.com/ShadowyBandit/.geometry-converter/issues/3).
Never the less, the [implementation](https://github.com/ShadowyBandit/.geometry-converter/blob/main/io_mesh_geometry/import_geometry.py) could be tremendously helpful given how well the code is documented.
This reverse will probably steal quite a bit from this implementation and at least be crossed-referenced heavily.

On the proprietary side, this [third Party tool](https://gamemodels3d.com/forum/?topic=1348&page=1) also exists. 

## Header

After a quick first pass & looking at the .geometry converter:


```
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
| nV | nV | nV | nV || nI | nI | nI | nI || nV | nV | nV | nV || nI | nI | nI | nI |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<--- n_ver_type -->||<--- n_ind_type -->||<--- n_ver_bloc -->||<--- n_ind_bloc -->|
|     32 bits       ||     32 bits       ||     32 bits       ||     32 bits       |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
| nC | nC | nC | nC || nA | nA | nA | nA || of | of | of | of || of | of | of | of |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<--- n_col_bloc -->||<--- n_arm_bloc -->||<--------------- offset_1 ------------->|
|     32 bits       ||     32 bits       ||                 64 bits                |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
| un | un | un | un || un | un | un | un || un | un | un | un || un | un | un | un |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<--------------- unknow_1 ------------->||<--------------- unknow_2 ------------->|
|                 64 bits                ||                 64 bits                |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
| un | un | un | un || un | un | un | un || un | un | un | un || un | un | un | un |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<--------------- unknow_3 ------------->||<--------------- unknow_4 ------------->|
|                 64 bits                ||                 64 bits                |
+====+====+====+====++====+====+====+====+
| un | un | un | un || un | un | un | un |
+====+====+====+====++====+====+====+====+
|<--------------- unknow_5 ------------->|
|                 64 bits                |
```


| Field                | Size    | Description                                                                                     |
|----------------------|---------|-------------------------------------------------------------------------------------------------|
| `n_ver_type`         | 32 bits | Number of vertex types                                                                          |
| `n_ind_type`         | 32 bits | Number of index types                                                                           |
| `n_ver_bloc`         | 32 bits | Number of vertex blocs                                                                          |
| `n_ind_bloc`         | 32 bits | Number of index blocs                                                                           |
| `n_col_bloc`         | 32 bits | Number of collision blocs                                                                       |
| `n_arm_bloc`         | 32 bits | Number of armor blocs                                                                           |
| `offset_1`           | 64 bits | Offset to data start (always 72 bytes)                                                          |
| `unknown_1`          | 64 bits | Multiple of 8/64bits, value between ~80 and ~1200  | offset?                                    |
| `unknown_2`          | 64 bits | Multiple of 8/64bits, value between ~100 and ~2000 | offset?                                    |
| `unknown_3`          | 64 bits | Large value (few thousands to millions) | vertex count?                                         |
| `unknown_4`          | 64 bits | Large value (few thousands to millions), 0 if no collision bloc | vertex count collision block? |
| `unknown_5`          | 64 bits | Large value (few thousands to millions), 0 if no armor bloc     | vertex count armor block?     |


## Section 1

```
00000040  00 00 00 00 00 00 00 00  88 7a 27 38 00 00 fe 33  |.........z'8...3|
00000050  00 00 00 00 f7 3c 00 00  34 b4 d4 7d 00 00 f2 38  |.....<..4..}...8|
00000060  f7 3c 00 00 b4 0f 00 00  db 4d 2f 09 01 00 8d 31  |.<.......M/....1|
00000070  00 00 00 00 32 03 00 00  ce 78 03 3b 00 00 8a 2d  |....2....x.;...-|
00000080  ab 4c 00 00 46 00 00 00  f4 cf be 08 01 00 ce 31  |.L..F..........1|
00000090  32 03 00 00 3c 00 00 00  80 37 19 83 00 00 66 31  |2...<....7....f1|
000000a0  f1 4c 00 00 64 02 00 00  ec 0e b4 2b 00 00 3e 38  |.L..d......+..>8|
000000b0  55 4f 00 00 86 08 00 00  35 e5 28 b9 00 00 bb 2d  |UO......5.(....-|
000000c0  db 57 00 00 40 00 00 00  9d e7 39 7e 00 00 6e 31  |.W..@.....9~..n1|
000000d0  1b 58 00 00 47 02 00 00  87 bb 49 a2 01 00 8d 31  |.X..G.....I....1|
000000e0  6e 03 00 00 9c 02 00 00  1d 6c 9a 2e 01 00 ce 31  |n........l.....1|
000000f0  0a 06 00 00 38 00 00 00
```

We have a 128 bits pattern repeating starting at `0x048`:

```
88 7a 27 38 00 00 fe 33 00 00 00 00 f7 3c 00 00
34 b4 d4 7d 00 00 f2 38 f7 3c 00 00 b4 0f 00 00
db 4d 2f 09 01 00 8d 31 00 00 00 00 32 03 00 00
```

We have
* 32 bits, every bit used, most likely an id (as float, values are all over the place)
* 16 bits, either 0 or 1, 2, most likely a type/enum.
* 16 bits, all bits used, most likely an id
* 32 bits, some kind of counter
* 32 bits, some kind of counter
