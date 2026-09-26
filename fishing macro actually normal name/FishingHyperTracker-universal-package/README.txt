FISHING HYPER TRACKER
=====================

Windows 10/11. Works on any monitor, resolution, or display scaling - the
fishing bar, exit-button, and cast-point locations are all calibrated live
on your own screen (see CALIBRATION TAB below) rather than assumed from any
fixed reference resolution. If the macro worked for one person but not
another, this is why: it used to assume everyone's screen matched a single
hardcoded layout. Nothing is hardcoded to a specific resolution anymore.

FIRST-TIME SETUP
-----------------
Before pressing Start, switch to the Calibration tab and set up all three:
  1. Calibrate Cast Point  - click the button, then click the exact spot
                              in-game you'd click to cast your line.
  2. Calibrate Fishing Bar - click the button, then click-and-drag a box
                              tightly around the vertical bar/reel-in
                              minigame area (the region containing the
                              moving green target zone and white player
                              block).
  3. Calibrate Exit Button - click the button, then click-and-drag a box
                              tightly around the red "Exit"/cancel button
                              that appears during the minigame.
Esc cancels any calibration in progress. Each one is saved to
FishingHyperTracker.ini immediately, so you only need to do this once per
computer/game window layout - it's remembered on the next launch. Until
all three are set, the Fishing tab shows "NOT CALIBRATED" and pressing
Start won't do anything useful.

If the game's window moves, resizes, or you change display resolution,
just recalibrate whichever regions moved - it can be done live, even
while the tracker is running.

RUNNING IT
----------
Once calibrated, press F6 (or click Start) to run the full cycle, or F8
for a safe exit. Both are rebindable - see the Keystrokes tab. The HUD is
always on top and can be dragged by its title area.

COLOR DETECTION
----------------
Within the calibrated regions, it recognizes color ranges around target
green RGB(60,200,80), player white RGB(250,250,250), and exit red
RGB(190,30,30) - not exact matches, since lighting, transparency,
anti-aliasing and screenshots can all shift the displayed pixels. It also
recognizes the yellow-green "warning" color the target zone turns when the
player block has been outside it too long (a bright gold border around
RGB(215,196,5) and a dimmer translucent olive fill around RGB(62,68,28) -
detected by hue rather than exact color, since this state can render at
very different brightness/opacity), so tracking doesn't lose the zone and
let the block fall to the bottom when that happens.

FULL CYCLE
----------
Once started, the tracker runs the whole loop on its own:
  1. CASTING      - guides the cursor to the calibrated cast point, dwells
                     there briefly so the game registers the hover/focus,
                     fires a firm click, then stays put there a while
                     longer before moving the cursor back (yanking it away
                     right after mouse-up was itself causing missed
                     clicks) to start the next fishing minigame.
  2. WAIT FOR FISH - a configurable safety delay (see WAIT FOR FISH
                     SETTING below) before handing control to the reel
                     logic - but the calibrated exit button is watched the
                     whole time, and as soon as it appears the wait ends
                     immediately rather than adding needless delay. If the
                     exit button never appears at all before the delay
                     runs out, the cast is assumed to have not registered
                     and is retried automatically.
  3. HOOKING       - the bar-catching control described below runs until
                     the exit button (which appeared during hooking)
                     reliably disappears again (debounced across several
                     frames so a single flickered/misread frame doesn't
                     end it early).
  4. COLLECTING    - a fixed 4-second pause after hooking ends ("Waiting
                     to collect" on the HUD).
  5. HOLDING T     - holds the T key down for 4 seconds to claim the
                     catch, then loops back to CASTING.
The Fishing tab's second line shows the current phase, with a countdown
for the timed phases.

POST-CAST FAILSAFE (10s)
--------------------------
Once a cast click is actually confirmed successful (the exit button has
appeared, meaning the reel is really out in the water), the tracker will
not fire another cast click for 10 seconds, no matter what brings the
phase machine back around to CASTING in that window. This guards against
a click landing more than once and the minigame appearing to start, end,
and restart in a rapid little loop. While waiting out this cooldown, the
Fishing tab shows "Post-cast failsafe: Xs left before next click". This
never adds delay before the very first cast of a run.

WAIT FOR FISH SETTING
-----------------------
The "Wait for fish" field (Fishing tab, in seconds, range 0-60) is a
minimum safety delay between the collect/hold-key phase finishing and the
next cast click - it prevents the next click from firing immediately after
the reel is returned. The calibrated exit button is still watched during
this delay and ends it early the moment the minigame is actually ready, so
this setting only ever adds a floor, never unnecessary extra waiting.

TABS
----
The window has three tabs, selected at the top:
  Fishing     - the phase readout, detection stats, cast point, wait-timer
                setting, and the Start/Exit buttons. A small fish icon in
                the top right (green = live, red = off) shows run state.
  Keystrokes  - shows the Start/Pause and Exit hotkeys and buttons to
                rebind them. The T key held during the Holding phase is
                fixed and not rebindable.
  Calibration - the three calibration buttons and current status of each
                (coordinates/size, or "NOT SET").

SETTINGS PERSIST ACROSS RESTARTS
-----------------------------------
Saved to FishingHyperTracker.ini next to the exe (created automatically):
rebound Start/Pause and Exit hotkeys, the Wait for Fish delay, and all
three calibrated regions (cast point, fishing bar, exit button). Nothing
needs to be redone on the next launch unless the game's layout changes.

WORKING WHEN THE GAME ISN'T FOCUSED
--------------------------------------
Both the Cast click and the held key force the calibrated game window to
the foreground for a moment before firing, then (after the key is
released) hand focus back to whichever window you were using before that
cycle started. This means you can be doing something else in another
window most of the time; the tracker briefly steals focus only for the
cast click and for the few seconds it's holding the key. The held key is
also injected as a scan code (not just a virtual-key code), since many
games read keyboard state via DirectInput/raw input and can otherwise miss
a plain SendInput keypress entirely.

CONTROL BEHAVIOR (during the Hooking phase)
--------------------------------------------
* Below the moving green zone: holds left mouse to rise.
* Above the moving green zone: releases to fall.
* Inside the zone: uses rapid 18 ms adaptive micro-pulses to float near center.
* Predicts target/player momentum to brake before overshoot.
* Forces recovery away from the top or bottom and inserts a tiny safety release
  after any unusually long hold.
* Releases the mouse immediately when paused, when the exit button
  disappears, or when the program closes; also releases the held key on
  pause/close.

IMPORTANT
---------
Some games prohibit automation. Use this only where permitted; account
penalties are possible if the game treats macros as cheating.

The HUD window is slightly translucent (whole-window alpha, so the text
and buttons are a bit see-through too, not just the background panel).

The HUD repaints at most ~11 times/sec (throttled independently from the
underlying 250-330 scans/sec control loop, which is unaffected) and draws
to an off-screen buffer before blitting it to the window in one go - both
were needed to stop the buttons from visibly flickering.

SOURCE / REBUILD
----------------
main.cpp, fish_icons.h (embedded PNG bytes for the status icon - generated
from icon_on.png/icon_off.png, included for reference/regeneration only,
not needed to build) are included. build.bat supports either Visual
Studio's Developer Command Prompt or an MSYS2 MinGW-w64 g++ installation.
Links comctl32 (tab control), gdiplus and ole32 (for decoding the embedded
status-icon PNGs) in addition to user32/gdi32/dwmapi/winmm.
