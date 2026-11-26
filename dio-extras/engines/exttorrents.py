# VERSION: 1.03
# AUTHORS: Dio (built with Claude)
# LICENSING INFORMATION: personal use
#
# EXT Torrents (ext.to) search for qBittorrent. Pages are fetched through
# ~/.qbt-cf/cfpass.py, which handles the Cloudflare check using your own Chrome.

import hashlib
import html
import json
import time
import os
import re
import subprocess
import sys
from datetime import datetime
from urllib.parse import quote, unquote, urlencode

from novaprinter import prettyPrinter

CF_PYTHON = os.path.expanduser("~/.qbt-cf/bin/python")
CF_HELPER = os.path.expanduser("~/.qbt-cf/cfpass.py")
DEBUG_LOG = os.path.expanduser("~/qBittorrent Plugins/plugin.log")


def dlog(msg):
    try:
        with open(DEBUG_LOG, "a") as f:
            f.write(time.strftime("%H:%M:%S ") + "EXT " + msg + "\n")
    except Exception:
        pass


def cf_get(url):
    try:
        p = subprocess.run([CF_PYTHON, CF_HELPER, "fetch", url],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=240)
    except Exception as e:
        sys.stderr.write("EXT: could not run helper: %r\n" % (e,))
        return ""
    if p.returncode != 0:
        dlog("helper failed %d on %s: %s" % (p.returncode, url, p.stderr.decode("utf-8", "replace")[-300:]))
        sys.stderr.write("EXT: helper failed (%d) on %s: %s\n" % (p.returncode, url, p.stderr.decode("utf-8", "replace")[-300:]))
        return ""
    return p.stdout.decode("utf-8", "replace")


