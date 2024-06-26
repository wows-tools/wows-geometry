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
| MA | MA | MA | MA || MA | MA | MA | MA || MA | MA | MA | MA || MA | MA | MA | MA |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<--- n_ver_type -->||<--- n_ind_type -->||<--- n_ver_bloc -->||<--- n_ind_bloc -->|
|     32 bits       ||     32 bits       ||     32 bits       ||     32 bits       |
+====+====+====+====++====+====+====+====+
| MA | MA | MA | MA || MA | MA | MA | MA |
+====+====+====+====++====+====+====+====+
|<--- n_col_bloc -->||<--- n_arm_bloc -->|
|     32 bits       ||     32 bits       |
```

#### Field descriptions

| Field                | Size    | Description                                       |
|----------------------|---------|---------------------------------------------------|
| `n_ver_type`         | 32 bits | Number of vertex types                            |
| `n_ind_type`         | 32 bits | Number of index types                             |
| `n_ver_bloc`         | 32 bits | Number of vertex blocs                            |
| `n_ind_bloc`         | 32 bits | Number of index blocs                             |
| `n_col_bloc`         | 32 bits | Number of collision blocs                         |
| `n_arm_bloc`         | 32 bits | Number of armor blocs                             |

