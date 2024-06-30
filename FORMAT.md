# WoWs .geometry Format.

## Introduction

TODO

## Convention

A **byte/8 bits** is represented as follows:
```
+====+
| XX |
+====+
```

A **variable length field** (ex: strings) is represented as follows:

```
|~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
|           Field            |
|~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
```

The boundary between two fields is marked as follows:

```
...=++=...
    ||
...=++=...
```

## Format

### Header

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

#### Field descriptions

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