def cf_post(url, body, referer):
    try:
        p = subprocess.run([CF_PYTHON, CF_HELPER, "post", url, body, referer],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except Exception as e:
        sys.stderr.write("EXT: could not run helper: %r\n" % (e,))
        return ""
    if p.returncode != 0:
        dlog("post helper exit %d: %s" % (p.returncode, p.stderr.decode("utf-8", "replace")[-300:]))
    return p.stdout.decode("utf-8", "replace")


def clean_magnet(link):
    """ext.to sends magnets with raw spaces in the name; qBittorrent splits on spaces. Encode them."""
    body = link[len("magnet:?"):]
    parts = re.split(r"&(?=(?:xt|dn|tr|xl|ws|as|xs|kt|mt|so|x\.pe)=)", body)
    out = []
    for part in parts:
        key, _, value = part.partition("=")
        out.append(part if key == "xt" else key + "=" + quote(unquote(value), safe=""))
    return "magnet:?" + "&".join(out)


def to_bytes(text):
    m = re.search(r"([\d.,]+)\s*([KMGTP]?)i?B", text, re.I)
    if not m:
        return -1
    power = " KMGTP".index(m.group(2).upper() or " ")
    return int(float(m.group(1).replace(",", "")) * 1024 ** power)


def to_timestamp(text):
    try:
        return int(datetime.strptime(text.strip(), "%d %B %Y").timestamp())
    except ValueError:
        return -1


def field(row, label):
    m = re.search(r'<span class="add-block">%s</span>\s*<span[^>]*>([^<]*)</span>' % label, row)
    return m.group(1).strip() if m else ""


class exttorrents(object):
    url = "https://ext.to"
    name = "EXT Torrents"
    supported_categories = {"all": "", "movies": "1", "tv": "2", "music": "3", "games": "4",
                            "software": "5", "books": "6", "anime": "7"}
    max_pages = 2  # 50 results per page

    def parse(self, page):
        results = []
        body = page.split("<tbody>", 1)[-1].split("</tbody>", 1)[0] if "<tbody>" in page else ""
        for row in body.split("<tr>")[1:]:
            t = re.search(r'<a href="([^"]+)" class="torrent-title-link"[^>]*>(.*?)</a>', row, re.S)
            if not t:
                continue
            desc = self.url + t.group(1)
            age = re.search(r'<span class="add-block">Age</span>\s*<span title="([^"]+)"', row)
            results.append({
                "link": desc,
                "name": html.unescape(re.sub(r"<[^>]+>", "", t.group(2))).strip(),
                "size": to_bytes(field(row, "Size")),
                "seeds": field(row, "Seeds").replace(",", "") or "-1",
                "leech": field(row, "Leechs").replace(",", "") or "-1",
                "engine_url": self.url,
                "desc_link": desc,
                "pub_date": to_timestamp(age.group(1)) if age else -1,
            })
        return results

    def search(self, what, cat="all"):
        category = self.supported_categories.get(cat, "")
        for n in range(1, self.max_pages + 1):
            page_url = "%s/browse/?q=%s&sort=seeds&order=desc&page=%d" % (self.url, what, n)
            if category:
                page_url += "&cat=" + category
            page = cf_get(page_url)
            rows = self.parse(page)
            for r in rows:
                prettyPrinter(r)
            if len(rows) < 50 or ("page=%d" % (n + 1)) not in page:
                break

    def download_torrent(self, info):
        dlog("download asked for %r | python %s | args %r" % (info, sys.executable, sys.argv))
        try:
            self._download(info)
        except Exception:
            import traceback
            dlog("CRASH: " + traceback.format_exc())

    def _download(self, info):
        # The torrent page holds two tokens; the site's own magnet button sends them
        # with a SHA-256 signature to /ajax/getTorrentMagnet.php. We do the same.
        page = cf_get(info)
        dlog("torrent page: %d chars" % len(page))
        direct = re.search(r'href="(magnet:\?xt=urn:btih:[^"]+)"', page)
        if direct:
            print(html.unescape(direct.group(1)) + " " + info)
            return
        tid = re.search(r'download-btn-magnet"[^>]*data-id="(\d+)"', page) or \
            re.search(r'id="show-hash-btn" data-id="(\d+)"', page) or re.search(r'-(\d+)/?$', info)
        ptoken = re.search(r"window\.pageToken\s*=\s*'([^']+)'", page)
        sessid = re.search(r"window\.csrfToken\s*=\s*'([^']+)'", page) or \
            re.search(r'<meta name="csrf-token" content="([^"]+)"', page)
        if not (tid and ptoken and sessid):
            sys.stderr.write("EXT: magnet tokens not found on %s\n" % info)
            return
        torrent_id, ts = tid.group(1), int(time.time())
        sig = hashlib.sha256(("%s|%d|%s" % (torrent_id, ts, ptoken.group(1))).encode()).hexdigest()
        body = urlencode({"torrent_id": torrent_id, "download_type": "magnet", "timestamp": ts,
                          "hmac": sig, "sessid": sessid.group(1)})
        raw = cf_post(self.url + "/ajax/getTorrentMagnet.php", body, info)
        dlog("magnet reply: %s" % raw[:300])
        try:
            resp = json.loads(raw)
        except ValueError:
            sys.stderr.write("EXT: unexpected magnet reply: %s\n" % raw[:200])
            return
        link = resp.get("url") or ""
        if not link.startswith("magnet:") and resp.get("hash"):
            link = "magnet:?xt=urn:btih:" + resp["hash"]
        if not link.startswith("magnet:"):
            sys.stderr.write("EXT: no magnet in reply: %s\n" % raw[:200])
            return
        if "&dn=" not in link:
            name = re.search(r"<title>(.*?)</title>", page, re.S)
            if name:
                link += "&" + urlencode({"dn": re.sub(r"^Download\s+", "", html.unescape(name.group(1)).split(" Torrent")[0].strip())})
        link = clean_magnet(link)
        dlog("printing: %s" % (link + " " + info)[:200])
        print(link + " " + info)
