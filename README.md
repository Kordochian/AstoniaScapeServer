# AstoniaScape server

RuneScape-inspired rebuild of the Astonia 3.5 community server (upstream `803b4cd`),
ported to 64-bit and native Windows, with a use-based skill engine, skilling (mining,
smithing, woodcutting, crafting, farming), skill quests and daily tasks.

## Layout
- `src/` server, tools and zone drivers (zone drivers build to `runtime/<area>/*.dll`)
- `zones/` zone data: `.map`, `.chr`, `.itm`, `.place`, and `zones/generic/*.cfg` (skills, skilling, quests)
- `install/` database schema, `accman/` account website, `docker/`, `docs/`
- `history/diffs/` the step-by-step diffs from before this repo existed
- `tools/` generator scripts for item data

## Building
- Linux: `make` (needs libmariadb-dev, libargon2-dev, zlib). Builds `server35`, tools and all zone DLLs with `-Werror`.
- Windows: `make -f Makefile.win` with MinGW-w64; see `WINDOWS-BUILD.md` and `WINDOWS.md`.

## Deploying to C:\Astonia35
- Any change in `src/` outside a zone driver: copy `server35.exe`.
- Zone driver change, or any change to `struct player` / `struct character`: copy `runtime\` too
  (server35.exe and the zone DLLs must match).
- Data only (`zones\...`): stop.bat, copy the changed files, start.bat. No rebuild.

The passwords in `src/config.c`, `install/MYSQLPASSWD` and `accman/config.php` are the upstream
development defaults. Real installs use the random passwords the installer writes to `astonia.cfg`.

Upstream readme: `docs/upstream-readme.md`. License: `license.txt`.
