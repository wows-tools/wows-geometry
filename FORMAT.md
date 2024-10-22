# WoWs .geometry Format.

## Introduction

TODO

## Format

### Header

```mermaid
%%{init: { 'theme': 'forest', 'config': {'bitsPerRow': 64, 'bitWidth': 15}}}%%
packet-beta
0-31: "n_vertex_type"
32-63: "n_index_type"
64-95: "n_vertex_bloc"
96-127: "n_index_bloc"
128-159: "n_collision_bloc"
160-191: "n_armor_bloc"
192-255: "offset_start"
256-319: "unknow_1"
320-383: "unknow_2"
384-447: "unknow_3"
448-511: "unknow_4"
512-575: "unknow_5"
```

| Field                      | Size    | Description                                                                                     |
|----------------------------|---------|-------------------------------------------------------------------------------------------------|
| `n_vertex_type`            | 32 bits | Number of vertex types                                                                          |
| `n_index_type`             | 32 bits | Number of index types                                                                           |
| `n_vertex_bloc`            | 32 bits | Number of vertex blocs                                                                          |
| `n_index_bloc`             | 32 bits | Number of index blocs                                                                           |
| `n_collision_bloc`         | 32 bits | Number of collision blocs                                                                       |
| `n_armor_bloc`             | 32 bits | Number of armor blocs                                                                           |
| `offset_1`                 | 64 bits | Offset to data start (always 72 bytes)                                                          |
| `unknown_1`                | 64 bits | Multiple of 8/64bits, value between ~80 and ~1200  | offset?                                    |
| `unknown_2`                | 64 bits | Multiple of 8/64bits, value between ~100 and ~2000 | offset?                                    |
| `unknown_3`                | 64 bits | Large value (few thousands to millions) | vertex count?                                         |
| `unknown_4`                | 64 bits | Large value (few thousands to millions), 0 if no collision bloc | vertex count collision block? |
| `unknown_5`                | 64 bits | Large value (few thousands to millions), 0 if no armor bloc     | vertex count armor block?     |

### Metadata

```mermaid
%%{init: { 'theme': 'forest', 'config': {'bitsPerRow': 64, 'bitWidth': 15}}}%%
packet-beta
0-31: "id_unk_6 (uint32_t)"
32-47: "type_unk_7 (uint16_t)"
48-63: "id_unk_8 (uint16_t)"
64-95: "n_unk_9 (uint32_t)"
96-127: "n_unk_10 (uint32_t)"
```

| Field           | Size      | Description                              |
|-----------------|-----------|------------------------------------------|
| `id_unk_6`      | 32 bits   | Unknown field, possibly an identifier    |
| `type_unk_7`    | 16 bits   | Unknown field, possibly a type value     |
| `id_unk_8`      | 16 bits   | Unknown field, possibly an identifier    |
| `n_unk_9`       | 32 bits   | Unknown field, related to count or index |
| `n_unk_10`      | 32 bits   | Unknown field, related to count or index |


### Metadata 2

```mermaid
%%{init: { 'theme': 'forest', 'config': {'bitsPerRow': 64, 'bitWidth': 15}}}%%
packet-beta
0-63: "off_ver_bloc_start (uint64_t)"
64-127: "n_size_type_str (uint64_t)"
128-191: "off_ver_bloc_end (uint64_t)"
192-223: "s_ver_bloc_size (uint32_t)"
224-255: "n_unk_5 (uint32_t)"
```

| Field                | Size      | Description                                                                         |
|----------------------|-----------|-------------------------------------------------------------------------------------|
| `off_ver_bloc_start` | 64 bits   | Offset to the corresponding vertex section relative to this current block           |
| `n_size_type_str`    | 64 bits   | Size of the vertex type string (e.g., len(set3/xyznuviiiwwtbpc))                    |
| `off_ver_bloc_end`   | 64 bits   | Offset to the end of the corresponding vertex block                                 |
| `s_ver_bloc_size`    | 32 bits   | Size of the vertex block                                                            |
| `n_unk_5`            | 32 bits   | Unknown field                                                                       |

### Metadata 3


