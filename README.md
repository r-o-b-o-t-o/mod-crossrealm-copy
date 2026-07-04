# mod-crossrealm-copy

An [AzerothCore](https://www.azerothcore.org) module that lets players copy one of their
characters from another realm onto the realm they are currently playing.

## How it works for players

1. Create (or pick) a character on this realm with the **same race and class** as the
   character you want to copy. By default it must be a fresh character
   (level 1, see `CrossRealmCopy.MaxTargetLevel`).
2. Log in on that character and type:

   ```
   .character copy Name
   ```

   where `Name` is the name of your character on the source realm.
3. Wait for your turn (copies run one at a time; the queue position is announced in the
   chat). When the copy starts you are warned, then disconnected a few seconds later.
4. **Stay logged out** for a few seconds. The character is temporarily
   banned during the copy so it cannot be played while its data is rewritten; the ban is
   lifted automatically the moment the copy finishes.

All data of the target character is **wiped** and replaced by the source character's
data. The target keeps its own name, account and GUID.

## What gets copied

Character sheet (level, xp, money, position, appearance, titles, taxi nodes, ...),
inventory/bank/equipment (items get fresh GUIDs on this realm), equipment sets, wrapped
gifts, refundable purchases, mailbox (including attachments), pets (spells, auras and
cooldowns included), spells, talents (both specs), glyphs, action bars, skills,
reputations, achievements (including progress), quest status (in-progress, rewarded and
daily/weekly/monthly/seasonal), auras, spell cooldowns, homebind, arena matchmaker
rating, per-character client settings and addon data.

**Not copied** (realm-specific or unsafe to transfer): guild and arena team membership,
friend/ignore lists, instance lockouts, auctions and auction-held items, active
petitions, corpse location, calendar events, declined names.

The copy is schema-tolerant: only columns that exist on **both** realms are transferred,
so custom columns on either side do not break the copy.

## mod-transmog integration (optional)

When [mod-transmog](https://github.com/azerothcore/mod-transmog) tables exist, the copy
also transfers:

- `custom_transmogrification` — active transmogs, following the copied items (remapped
  to their new item GUIDs).
- `custom_transmogrification_sets` — outfit presets, wiped and replaced like the rest
  of the character data.
- `custom_unlocked_appearances` — the source account's appearance collection is
  **merged** into the target account's existing collection (account-wide data is never
  wiped).

The integration is fully optional: realms without mod-transmog simply skip these
tables. When both modules are compiled statically into the same worldserver, the merged
appearances are also pushed into mod-transmog's in-memory collection cache right after
the copy, so they are usable immediately; otherwise they show up after a server restart
or a `.transmog reload`.

## Requirements & safety design

- Works with the `acore_characters` database of any realm running the same (or a close)
  AzerothCore branch. `CrossRealmCopy.RequireSameAccount` assumes both realms share the
  same auth database (account ids must match).
- The source database is accessed with a **fresh, short-lived connection per copy**,
  with connect/read timeouts, opened and closed on a worker thread. No idle long-lived
  connection exists that a MySQL server or firewall could silently drop (the classic
  cause of `SIGPIPE` crashes with permanently-open secondary connections). On Linux,
  `SIGPIPE` is additionally ignored process-wide at startup, turning any broken-pipe
  write into a regular error.
- All writes to this realm happen in **one atomic transaction**: a failed copy leaves
  the target character untouched.
- The world thread never blocks on the source database: lookups and snapshots run on a
  worker thread and the world only polls their completion.
- The copy waits until the kicked player's logout save is confirmed flushed (the
  `online` flag clearing behind the logout save) before writing anything.

## Installation

1. Clone this module into your `modules/` directory as `mod-crossrealm-copy`.
2. Re-run CMake and rebuild the worldserver.
3. Copy `conf/mod_crossrealm_copy.conf.dist` next to your `worldserver.conf` (or let the
   installer do it), set `CrossRealmCopy.Enabled = 1` and configure
   `CrossRealmCopy.SourceDatabaseInfo`.

The MySQL user configured in `SourceDatabaseInfo` only needs `SELECT` on the source
characters database.

## Configuration

See `conf/mod_crossrealm_copy.conf.dist` for all options: enable switch, source database
connection, same-account requirement, GM bypass security level, max target level, kick
delay, safety ban duration, queue size and network timeouts.
