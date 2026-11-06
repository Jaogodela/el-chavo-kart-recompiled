El Chavo Kart - PC port
Version @VERSION@

El Chavo Kart (Chaves Kart) for Xbox 360, statically recompiled to run natively
on Windows with the ReXGlue SDK. This is a native recompilation, not an
emulator.

Source code and issue tracker:
https://github.com/saulob/el-chavo-kart-recompiled


NO GAME FILES ARE INCLUDED
--------------------------
You need your own copy of the game: the El Chavo Kart (USA) Xbox 360 disc image
(.iso).

  Title ID:      475807D7
  Media ID:      5C2ECDD6
  Game version:  0.0.0.3


REQUIREMENTS
------------
- Windows 10 or 11, 64-bit
- A Direct3D 12 capable graphics card
- The latest Microsoft Visual C++ Redistributable 2015-2022 (x64). Most PCs
  have it. If Windows reports a missing VCRUNTIME140.dll or MSVCP140.dll,
  install it from https://aka.ms/vs/17/release/vc_redist.x64.exe
- Your own disc image, plus about 2 GB of free disk space next to the program
  for the game files


INSTALL
-------
1. Extract the ZIP anywhere you can write to (not "Program Files").
2. Run el_chavo_kart.exe. The launcher opens.
3. On the Play page, click "Install from disc image..." and select your disc
   image.
4. Wait while the launcher checks the image and copies the game files into the
   "game" folder next to the program.
5. When the Play page says "Ready to play", press PLAY.

To repair the game files, use "Reinstall from disc image..." on the Play page.
Your settings and saves are kept.

To start the game directly, turn "Show this launcher" off on the Play page.
Hold Shift while starting the program to bring the launcher back.


WHERE YOUR FILES ARE
--------------------
Next to the program:

  game\                  the game files the launcher installed
  user_data\             saves, achievements and settings.toml
  logs\                  session and crash logs

The launcher's About page opens these folders.


HOTKEYS
-------
  F2      Frame counter
  F3      Online Ping display (needs Online play)
  F7      Achievements
  F11     Windowed / Borderless Fullscreen
  Alt+F4  Exit


ONLINE PLAY
-----------
Online play is off by default.

1. On the launcher's Online page, turn Online play on and choose an online
   name.
2. Run the Connection test. Play waits until one test has finished, whatever
   its result.
3. Share Your friend code. Add a friend's code under Friends; they accept it
   under Friend requests.
4. In the game, go to Free Race and pick a competition and difficulty. On the
   character screen, X invites a friend, up to 4 players in all.

The race runs directly between the host and each friend over UDP; a friend the
host cannot reach is relayed through the online service. The host simulates the
race; items, laps, placement and the finish are not synchronized yet.


REPORTING A BUG
---------------
On the launcher's About page, "Report a bug" shows the newest log file and
opens the project's issue page. Nothing is uploaded: check the log, then attach
it to your report yourself.


UPDATES
-------
The launcher checks the project's GitHub releases once per start. When a newer
release exists, an update banner appears in the header; nothing is installed
until you choose UPDATE NOW and then RESTART TO UPDATE. The update replaces
only the port's own program files. The "game" folder, "user_data", saves,
settings and logs are kept.


WINDOWS SMARTSCREEN
-------------------
The first time you run el_chavo_kart.exe, Windows SmartScreen may show a
warning, because the program is not digitally signed. Choose "More info", then
"Run anyway".
