# VERSION: 3.00
# AUTHORS: nbusseneau (original parsing, https://github.com/nbusseneau/qBittorrent-RuTracker-plugin); network layer rewritten for Dio's Mac (built with Claude)
#
# RuTracker search for qBittorrent. RuTracker sits behind Cloudflare, which blocks plain requests
# from VPN addresses, so every request goes through ~/.qbt-cf/cfpass.py (the same Cloudflare pass
# helper the 1337x and EXT plugins use). The login session cookie is kept by that helper too.


class Config(object):
    # Filled in from qBittorrent: Search tab, Extras, RuTracker login. Use a password you don't use anywhere else.
    username = "YOUR_USERNAME_HERE"
    password = "YOUR_PASSWORD_HERE"


import html
import os
import re
import subprocess
import sys
from urllib.parse import quote, unquote, urlencode

from novaprinter import prettyPrinter

CF_PYTHON = os.path.expanduser("~/.qbt-cf/bin/python")
CF_HELPER = os.path.expanduser("~/.qbt-cf/cfpass.py")
BASE = "https://rutracker.org"


def cf_get(url):
    try:
        p = subprocess.run([CF_PYTHON, CF_HELPER, "fetch", url],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=240)
    except Exception as e:
        sys.stderr.write("RuTracker: could not run helper: %r\n" % (e,))
        return ""
    if p.returncode != 0:
        sys.stderr.write("RuTracker: helper failed (%d) on %s: %s\n" % (p.returncode, url, p.stderr.decode("utf-8", "replace")[-300:]))
        return ""
    return p.stdout.decode("utf-8", "replace")


def cf_post_form(url, body, referer):
    try:
        p = subprocess.run([CF_PYTHON, CF_HELPER, "post", url, "-", referer, "form"], input=body.encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except Exception as e:
        sys.stderr.write("RuTracker: could not run helper: %r\n" % (e,))
        return 1, ""


def logged_in(page):
    return "logged-in-username" in page or "login.php?logout=1" in page


def clean_magnet(link):
    body = link[len("magnet:?"):]
    parts = re.split(r"&(?=(?:xt|dn|tr|xl|ws|as|xs|kt|mt|so|x\.pe)=)", body)
    out = []
    for part in parts:
        key, _, value = part.partition("=")
        out.append(part if key == "xt" else key + "=" + quote(unquote(value), safe=""))
    return "magnet:?" + "&".join(out)


class rutracker(object):
    name = "RuTracker"
    url = BASE
    supported_categories = {"all": ""}
    max_pages = 4  # 50 per page

    # Result parsing from nbusseneau's plugin (v2.2.1, July 2026)
    re_search_queries = re.compile(r'<a.+?href="tracker\.php\?(.*?start=\d+)"')
    re_threads = re.compile(r'<tr id="trs-tr-\d+".*?</tr>', re.S)
    re_torrent_data = re.compile(
        r'a data-topic_id="(?P<id>\d+?)".*?>(?P<title>.+?)<'
        r".+?" r'data-ts_text="(?P<size>\d+?)"'
        r".+?" r'data-ts_text="(?P<seeds>[-\d]+?)"'
        r".+?" r"leechmed.+?>(?P<leech>\d+?)<"
        r".+?" r'data-ts_text="(?P<pub_date>\d+?)"',
        re.S,
    )

    def login(self):
        body = urlencode({"login_username": Config.username, "login_password": Config.password,
                          "login": "Вход"}, encoding="cp1251")
        cf_post_form(BASE + "/forum/login.php", body, BASE + "/forum/index.php")

    def get_logged_in(self, url):
        page = cf_get(url)
        if page and not logged_in(page):
            self.login()
            page = cf_get(url)
        if page and not logged_in(page):
            sys.stderr.write("RuTracker: login failed - check the username/password in the plugin\n")
            return ""
        return page

    def parse(self, page, seen):
        for thread in self.re_threads.findall(page):
            m = self.re_torrent_data.search(thread)
            if not m or m.group("id") in seen:
                continue
            seen.add(m.group("id"))
            topic = BASE + "/forum/viewtopic.php?t=" + m.group("id")
            prettyPrinter({
                "link": topic,
                "name": html.unescape(m.group("title")),
                "size": m.group("size"),
                "seeds": m.group("seeds"),
                "leech": m.group("leech"),
                "engine_url": BASE,
                "desc_link": topic,
                "pub_date": m.group("pub_date"),
            })

    def search(self, what, cat="all"):
        query = urlencode({"nm": unquote(what).replace("+", " ")}, encoding="cp1251")
        page = self.get_logged_in(BASE + "/forum/tracker.php?" + query)
        if not page:
            return
        seen = set()
        self.parse(page, seen)
        others = list(dict.fromkeys(self.re_search_queries.findall(page)))[: self.max_pages - 1]
        for q in others:
            self.parse(cf_get(BASE + "/forum/tracker.php?" + html.unescape(q)), seen)

    def download_torrent(self, info):
        page = self.get_logged_in(info)
        m = re.search(r'href="(magnet:\?[^"]+)"', page)
        if not m:
            sys.stderr.write("RuTracker: no magnet link on %s\n" % info)
            return
        print(clean_magnet(html.unescape(m.group(1))) + " " + info)
