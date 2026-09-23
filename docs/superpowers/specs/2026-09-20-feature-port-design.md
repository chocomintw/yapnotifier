# YapNotifier 0.2

Ports the roster, state indicators, notifications, chat feed and customisation
depth of the MIT-licensed ReShade addon into YapNotifier's own architecture
(`.asi` with a standalone DirectComposition window + TS3 plugin over UDP).
The 2026-09-19 spec's hook/eject sections are obsolete; its wire-format and
config sections are superseded by this document.

## Wire protocol v2 (`shared/yap_protocol.h`)

One UDP datagram = complete state of the active TeamSpeak tab. Sent on every
change and as a 1 s heartbeat; 3 s of silence = plugin gone. Tab-separated
lines; **the last field of a line is the remainder of the line**. The plugin
replaces `\t \n \r` inside free text with spaces.

```
YAP2\n
S\t<conn>\t<server_name>\n                       conn: 0 off, 1 connecting, 2 connected
C\t<channel_id>\t<parent_name>\t<channel_name>\n  only while connected
U\t<clid>\t<flags>\t<uid>\t<contact_nick>\t<nick>\n   every client in the own channel
E\t<kind>\t...\n                                  one-shot events, never in heartbeats
```

`flags` bitmask: `Self, Talking, Whisper, InputMuted, OutputMuted, Away,
Recording, Commander, Priority, LocallyMuted, HardwareOff, Suppressed, Friend,
Blocked`. Events: `join <from> <name>`, `leave <to> <name>`,
`switch <count> <prev> <channel>`, `conn <server> <status>`,
`whisper <from_channel> <name>`, `chat <category> <sender> <channel> <text>`.
Limits: 256 users per datagram, chat text 200 chars. `YAP1` is still accepted
(talk-only, `legacy` flag → "plugin outdated" line in the HUD).

Privacy: chat crosses loopback to the same user's game process regardless of
what the overlay shows; the overlay filters by category (private off by
default). A reverse subscribe datagram would move the filter to the source if
ever needed.

## TS3 plugin

Mirrors the active tab only. Every roster-affecting SDK callback does a full
`resync_locked()` (channel client list + per-client properties) and sends.
Talk/whisper state is kept in sets. Move callbacks derive join/leave/switch
events with channel names; kick/ban/timeout of the own client become `conn`
events. Text messages and pokes become `chat` events (never consumed). Friends
are read from `<config>/settings.db` with the reference's read-only SQLite
reader (`ts3plugin/sqlite_read.cpp`, `ts_contacts.cpp`, MIT).

## `.asi`

- `teamspeak.cpp`: parser, snapshot publication, bounded event queue.
- `roster.cpp`: visibility (`show_local_user`, `only_show_talking`,
  `show_muted_users`), sort (channel order / alphabetical / speaking first, self
  always first), per-user styling with the reference's priority order
  (away → suppressed → locally muted → mic muted → speaker muted → speaking →
  whispering; commander/priority icons lead, others trail), speaking envelope.
- `notify.cpp`: `{placeholder}` templates, toast queue (fade in / hold / out,
  duplicate merge, quiet window after connect, cap), chat log filtering.
- `hud.cpp`: title, roster, toasts, chat feed into the background draw list,
  9-point anchoring, text shadow/outline, idle fade, demo mode.
- `menu.cpp`: tabbed editor for everything above plus per-user
  (`[user:<uid>]`) and per-channel (`[channel:<id>]`) overrides and profiles
  (copies of the INI in `YapNotifier.profiles/`).
- `config.cpp`: one `visit()` field list drives load/save; colours `#RRGGBBAA`.

## Not ported

Notification sounds, list-reorder animation, glow/border-sweep speaking modes,
avatar initials, per-executable profile mapping, font weight synthesis, name
overflow modes other than ellipsis, diagnostics counters: dead or N/A in the
reference.
