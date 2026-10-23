# Rommarch

Rommarch is a fork of [RetroArch](https://www.retroarch.com/) with a built-in client for [RomM](https://github.com/rommapp/romm) (ROM Manager): a self-hosted game library server. Instead of copying ROMs onto the device RetroArch runs on, Rommarch talks to your RomM server directly — it lists your library, downloads a game into memory when you select it, and syncs saves and save states back to RomM by game id.

This is a personal project, built with heavy use of Claude (Anthropic's AI). It is not affiliated with, endorsed by, or submitted upstream to either the RetroArch or RomM projects. The stock RetroArch README is kept at [README-retroarch.md](README-retroarch.md).

## Why

RetroArch is a great multi-system emulator front end, but it expects a local ROM collection. RomM is a great self-hosted library/metadata server, but it has no native emulator — only a web player. Keeping a RomM library in sync with a local RetroArch folder by hand doesn't scale past a handful of games.

Rommarch's goal: open the app, see your whole RomM library as normal RetroArch playlists, pick a game, play it — with no separate sync step, no local copy to manage, and saves that follow you between devices through RomM.

## How it works

- **The library is native RetroArch playlists.** Rommarch writes one `.lpl` playlist per RomM platform (named by `libretro_slug`, so thumbnails, history and favorites work unmodified) plus a local JSON manifest. This reuses every existing menu driver (Ozone, XMB, RGUI, MaterialUI) instead of building a custom browser.
- **Games load from RAM, not disk.** Selecting a game downloads it from RomM into a RAM-backed blob store, exposed through a new `romm://` VFS scheme (alongside RetroArch's existing `smb://`/`nfs://`/CD-ROM schemes). Cores that accept RetroArch's file layer read the game straight out of RAM; a handful of cores that insist on a real file path get a temporary copy that's deleted on unload. BIOS files are downloaded into RetroArch's managed `system/` folder, since cores look those up by path.
- **Saves and states sync per RomM game id**, not by file name, pulled before a game launches and pushed after it closes. This was a deliberate choice over RomM's name-based save matching, which breaks on renamed or multi-disc games.
- **Pairing, not a password.** Setup uses RomM's device-pairing flow (approve a code in RomM's web UI), the same shape as RetroArch's existing Google Drive login, rather than storing a RomM username and password on the device.
- **A pre-launch task does all the networking** (resolve/install the right core, fetch BIOS, pull saves, download the game) with a progress bar and cancel, before handing off to RetroArch's normal, synchronous playlist-load path — so the UI never blocks on the network mid-load.

## Status

Actively developed, for personal use on macOS (Apple Silicon), with Windows and Linux builds also exercised in CI/Docker. Not packaged for distribution and not intended for the general public.

Done so far:
- Device pairing, authenticated RomM API access, settings sync.
- Library sync to native playlists, with per-system sync control (on/off, full refetch, rebuild) and automatic core downloads.
- In-memory game loading for cartridge-era and most disc-based systems; the few emulators that can't read through RetroArch's file layer are excluded rather than falling back to a disk copy silently.
- Per-game save/state sync by RomM game id.
- A RomM-aware sidebar (collections, favorites, per-system pages) and game information pulled from RomM's metadata.

Only a subset of systems and cores has actually been tested end to end (against a real library, not just read in code); others may work but haven't been verified.

Not yet built: a guided first-run setup wizard, a save-conflict resolution screen, offline/pre-downloaded play, and arcade system support.

## Building

Same as upstream RetroArch, with one extra flag:

```
./configure --enable-romm
make -j
```

That produces a `rommarch` binary (`config.mk` will show `HAVE_ROMM = 1`). On macOS, build through Xcode as upstream describes in [README-retroarch.md](README-retroarch.md), with `-DHAVE_ROMM` added to the build settings.

CI runs macOS, Linux, Windows x64 (MXE cross-compile) and a RomM-enabled smoke build (`.github/workflows/Rommarch.yml`); RetroArch's many other upstream workflows are kept but disabled (moved to `.github/workflows-disabled/`) to keep this fork's Actions usage focused.
