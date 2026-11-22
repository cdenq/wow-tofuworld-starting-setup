# wow-tofuworld-starting-setup

Tofu Worldview module for the mod-playerbots fork of AzerothCore (3.3.5a). Written against core 32fd347 and mod-playerbots ecc4a53. Requires mod-playerbots in the same build (it calls `RandomPlayerbotMgr::IsRandomBot` and `IsAddclassBot`).

## What it does

- Starting kit, once per character: weapon proficiencies, gold, random bag(s), items, and an optional welcome mail. New characters get it at first login; existing characters only with `StartingSetup.ExistingCharacters = 1`, by mail. Random bots and addclass bots never get it; altbots (player-made characters) do. Recorded in `acore_characters.tofu_starting_setup`.
- Weapon skills: mode 1 learns every weapon the class can normally train (the trainers' own check, so mod-uac combinations are covered); mode 2 learns every weapon for every class, re-learned at each login because the core removes off-class skills when a character loads.
- A new character joins the configured channel (default `World`) at its first login; after that the client rejoins it on its own, and a character that leaves with `/leave World` stays out. The first-login join runs 3 seconds after login, because a join sent during login is dropped by the client (the server would count the player as a member while the client does not, and a manual `/join` would then answer "already on the channel"). Bots (headless sessions) join at every login, since they have no client.
- PvP on by default (both off in the dist): `StartingSetup.PvPDefaultOn = 1` turns `/pvp` on for a new player-made character at its first login, once. The core saves `PLAYER_FLAGS_IN_PVP` in `characters.playerFlags` and restores it at login, so a later `/pvp` off sticks. `StartingSetup.PvPDefaultOn.RandomBots = 1` turns it on for every random bot at every login, so bots fight flagged enemies in the open world (playerbots' `pvp` strategy). Meant for a Normal realm (`GameType = 0`).

## Files

- `src/starting_setup.cpp`, `src/starting_setup_loader.cpp`: the module. The repo is `wow-tofuworld-starting-setup`, but its folder in the deploy set must be named `mod-tofu-starting-setup`, because the core calls `Addmod_tofu_starting_setupScripts()` (Add + folder name with "-" as "_" + Scripts).
- `conf/starting_setup.conf.dist`: every setting, with descriptions.
- `data/sql/db-characters/tofu_starting_setup_01_table.sql`: the tracking table (applied by the worldserver's updater, state MODULE).

## Install

1. Put the repo in the deploy set as `C:\Users\chris\Desktop\WoW Projects\modules\mod-tofu-starting-setup` (committed, on a branch).
2. Run `Update Source.bat` (Rebuild anyway if nothing else changed).
3. Copy `C:\Build\bin\RelWithDebInfo\configs\modules\starting_setup.conf.dist` to `starting_setup.conf` in the same folder and edit it.
4. Keep `StartPlayerMoney = 0` in worldserver.conf, or new characters get gold twice.
5. Start the servers.

## Checks

- Worldserver log at startup: no `StartingSetup:` errors.
- `SHOW CREATE TABLE acore_characters.tofu_starting_setup;` returns the table.
- New character: bag equipped, 10 silver, class weapon skills in the skill tab, "Joined Channel: [N. World]" within a few seconds of the first login; after a relog still in World, no second kit. `/leave World`, relog: not in World. Delete the character and re-create it with the same name: in World again.
- `SELECT * FROM acore_characters.tofu_starting_setup;` has a row for it.
- With `PvPDefaultOn = 1`: the new character's portrait shows the PvP flag; `/pvp`, wait out the timer, relog: still unflagged. With `.RandomBots = 1`: target any random bot in the open world; its portrait shows the PvP flag.
