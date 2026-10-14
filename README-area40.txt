Zone 40 - Thornwood Vale (village of Thornwick), 1024x1024
=============================================================
Files (copy the whole folder zones\40 into C:\Astonia35\zones\40, replacing the 256 version):
  thornwood.map  1024x1024 map (~36 MB)
  thornwood.chr  7 villagers + 16 monster templates (levels 8-14)
  thornwood.itm  2 door templates (z40_door_x / z40_door_y)

To run area 40:
  1. Run area40.sql once against the game database (adds the area row).
  2. Add this line to C:\Astonia35\start.bat next to the other areas:
       call :area 40 "Thornwood Vale"
  3. No runtime\40 DLL is needed (generic drivers only).
     server35.exe must be rebuilt for /goto thorn (src\command.c).
  4. In game, as a god: /goto thorn   or   /goto 516 512 40   (village plaza)

Editor: open it with the 1024 build of AstoniaMapStudio.
Note: there is no exit teleport out of zone 40 yet.
The previous 256x256 version is kept in 64bit-port\zone40-256 (not in zones, so it is not loaded).
