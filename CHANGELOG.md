# Changelog

Each release gets a `## <version>` section; CI copies the one matching `shared/version.h` into the GitHub Release notes. Add entries under `Unreleased` as you go, then rename it to the version when you bump.

## 0.4.0 - 2026-09-23

- HOME hides or shows the overlay; also a checkbox on the Status tab. Saved with the rest of the settings.
- New Keys tab in the menu: click a hotkey and press the new key to rebind it (menu and hide overlay). No INI editing or relaunch needed.
- After an update the menu's Status tab shows what changed in the new version until you press "Got it".
- Smaller download: the embedded font is trimmed to the characters the overlay can draw (`.asi` 4.2 MB to 3.6 MB). Text looks the same.
- Menu: colour pickers line up with their labels, and slider handles sit centred on the track instead of riding up into the row above.

## 0.3.0 - 2026-09-22

- Drag HUD blocks (roster, notifications, chat) to reposition them while the menu is open; the Layout sliders follow.
- Menu restyled after DESIGN.md: edit cards and a coloured checkbox tick.

## 0.2.1 - 2026-09-21

- The menu has its own UI context, so the Scale slider only resizes the HUD.

## 0.2.0 - 2026-09-21

First public release.

- Overlay drawn in its own transparent window; no game hooks.
- UI rebuilt on RmlUi with the Inter font.
- Roster with talk, mute and away states; join/leave/whisper notifications; chat feed.
- TS3 plugin sends the channel roster over localhost UDP (protocol v2).
- Self-updater: installs the latest GitHub release after verifying its SHA-256.
