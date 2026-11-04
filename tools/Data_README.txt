Project Midas - Data folder
===========================

Put your own GoldenEye 007 (USA) cart image in this folder. The game also
keeps its saves, profiles, weapon sets, scenarios and controller settings
here.

Accepted cart images
--------------------

The file name and extension do not matter; the game checks the first bytes.

  Format   Byte order     First bytes
  .z64     big-endian     80 37 12 40
  .v64     byte-swapped   37 80 40 12
  .n64     word-swapped   40 12 37 80

The image must be the stock (U) cart: 12,582,912 bytes. A modded cart,
another region or a bad dump is rejected at startup, and the log says why.

Where the game looks, in order
------------------------------

  1. A file dropped onto ProjectMidas.exe
  2. ProjectMidas.exe --rom-file <path>
  3. The GE_ROM=<path> environment variable
  4. rom=<path> in the [data] section of midas.ini
     (relative paths start next to ProjectMidas.exe)
  5. The first usable file in this folder, alphabetically
  6. baserom.u.z64 next to the executable, or one folder up

The log shows which one was used: look for the line starting
"ge: romdata: ROM =". When the game is started by double-click, from Steam
or from a shell on Windows, the log is Logs/ge_direct.log in this folder.

Folders the game creates here
-----------------------------

  Saves/        eeprom.bin, the save file
  Profiles/     player profiles (.geprofile) and multiplayer stats (.gestats)
  WeaponSets/   custom multiplayer weapon sets (.gewset)
  MatchSettings/ saved multiplayer settings (.gemp)
  Devices/      per-device settings (.gedevice); kbm.gedevice holds the
                keyboard and mouse binds
  Logs/         ge_direct.log, the run log, and crash.log (Windows)

Shader lists
------------

  shaders_base.txt  ships with the game: the shaders every stage uses
  shaders.txt       written by the game: shaders this install has used

The game compiles the shaders in both lists while the menus are up, so
gameplay does not pause the first time it draws something new. Deleting
either file is safe; shaders.txt fills again as you play.
