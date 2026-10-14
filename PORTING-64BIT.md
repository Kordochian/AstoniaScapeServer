# Astonia 3.5 server: 64-bit port

This is George's server (`Downloads\astonia_community_server35`). It is upstream commit `803b4cd`
plus his local changes, ported so that it builds and runs as a 64-bit program.

- `64bit-port-only.diff` shows only the 64-bit changes, on top of George's code.
- `64bit-port-full.diff` shows George's changes and the 64-bit changes together, against upstream `803b4cd`.

The zones folder is not included because the port doesn't change it. Copy your existing
`zones` folder next to `src`.

## Building (Linux, for now)

```
sudo apt install build-essential libmariadb-dev-compat libargon2-dev zlib1g-dev mariadb-server
make -j4            # 64-bit (default)
make ARCH=-m32      # the old 32-bit build still works
```

You no longer need the i386 packages (`gcc-multilib`, `:i386` libraries) for the default build.
The Windows Server 2019 build is step 3 of the plan.

## Your existing database works unchanged

Characters, inventories, depots and lab progress are stored in the database as raw C structs.
The port keeps their byte layout identical to the 32-bit server:

| record | stored size | how |
|---|---|---|
| character (`chars.chr`) | 1028 bytes | `char_to_disk()` / `char_from_disk()` in `database.c` convert on save/load |
| item (inventory, depot) | 252 bytes | `struct item` uses 4-byte packing; its free-list pointer became an index |
| `lab_ppd` | 196 bytes | 4-byte packing |
| `depot_ppd` | 30304 bytes | follows from `struct item` |

Compile-time checks (`_Static_assert`) stop the build if someone changes `struct character`
without updating the disk layout.

A 32-bit and a 64-bit server can share one database.

## What changed

- **Saved data layout** (above): `server.h`, `lab.h`, `create.c`, `database.c`, `database.h`, `task.c`.
- **Pointers passed through ints**: `struct msg` `dat1..dat3`, `notify_*()`, `char_driver()` and every
  DLL's `driver()` `ret` argument are now `intptr_t`. They carry text pointers (`NT_TEXT`, strategy parser).
- **64-bit bugs that only showed when running**:
  - `localtime((time_t *)&int_var)` read 8 bytes from a 4-byte int, which garbled every log
    timestamp. Fixed in `log.c` and `database.c`.
  - `pthread_join(..., (void **)&int_var)` wrote 8 bytes into an int, which crashed the server
    on shutdown. Fixed in `database.c`.
  - `chat.c` read an IPv4 address as `unsigned long` (8 bytes).
  - `create.c` / `server.c` did pointer math through `unsigned int`.
- **Build**: `rdtsc.S` (32-bit assembly) was replaced by `rdtsc.c` (`__rdtsc()`), missing `stdlib.h`
  includes were added, `size_t` printf formats were fixed, `mysql/errmsg.h` was included, and the
  Makefile now has an `ARCH` variable.
- **George's code**: the `/rescue` command in `command.c` is wrapped in `#if 0` because
  `cmd_rescue()` isn't defined anywhere. `minoa.c` is still not in the Makefile.

## How it was tested

1. It builds with `-Wall -Werror` as 64-bit.
2. A 32-bit build wrote a character record with every byte set. The 64-bit `char_from_disk()`
   read back all 73 non-pointer fields identically, and `char_to_disk()` reproduced the record
   byte for byte.
3. A 64-bit server (area 1) ran on MariaDB 10.11. A test account logged in through the real
   login handshake, which created a new character. The character was saved (`chr` = 1028 bytes)
   and logged in again, which loaded it. A 32-bit program read that saved record correctly
   (name, level, hp and positions).
4. AddressSanitizer run: no errors. Shutdown by SIGINT or SIGTERM is clean.
