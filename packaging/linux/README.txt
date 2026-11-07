El Chavo Kart - PC port
Version @VERSION@

El Chavo Kart (Chaves Kart) for Xbox 360, statically recompiled to run natively
on Linux x86_64 with the ReXGlue SDK. This is a native recompilation with Vulkan,
not an emulator, and it does not use Wine or Proton.

Linux support is experimental: this release has not been validated on real Linux
PCs or Steam Deck hardware. Reports are welcome.

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
- 64-bit x86 Linux with glibc 2.31 or newer (Ubuntu 20.04 or later)
- A Vulkan driver, and an X11 or Wayland desktop
- xdg-desktop-portal, zenity or kdialog for the launcher's file picker
- For online play, the system's OpenSSL (libssl.so.3 or libssl.so.1.1)
- Your own disc image, plus about 2 GB of free disk space next to the AppImage
  for the game files


INSTALL
-------
1. Put the AppImage in a folder you can write to.
2. Make it executable: chmod +x ElChavoKart-v@VERSION@-x86_64.AppImage
3. Run it. The launcher opens.
4. On the Play page, click "Install from disc image..." and select your disc
   image.
5. When the Play page says "Ready to play", press PLAY.

To repair the game files, use "Reinstall from disc image..." on the Play page.
Your settings and saves are kept.

To start the game directly, turn "Show this launcher" off on the Play page.
Hold Shift while starting the program to bring the launcher back.

The AppImage starts without FUSE: where it cannot mount itself it unpacks into a
private folder under /tmp and removes it afterwards.
APPIMAGE_EXTRACT_AND_RUN=1 forces this.


WHERE YOUR FILES ARE
--------------------
Next to the AppImage:

  game/                  the game files the launcher installed
  user_data/             saves, achievements and settings.toml
  logs/                  session and crash logs

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
Online play is off by default, and Windows and Linux players play together.

1. On the launcher's Online page, turn Online play on and choose an online
   name.
2. Run the Connection test. Play waits until one test has finished, whatever
   its result.
3. Share Your friend code. Add a friend's code under Friends; they accept it
   under Friend requests.
4. In the game, go to Free Race and pick a competition and difficulty. On the
   character screen, X invites a friend, up to 4 players in all.


STEAM AND STEAM DECK
--------------------
In Desktop Mode, use Add a Non-Steam Game and select the AppImage. Do not set a
compatibility tool: this is a native Linux program and Proton is not used.


REPORTING A BUG
---------------
On the launcher's About page, "Report a bug" shows the newest log file and
opens the project's issue page. Nothing is uploaded: check the log, then attach
it to your report yourself.


UPDATES
-------
The launcher checks the project's GitHub releases once per start. When a newer
release exists, an update banner appears in the header; nothing is installed
until you choose UPDATE NOW and then RESTART TO UPDATE. The update replaces only
the AppImage you started. The "game" folder, "user_data", saves, settings and
logs are kept.
