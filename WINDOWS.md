# Astonia 3.5 server on Windows Server 2019

The server builds as a native 64-bit Windows program with MinGW-w64. It uses
the same database and the same client protocol as the 64-bit Linux build, so
both can share one database.

## What you get

| File | Purpose |
|---|---|
| `server35.exe` | the area server, one process per area |
| `chatserver.exe` | relays chat between areas, start it first |
| `create_account.exe` | `create_account.exe <email> <password>` |
| `create_character.exe` | `create_character.exe <accountID> <name> <MW/FM/MWG...>` |
| `runtime\<area>\*.dll` | the zone drivers (NPC and item scripts) |
| `start.bat` / `stop.bat` | start or stop the chat server and all areas |

No extra DLLs are needed. MariaDB Connector/C, zlib and argon2 are linked into
the executables.

You still need the `zones` folder (copy your own `zones` folder next to
`server35.exe`) and a `motd.txt`.

## Setting up the database

1. Install MariaDB Server for Windows (the 10.11 or 11.4 LTS MSI from
   mariadb.org). Set the root password to the one in `install\MYSQLPASSWD`,
   or pick your own and pass it to every program with `-s dbpass=<password>`.
   Leave "Enable access from remote machines" off.
2. Open "Command Prompt (MariaDB)" in the server folder and create the tables:

   ```
   mysql -uroot -p < install\create_tables.sql
   mysql -uroot -p merc35 < install\merc.sql
   ```

3. Create the message of the day: `echo Welcome to Astonia > motd.txt`

The programs connect to `localhost` over TCP port 3306. Use `-s dbhost=...`,
`-s dbuser=...`, `-s dbpass=...`, `-s dbname=...` or a config file
(`-f <file>`) to change that. These options work on every program.

## First start

```
server35.exe -a 1
```

Look for errors, then press Ctrl+C. The server saves everybody and prints
"Clean shutdown". Then:

```
create_account.exe you@example.com yourpassword
create_character.exe 1 Ishtar MWG
start.bat
```

Connect with the client: `bin\moac -uIshtar -p<password> -d<server IP> -v35`

## Starting and stopping

* `start.bat` starts the chat server and every area, each in its own
  minimized window. The window title shows the area, players online and load.
* `stop.bat` creates `stop.flag`. Every area notices it within about a second,
  saves all players and exits. The script waits for that, then stops the chat
  server. `start.bat` removes a leftover `stop.flag`.
* Ctrl+C in an area window also saves and exits cleanly.
* Avoid closing an area window with the X button. Windows only gives the
  program about 5 seconds before killing it, which may not be enough to save
  everybody on a busy area.

To start the server when Windows boots, create a Task Scheduler task with the
trigger "At startup", the action `start.bat`, "Start in" set to the server
folder, and "Run whether user is logged on or not".

## Firewall

Allow the game ports and keep the database and chat server private
(PowerShell as Administrator):

```
New-NetFirewallRule -DisplayName "Astonia 3.5 areas" -Direction Inbound -Protocol TCP -LocalPort 27584-27777 -Action Allow
```

Ports 3306 (MariaDB) and 5554 (chat server) need no rule; Windows blocks them
from outside by default.

## Differences from Linux

* `-d` (run in the background) is ignored. Use `start.bat` or Task Scheduler.
* Windows locks a loaded DLL, so you cannot overwrite a zone driver while its
  area is running. Rename the old DLL first, then copy the new one in; the
  server should then load the new one as it does on Linux (not yet tested on
  Windows). Stopping the area first always works.
* Auto-complaints are not e-mailed (there is no sendmail).
* Crash backtraces are not written to the log.
* The memory report at shutdown leaves out the malloc details.

## Building from source

`build-windows.sh` downloads and builds zlib, argon2 and MariaDB Connector/C
into `windeps\`, then builds the server. Run it once; later runs reuse
`windeps\` and only rebuild the server.

On Windows, install MSYS2 from msys2.org, open the "MSYS2 MINGW64" shell, go
to the server folder (`cd /f/path/to/server`) and run:

```
pacman -S --needed git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
./build-windows.sh
```

From Linux with the MinGW-w64 cross compiler:

```
CROSS=x86_64-w64-mingw32 ./build-windows.sh
```

MariaDB Connector/C 3.4 and newer refuse a MariaDB server without a TLS
certificate by default ("SSL is required, but the server does not support
it"). The server turns that check off when it connects (`src/dbconnect.h`), so
a Connector/C you did not build yourself works too.

The Linux `Makefile` and `Makefile.win` both write to `runtime\`, so run
`make -f Makefile.win clean` before switching between them in one folder.

## How the port works

* `src/netcompat.h` maps the few socket calls that differ (close, ioctl,
  errno) to Winsock, and `net_startup()` calls `WSAStartup`.
* `src/wincompat.h` is force-included in the Windows build and supplies
  `bzero` and `kill`.
* `src/libload.c` loads zone drivers with `LoadLibrary`. `server35.exe`
  exports its functions and data and writes an import library that every zone
  DLL links against, which is what `-rdynamic` does on Linux.
* `select()` cannot be used as a sleep on Windows, so the tick timer uses
  `Sleep()` with a 1 ms timer resolution (`timeBeginPeriod(1)`).
* The listening socket uses `SO_EXCLUSIVEADDRUSE` instead of `SO_REUSEADDR`;
  on Windows `SO_REUSEADDR` would let two areas bind the same port.
* All files are opened in binary mode, as on Linux.
