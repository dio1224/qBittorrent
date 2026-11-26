# Dio's qBittorrent build

This is qBittorrent (based on the official release 5.3.1) with a few additions:

- **Filter builder in Search.** Under the results filter box: "Add filter" rows (contains / doesn't
  contain / contains the word / doesn't contain the word) and "match all (AND) / any (OR)". The filters
  stay in place between searches and restarts.
- **Bundled search plugins:** 1337x, EXT Torrents, YTS, Nyaa, FitGirl Repacks, XXXClub, Internet
  Archive and RuTracker (`dio-extras/engines`). The app installs them, and updates them when this
  build carries a newer version, every time it starts.
- **Cloudflare helper** (`dio-extras/cfhelper/cfpass.py`) for the sites behind Cloudflare. Search tab,
  Extras: set up the helper (creates `~/.qbt-cf` with SeleniumBase and curl_cffi), get a fresh
  Cloudflare pass, and set the RuTracker login (saved only in the local RuTracker plugin file).

## Getting a new build

Every push to the `dio` branch runs the "Dio build - macOS app" workflow, which publishes
`qbittorrent.dmg` under Releases. The app is ad-hoc signed, so the first time macOS may refuse to open
it: right-click the app and choose Open, or run `xattr -cr /Applications/qbittorrent.app`.

## Updating to a new qBittorrent release

Merge the new `release-x.y.z` tag from upstream into `dio`, push, and use the new release download.
