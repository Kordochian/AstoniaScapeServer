# Building and running Astonia 3.5 on Windows

How to build the server and the client on Windows and run them on Windows.
The server runs on the machine at **10.1.0.36**; the client connects there by
default.

Everything below is done on Windows. Nothing needs Linux, Docker or WSL.

## What is where

| | Server | Client |
|---|---|---|
| Source | `F:\Claude_Astonia\64bit-port\astonia35_server_files` | `F:\Claude_Astonia\64bit-port\astonia_client\astonia_client_src.zip` |
| Ready-built | `server35.exe`, `chatserver.exe`, `create_*.exe`, `runtime\` in the same folder | `F:\Claude_Astonia\64bit-port\astonia_client\bin\moac.exe` + DLLs |
| Build shell | MSYS2 **MINGW64** | MSYS2 **CLANG64** |

The ready-built files were made from exactly this source, so you can skip the
build sections and go straight to "Running the server" if you only want to run
it.

## 1. Install MSYS2 (once, for both builds)

1. Download the installer from https://www.msys2.org and install to the
   default `C:\msys64`.
2. Start menu, "MSYS2 MSYS", then update it:

   ```
   pacman -Syu
   ```

   It may close the window when it is done. Open it again and run
   `pacman -Syu` a second time.

MSYS2 installs several shells. The server is built in **MSYS2 MINGW64**, the
client in **MSYS2 CLANG64**. In those shells your drives are `/c`, `/f` and so
on, so `F:\Claude_Astonia` is `/f/Claude_Astonia`.

## 2. Build the server

Open **MSYS2 MINGW64** and run:

```
pacman -S --needed git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
cd /f/Claude_Astonia/64bit-port/astonia35_server_files
sh build-windows.sh
```

The first run downloads and builds zlib 1.3.2, argon2 and MariaDB
Connector/C 3.4.9 into `windeps\`, then builds the server. Later runs reuse
`windeps\` and only rebuild the server, so after editing the source just run
`sh build-windows.sh` again.

Result, in the server folder:

* `server35.exe`, `chatserver.exe`, `create_account.exe`, `create_character.exe`
* `runtime\<area>\*.dll`, the zone drivers

The libraries are linked in, so the exes need no extra DLLs and can be copied
to another Windows machine as they are (together with `runtime\`).

If the build stops with "No rule to make target" or old objects, run
`make -f Makefile.win clean` and build again.

## 3. Build the client

1. Unzip `astonia_client_src.zip` somewhere, for example `F:\build\`. That
   gives `F:\build\astonia_client_src`.
2. Open **MSYS2 CLANG64** and install the tools (this is the client's own
   readme, unchanged):

   ```
   pacman -S --needed mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-libpng mingw-w64-clang-x86_64-libzip mingw-w64-clang-x86_64-mimalloc mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja make zip git mingw-w64-clang-x86_64-dwarfstack mingw-w64-clang-x86_64-zig mingw-w64-clang-x86_64-rustup
   rustup toolchain install stable-x86_64-pc-windows-gnullvm
   rustup default stable-x86_64-pc-windows-gnullvm
   ```

3. Build SDL3 once, then the client:

   ```
   cd /f/build/astonia_client_src
   make build-sdl3 build-sdl3-mixer
   make WARNING_FLAGS="-Wall -Wextra -Wno-unused-but-set-global"
   ```

   The `WARNING_FLAGS` part matters: the client builds with "warnings are
   errors" by default, and a newer clang than the client authors used adds a
   warning that stops the build.

4. Copy the DLLs the exe needs next to it:

   ```
   ldd bin/moac.exe | grep -i clang64 | awk '{print $3}' | xargs -I{} cp {} bin/
   ```

5. The `res` folder in the source has placeholder files only (the real ones
   are kept in Git LFS). Copy `res\` from the official client v1.1.3 release
   zip over it. `res\gx1.zip` alone is 174 MB.

The client is now `bin\moac.exe`. Copy the whole folder (`bin\`, `res\`,
`create_shortcut.bat`) to wherever players will run it.

### Changing the server address

The client goes to 10.1.0.36 as a 3.5 client when started without `-d` and
`-v`. That default is one line in `src\game\main.c`:

```c
#define DEFAULT_SERVER "10.1.0.36"
```

Change it and rebuild to point the client somewhere else. A single player can
also override it with `-d<address>`.

## 4. Running the server

On the server machine (the one with address 10.1.0.36).

### Database (once)

1. Install MariaDB Server for Windows, 10.11 or 11.4 LTS MSI from
   mariadb.org. Set the root password to the one in `install\MYSQLPASSWD`.
   Leave "Enable access from remote machines" off.
2. Open "Command Prompt (MariaDB)", go to the server folder, then:

   ```
   mysql -uroot -p < install\create_tables.sql
   mysql -uroot -p merc35 < install\merc.sql
   echo Welcome to Astonia > motd.txt
   ```

3. Copy your `zones` folder next to `server35.exe`.

### First start and accounts

```
server35.exe -a 1
```

Wait for "Server started", check for errors, then Ctrl+C. Then create an
account and a character, and start everything:

```
create_account.exe you@example.com yourpassword
create_character.exe 1 Ishtar MWG
start.bat
```

`stop.bat` saves everybody and stops all areas.

### Firewall

PowerShell as Administrator, on the server machine:

```
New-NetFirewallRule -DisplayName "Astonia 3.5 areas" -Direction Inbound -Protocol TCP -LocalPort 27584-27777 -Action Allow
```

That is the only rule needed. Each area listens on the first free port from
27584 up. The database (3306) and the chat server (5554) stay closed to the
network.

The server listens on every address of the machine, so it needs no setting for
10.1.0.36. That address only has to be the server machine's own IP (check with
`ipconfig`).

## 5. Running the client

```
bin\moac.exe -uIshtar -pyourpassword
```

`create_shortcut.bat` puts a shortcut to `moac.exe` on the desktop; add
`-u<name> -p<password>` to its target.

Wait about 10 seconds after `start.bat` before logging in. Right after start
the areas have not registered with the database yet, and a login that early
fails with "cannot rescue, area 1 and 3 are down too" in the server log.

When a player walks into another area the server hands the client a new port;
the client always uses the same address it logged in to. So every area must
run on the 10.1.0.36 machine, which `start.bat` does.

### Checking the connection from a player's PC

PowerShell:

```
Test-NetConnection 10.1.0.36 -Port 27584
```

`TcpTestSucceeded : True` means the client can reach the server. If it says
False: is `start.bat` running, is the firewall rule there, and can the PC
reach 10.1.0.36 at all (`ping 10.1.0.36`, VPN or LAN)?

## What was tested

* `build-windows.sh` from a clean copy of the source, with the MinGW-w64 cross
  compiler (the same script, `CROSS=x86_64-w64-mingw32`). The native MSYS2 run
  could not be tried here because the MSYS2 package server is blocked from this
  sandbox.
* The resulting `server35.exe` under Wine against MariaDB, with a client started
  with no `-d` or `-v`: it connected to 10.1.0.36:27584 and the character
  entered the game (`Character entered server ... IP=10.1.0.36` in the log).
* The client was cross-built with llvm-mingw (clang), the same compiler family
  as MSYS2 CLANG64. The MSYS2 client steps are the client's own readme plus the
  `WARNING_FLAGS` and DLL fixes above, not run natively.
* Not yet run on real Windows Server 2019.
