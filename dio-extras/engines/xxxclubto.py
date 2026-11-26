# VERSION: 2.00
# AUTHORS: BurningMop (original), rewritten for Dio's Mac (built with Claude)
#
# XXXClub search for qBittorrent. Rewritten in Nov 2026 for the site's current layout:
# search is /torrents/search/<category>/<query> (no extra options), results are 50 per page,
# and rows contain hidden decoy links that must not be followed.

import html
import os
import re
import subprocess
import sys
from datetime import datetime
from urllib.parse import quote, unquote

from helpers import retrieve_url
from novaprinter import prettyPrinter

CF_PYTHON = os.path.expanduser("~/.qbt-cf/bin/python")
CF_HELPER = os.path.expanduser("~/.qbt-cf/cfpass.py")


def blocked(page):
    return bool(re.search(r"<title>\s*Just a moment|_cf_chl_opt", page[:30000]))


def get(url):
    """Plain fetch; if Cloudflare ever shows up, go through the Cloudflare pass helper."""
    try:
        page = retrieve_url(url)
    except Exception as e:
        sys.stderr.write("XXXClub: fetch failed %s: %r\n" % (url, e))
        page = ""
    if page and not blocked(page):
        return page
    if os.path.exists(CF_HELPER):
        try:
            p = subprocess.run([CF_PYTHON, CF_HELPER, "fetch", url],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=240)
            if p.returncode == 0:
                return p.stdout.decode("utf-8", "replace")
        except Exception as e:
            sys.stderr.write("XXXClub: helper failed: %r\n" % (e,))
    return ""


def to_bytes(text):
    m = re.search(r"([\d.,]+)\s*([KMGTP]?)i?B", text or "", re.I)
    if not m:
        return -1
    return int(float(m.group(1).replace(",", "")) * 1024 ** " KMGTP".index(m.group(2).upper() or " "))


def to_timestamp(text):
    try:
        return int(datetime.strptime(text.strip(), "%d %b %Y %H:%M:%S").timestamp())
    except ValueError:
        return -1


def clean_magnet(link):
    """The site puts raw spaces in magnet names; qBittorrent splits on spaces. Encode them."""
    body = link[len("magnet:?"):]
    parts = re.split(r"&(?=(?:xt|dn|tr|xl|ws|as|xs|kt|mt|so|x\.pe)=)", body)
    out = []
    for part in parts:
        key, _, value = part.partition("=")
        out.append(part if key == "xt" else key + "=" + quote(unquote(value), safe=""))
    return "magnet:?" + "&".join(out)


ROW = re.compile(r"<li>\s*<span class=\"catlabe\".*?</li>", re.S)
# The real title link points at /torrents/details/; decoys point elsewhere and are hidden off-screen.
NAME = re.compile(r'<a href="(/torrents/details/[^"]+)"[^>]*>(.*?)</a>', re.S)


def cell(row, cls):
    m = re.search(r'<span class="%s[^"]*">([^<]*)</span>' % cls, row)
    return m.group(1).strip() if m else ""


class xxxclubto(object):
    url = "https://xxxclub.to"
    name = "XXXClub"
    supported_categories = {"all": "all", "pictures": "5"}
    max_pages = 2  # 50 per page; the site caps searches at 100 results

    def search(self, what, cat="all"):
        category = self.supported_categories.get(cat, "all")
        query = quote(unquote(what).replace("+", " "))
        page_url = "%s/torrents/search/%s/%s" % (self.url, category, query)
        for _ in range(self.max_pages):
            page = get(page_url)
            rows = ROW.findall(page.split("browsetableinside", 1)[-1]) if "browsetableinside" in page else []
            for row in rows:
                m = NAME.search(row)
                if not m:
                    continue
                desc = self.url + m.group(1)
                prettyPrinter({
                    "link": desc,
                    "name": html.unescape(re.sub(r"<[^>]+>", "", m.group(2))).strip(),
                    "size": to_bytes(cell(row, "siz")),
                    "seeds": cell(row, "see") or "-1",
                    "leech": cell(row, "lee") or "-1",
                    "engine_url": self.url,
                    "desc_link": desc,
                    "pub_date": to_timestamp(cell(row, "adde")),
                })
            # Real "Next Page" link is the visible one; decoys are positioned off-screen.
            nxt = None
            for a in re.findall(r'<a[^>]*title="Next Page"[^>]*>', page):
                if "-9999px" not in a:
                    h = re.search(r'href="([^"]+)"', a)
                    nxt = h and h.group(1)
                    break
            if len(rows) < 50 or not nxt:
                break
            page_url = self.url + nxt

    def download_torrent(self, info):
        page = get(info)
        m = re.search(r'href="(magnet:\?[^"]+)"', page)
        if not m:
            sys.stderr.write("XXXClub: no magnet on %s\n" % info)
            return
        print(clean_magnet(html.unescape(m.group(1))) + " " + info)
