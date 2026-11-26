# VERSION: 1.00
# AUTHORS: Dio (built with Claude)
# LICENSING INFORMATION: personal use
#
# 1337x search for qBittorrent. Pages are fetched through ~/.qbt-cf/cfpass.py,
# which handles the Cloudflare check using your own Chrome.

import html
import os
import re
import subprocess
import sys
import time
from datetime import datetime

from novaprinter import prettyPrinter

CF_PYTHON = os.path.expanduser("~/.qbt-cf/bin/python")
CF_HELPER = os.path.expanduser("~/.qbt-cf/cfpass.py")


def cf_get(url):
    try:
        p = subprocess.run([CF_PYTHON, CF_HELPER, "fetch", url],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=240)
    except Exception as e:
        sys.stderr.write("1337x: could not run helper: %r\n" % (e,))
        return ""
    if p.returncode != 0:
        sys.stderr.write("1337x: helper failed (%d) on %s: %s\n" % (p.returncode, url, p.stderr.decode("utf-8", "replace")[-300:]))
        return ""
    return p.stdout.decode("utf-8", "replace")


def to_bytes(text):
    m = re.search(r"([\d.,]+)\s*([KMGTP]?)i?B", text, re.I)
    if not m:
        return -1
    power = " KMGTP".index(m.group(2).upper() or " ")
    return int(float(m.group(1).replace(",", "")) * 1024 ** power)


def to_timestamp(text):
    t = re.sub(r"<[^>]+>", "", text).strip()
    t = re.sub(r"(\d+)(st|nd|rd|th)\b", r"\1", t).replace(".", "")
    t = re.sub(r"\b([A-Za-z]{3})[a-z]*\b", r"\1", t)  # Sept -> Sep, June -> Jun
    t = re.sub(r"^\d{1,2}(:\d{2})?\s*[ap]m\s+(?=[A-Za-z])", "", t, flags=re.I)  # "9am Nov 14" -> "Nov 14"
    now = datetime.now()
    for fmt in ("%b %d '%y", "%b %d %Y"):
        try:
            return int(datetime.strptime(t, fmt).timestamp())
        except ValueError:
            pass
    try:  # this year, e.g. "Nov 3"
        d = datetime.strptime("%s %d" % (t, now.year), "%b %d %Y")
        if d > now:
            d = d.replace(year=now.year - 1)
        return int(d.timestamp())
    except ValueError:
        pass
    for fmt in ("%I:%M%p", "%I%p"):  # today, e.g. "10:22pm"
        try:
            tm = datetime.strptime(t.upper().replace(" ", ""), fmt)
            return int(now.replace(hour=tm.hour, minute=tm.minute, second=0, microsecond=0).timestamp())
        except ValueError:
            pass
    return -1


ROW = re.compile(
    r'<td class="coll-1 name"[^>]*>(?P<name>.*?)</td>\s*'
    r'<td class="coll-2 seeds">(?P<seeds>[\d,]+)</td>\s*'
    r'<td class="coll-3 leeches">(?P<leech>[\d,]+)</td>\s*'
    r'<td class="coll-date">(?P<date>.*?)</td>\s*'
    r'<td class="coll-4 size[^"]*">(?P<size>[^<]*)', re.S)
LINK = re.compile(r'href="(/torrent/\d+/[^"]*)"[^>]*>(.*?)</a>', re.S)


class leetx(object):
    url = "https://1337x.to"
    name = "1337x"
    supported_categories = {"all": "", "movies": "Movies", "tv": "TV", "music": "Music",
                            "games": "Games", "anime": "Anime", "software": "Apps"}
    max_pages = 3  # 20 results per page

    def parse(self, page):
        results = []
        for row in ROW.finditer(page):
            link = LINK.search(row.group("name"))
            if not link:
                continue
            path = link.group(1)
            title = html.unescape(re.sub(r"<[^>]+>", "", link.group(2))).strip()
            if title.endswith("..."):  # long names are cut short; the address has the full one
                title = path.rstrip("/").split("/")[-1].replace("-", " ")
            desc = self.url + path
            results.append({
                "link": desc,
                "name": title,
                "size": to_bytes(row.group("size")),
                "seeds": row.group("seeds").replace(",", ""),
                "leech": row.group("leech").replace(",", ""),
                "engine_url": self.url,
                "desc_link": desc,
                "pub_date": to_timestamp(row.group("date")),
            })
        return results

    def search(self, what, cat="all"):
        category = self.supported_categories.get(cat, "")
        for n in range(1, self.max_pages + 1):
            if category:
                page_url = "%s/category-search/%s/%s/%d/" % (self.url, what, category, n)
            else:
                page_url = "%s/search/%s/%d/" % (self.url, what, n)
            page = cf_get(page_url)
            rows = self.parse(page)
            for r in rows:
                prettyPrinter(r)
            if len(rows) < 20 or ("/%d/\">&gt;&gt;" % (n + 1)) not in page:
                break

    def download_torrent(self, info):
        page = cf_get(info)
        m = re.search(r'href="(magnet:\?xt=urn:btih:[^"]+)"', page)
        if not m:
            sys.stderr.write("1337x: no magnet link found on %s\n" % info)
            return
        print(html.unescape(m.group(1)) + " " + info)
