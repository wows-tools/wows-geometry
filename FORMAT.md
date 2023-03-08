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
+====+====+====+====+
| MA | MA | MA | MA |
+====+====+====+====+
|<----- n_entry --->|
|     32 bits       |
```

#### Field descriptions

| Field                | Size    | Description                                       |
|----------------------|---------|---------------------------------------------------|
| `n_entry`            | 32 bits | Number of box entries in the file                 |


### Entries

#### Layout
```
+====+====+====+====++~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~+
| SN | SN | SN | SN ||                           entry name                        |
+====+====+====+====++~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~+
|<--- size_name --->|<--------------------------- name ----------------------------|
|      32 bits      |                                                              |

+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
| MA | MA | MA | MA || MA | MA | MA | MA || MA | MA | MA | MA || MA | MA | MA | MA |
+====+====+====+====++====+====+====+====++====+====+====+====++====+====+====+====+
|<----- dim_1 ----->||<------ dim_2 ---->||<------ dim_3 ---->||<------ dim_4 ---->|
|     32 bits       ||      32 bits      ||       32 bits     ||       32 bits     |

+====+====+====+====++====+====+====+====+
| MA | MA | MA | MA || MA | MA | MA | MA |
+====+====+====+====++====+====+====+====+
|<------ dim_5 ---->||<------ dim_6 ---->|
|       32 bits     ||       32 bits     |

[...repeat n_entry times...]
```

#### Field descriptions

| Field                | Size    | Description                                                         |
|----------------------|---------|---------------------------------------------------------------------|
| `size_name`          | 32 bits | strlen of the char string `name`                                    |
| `name`               | N/A     | `char[size_name]` array containing the name (/!\ no `\0` at the end |
| `dim_X`              | 32 bits | Dimension of the box (x y z dx dy dz), order & float vs int unknown |


