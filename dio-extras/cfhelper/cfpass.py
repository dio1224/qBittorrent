#!/usr/bin/env python3
"""Cloudflare pass helper for Dio's qBittorrent search plugins (1337x, EXT).

Lives in ~/.qbt-cf and runs with that folder's own Python (~/.qbt-cf/bin/python).
  cfpass.py fetch <url>    print the page; opens Chrome only if the saved pass stopped working
  cfpass.py refresh <url>  always open Chrome on that site and save a fresh pass
  cfpass.py post <url> <form-body|-> <referer> [form]   send a form (EXT magnets, RuTracker login);
                                                      "-" reads the body from stdin, "form" = normal form post
The pass (Cloudflare cookie + matching browser ID) is saved in ~/.qbt-cf/session.json.
"""
import fcntl
import json
import os
import re
import sys
import time
from urllib.parse import urlparse

BASE = os.path.dirname(os.path.abspath(__file__))
SESSION = os.path.join(BASE, "session.json")
PROFILE = os.path.join(BASE, "chrome-profile")
LOCK = os.path.join(BASE, "refresh.lock")
LOG = os.path.join(BASE, "cfpass.log")
BROWSER_WAIT = 120  # seconds to wait for the check to clear (or for a click)


def log(msg):
    try:
        with open(LOG, "a") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except Exception:
        pass


def challenged(text):
    head = text[:30000]
    return bool(re.search(r"<title>\s*Just a moment|_cf_chl_opt|Performing security verification", head))


def host_of(url):
    return (urlparse(url).hostname or "").lower()


def load():
    try:
        with open(SESSION) as f:
            return json.load(f)
    except Exception:
        return {}


def save(data):
    tmp = SESSION + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, SESSION)


def fetch_once(url, data=None, referer=None, ajax=True):
    """GET (or POST when data is given) with the saved pass; keeps any new site cookies."""
    from curl_cffi import requests as cr
    host = host_of(url)
    entry = load().get(host, {})
    headers = {"User-Agent": entry["ua"]} if entry.get("ua") else {}
    with cr.Session(impersonate="chrome") as s:
        s.cookies.update(entry.get("cookies", {}))
        if data is None:
            r = s.get(url, headers=headers, timeout=30, allow_redirects=True)
        else:
            headers.update({"Content-Type": "application/x-www-form-urlencoded",
                            "Origin": "https://" + host, "Referer": referer or url})
            if ajax:
                headers.update({"X-Requested-With": "XMLHttpRequest",
                                "Accept": "application/json, text/javascript, */*; q=0.01"})
            r = s.post(url, data=data, headers=headers, timeout=30)
        jar = {}
        for c in s.cookies.jar:
            jar[c.name] = c.value
    if jar:
        merged = dict(entry.get("cookies", {}))
        merged.update(jar)
        if merged != entry.get("cookies"):
            all_data = load()
            all_data.setdefault(host, {})["cookies"] = merged
            save(all_data)
    return r.status_code, r.text


def open_browser(url):
    """Open Chrome on the site, wait for the real page (a click if needed), save the pass."""
    from seleniumbase import SB
    ok = False
    with SB(uc=True, headless=False, user_data_dir=PROFILE) as sb:
        sb.uc_open_with_reconnect(url, 3)
        end = time.time() + BROWSER_WAIT
        while time.time() < end:
            if not challenged(sb.get_page_source()):
                ok = True
                break
            time.sleep(1.5)
        if ok:
            time.sleep(1)
            data = load()
            data[host_of(url)] = {
                "ua": sb.get_user_agent(),
                "cookies": {c["name"]: c["value"] for c in sb.get_cookies()},
                "saved": int(time.time()),
            }
            save(data)
    log("browser pass for %s: %s" % (host_of(url), "ok" if ok else "FAILED"))
    return ok


def refresh(url, force=False):
    # One Chrome at a time: both plugins share the same Chrome profile.
    with open(LOCK, "w") as lf:
        fcntl.flock(lf, fcntl.LOCK_EX)
        if not force:
            try:
                status, text = fetch_once(url)  # another search may have just renewed it
                if not challenged(text):
                    return True
            except Exception:
                pass
        return open_browser(url)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("fetch", "refresh", "post"):
        sys.stderr.write(__doc__)
        return 1
    cmd, url = sys.argv[1], sys.argv[2]
    # qBittorrent starts us from "/" (read-only); Chrome automation writes a working
    # folder in the current directory, so always work from our own folder.
    os.chdir(BASE)
    try:
        if cmd == "refresh":
            ok = refresh(url, force=True)
            print("Pass saved for " + host_of(url) if ok else "Could not get a pass for " + host_of(url))
            return 0 if ok else 3
        if cmd == "post":
            body = sys.stdin.read() if sys.argv[3] == "-" else sys.argv[3]
            status, text = fetch_once(url, data=body, referer=sys.argv[4] if len(sys.argv) > 4 else None,
                                      ajax=not (len(sys.argv) > 5 and sys.argv[5] == "form"))
            sys.stdout.buffer.write(text.encode("utf-8"))
            return 0 if status < 400 and not challenged(text) else 3
        status, text = fetch_once(url)
        if challenged(text):
            log("pass not accepted by %s (HTTP %s), opening Chrome" % (host_of(url), status))
            if not refresh(url):
                return 3
            status, text = fetch_once(url)
            if challenged(text):
                log("still blocked after browser pass: " + url)
                return 3
        sys.stdout.buffer.write(text.encode("utf-8"))
        return 0
    except Exception as e:
        log("error on %s: %r" % (url, e))
        sys.stderr.write("cfpass error: %r\n" % (e,))
        return 2


if __name__ == "__main__":
    sys.exit(main())
