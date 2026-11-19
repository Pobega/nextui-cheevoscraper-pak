# CheevoScraper (`nextui-cheevoscraper-pak`)

A NextUI Tool Pak that scans your ROM library (including `.zip` and `.chd` disc images), computes RetroAchievements (`rcheevos`) hashes, and pre-populates the offline RetroAchievements response cache (`$SHARED_USERDATA_PATH/.ra/offline/cache/`) and badge cache (`$SHARED_USERDATA_PATH/.ra/badges/`).

## Features
- **Sync by System**: Pick a console folder and sync uncached games, all games, or hand-pick individual titles via a checklist.
- **Sync New Games**: Incrementally scan all supported ROM folders using a local hash index (`scraper_index.tsv`).
- **Refresh Sessions**: Quickly refresh `startsession` unlock states for already-cached games.
- **Cache Tools**: Force rebuild cache, re-check unrecognized ROMs (`GameID == 0`), and view cache stats/disk usage.
- **Settings**: Configure badge downloads (`All` / `Unlocked Only` / `None`) and skip disc systems for fast cartridge syncs.

## Installation
1. Download `CheevoScraper.pak.zip` from Releases.
2. Extract `CheevoScraper.pak` to `Tools/<platform>/CheevoScraper.pak` on your SD card (e.g. `Tools/tg5040/CheevoScraper.pak` or `Tools/h700/CheevoScraper.pak`).
3. Ensure your RetroAchievements account is signed in under **NextUI Settings**.
4. Launch **Tools → CheevoScraper** on your handheld.

## Building
Requires `podman` or `docker`:
```sh
make test
make build package
```
Output is written to `dist/CheevoScraper.pak.zip` and `dist/Tools/<platform>/CheevoScraper.pak/`.
