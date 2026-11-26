# VERSION: 1.01
# Dio's build: reads a torrent's own page (description, MediaInfo, file list) and reports
# quality details the name doesn't show, such as Dolby Vision or Atmos.
#
#   python dio_details.py <page url> [<page url> ...]
#
# Prints one JSON line per page as it finishes:
#   {"url": ..., "ok": true, "resolution": 2160, "hdr": 3, "hdrText": "DV", "audio": 5,
#    "audioText": "Atmos", "codec": "HEVC", "source": 6, "sourceText": "REMUX", "found": [...]}
# Pages are fetched through Dio's Cloudflare helper (~/.qbt-cf) when it's installed.

import html
import json
import os
import re
import sys
import threading
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from html.parser import HTMLParser

HELPER_DIR = os.path.expanduser("~/.qbt-cf")
USER_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36"
PRINT_LOCK = threading.Lock()

_cfpass = None
if os.path.exists(os.path.join(HELPER_DIR, "cfpass.py")):
    try:
        import curl_cffi  # noqa: F401  (only in the helper's own Python)
        sys.path.insert(0, HELPER_DIR)
        import cfpass as _cfpass
    except Exception:
        _cfpass = None


def fetch(url):
    if _cfpass is not None:
        status, text = _cfpass.fetch_once(url)
        if _cfpass.challenged(text):
            os.chdir(HELPER_DIR)  # Chrome automation works from the helper's folder
            if _cfpass.refresh(url):
                status, text = _cfpass.fetch_once(url)
        if _cfpass.challenged(text):
            raise RuntimeError("blocked by Cloudflare")
        if status >= 400:
            raise RuntimeError("HTTP %d" % status)
        return text
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:  # nosec B310
        charset = response.headers.get_content_charset() or "utf-8"
        return response.read().decode(charset, "replace")


VOID_TAGS = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}
SECTION_HINTS = re.compile(r"(^|[\s_-])(description|torrent-description|descr|files|file-list|filelist|torrent-file-list|"
                           r"post_body|mediainfo|media-info|tech-spec[\w-]*|nfo)($|[\s_-])", re.I)
SKIP_TAGS = {"script", "style", "noscript", "head", "nav", "footer"}


class SectionText(HTMLParser):
    """Text inside the description / MediaInfo / file-list parts of a page, and the page text without links."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.depth = 0
        self.capture_depth = []     # depths where a captured section started
        self.skip_depth = None
        self.link_depth = 0
        self.sections = []
        self.page = []
        self.post_bodies = 0

    def handle_starttag(self, tag, attrs):
        if tag in VOID_TAGS:
            if tag == "br":
                self._text("\n")
            return
        self.depth += 1
        if self.skip_depth is None and tag in SKIP_TAGS:
            self.skip_depth = self.depth
        if tag == "a":
            self.link_depth += 1
        attributes = dict(attrs)
        names = " ".join(filter(None, [attributes.get("id", ""), attributes.get("class", "")]))
        if names and SECTION_HINTS.search(names) and "comment" not in names.lower():
            if "post_body" in names:
                self.post_bodies += 1
                if self.post_bodies > 1:  # forum topics: only the first post is the release
                    return
            self.capture_depth.append(self.depth)

    def handle_endtag(self, tag):
        if tag in VOID_TAGS:
            return
        if self.capture_depth and self.capture_depth[-1] == self.depth:
            self.capture_depth.pop()
            self.sections.append("\n")
        if self.skip_depth == self.depth:
            self.skip_depth = None
        if tag == "a" and self.link_depth > 0:
            self.link_depth -= 1
        self.depth = max(0, self.depth - 1)

    def handle_data(self, data):
        self._text(data)

    def _text(self, data):
        if self.skip_depth is not None:
            return
        if self.capture_depth:
            self.sections.append(data)
        elif self.link_depth == 0:  # other torrents are listed as links: leave them out
            self.page.append(data)


def page_text(url, raw):
    host = urllib.parse.urlparse(url).hostname or ""
    if host.endswith("apibay.org"):
        try:
            return json.loads(raw).get("descr", ""), False
        except Exception:
            return raw, True
    parser = SectionText()
    try:
        parser.feed(raw)
    except Exception:
        pass
    text = "".join(parser.sections)
    if len(text.strip()) >= 40:
        return html.unescape(text), False
    # no description section found: the whole page, read strictly (only unmistakable terms)
    return html.unescape("".join(parser.page)), True


def details_url(url):
    # The Pirate Bay's page is built by script; its description comes from the API
    parts = urllib.parse.urlparse(url)
    if (parts.hostname or "").endswith("thepiratebay.org"):
        query = urllib.parse.parse_qs(parts.query)
        if "id" in query:
            return "https://apibay.org/t.php?id=" + query["id"][0]
    return url


def first(text, patterns):
    for pattern, value in patterns:
        if re.search(pattern, text):
            return value
    return None


def analyse(text, strict=False):
    """strict: the text is a whole page, not a description, so only unmistakable terms count."""
    t = text.lower()
    found = []
    result = {}

    hdr_patterns = [
        (r"dolby\s*vision|(?<![a-z0-9])dovi(?![a-z])|(?<![a-z0-9])dv(?![a-z])\s*(profile|p\d|hdr)|dvhe\.|dvh1", (3, "DV")),
        (r"hdr10\s*(\+|plus)|smpte\s*st\s*2094", (2, "HDR10+")),
        (r"hdr10|smpte\s*st\s*20(84|86)", (1, "HDR10")),
    ]
    if not strict:
        hdr_patterns += [
            (r"(?<![a-z])pq(?![a-z])|(?<![a-z0-9])hdr(?![a-z])", (1, "HDR10")),
            (r"(?<![a-z])hlg(?![a-z])|arib\s*std-?b67", (1, "HLG")),
        ]
    hdr = first(t, hdr_patterns)
    if hdr:
        result["hdr"], result["hdrText"] = hdr
        found.append(hdr[1])

    audio_patterns = [
        (r"atmos", (5, "Atmos")),
        (r"dts[\s:-]*x(?![a-z0-9])|dts\s*xll\s*x", (5, "DTS:X")),
        (r"true\s*-?hd|mlp\s*fba", (4, "TrueHD")),
        (r"dts-?hd\s*(master|ma)|dts\s*xll", (4, "DTS-HD MA")),
        (r"e-?ac-?3|dolby\s*digital\s*plus|(?<![a-z0-9])ddp(?![a-z])|dd\+", (3, "DD+")),
    ]
    if not strict:
        audio_patterns[4:4] = [
            (r"(?<![a-z])flac(?![a-z])", (4, "FLAC")),
            (r"(?<![a-z])l?pcm(?![a-z])", (4, "PCM")),
        ]
        audio_patterns += [
            (r"(?<![a-z])dts(?![a-z])", (3, "DTS")),
            (r"(?<![a-z])ac-?3(?![a-z])|dolby\s*digital", (2, "DD")),
            (r"(?<![a-z])aac(?![a-z])", (1, "AAC")),
        ]
    audio = first(t, audio_patterns)
    if audio:
        result["audio"], result["audioText"] = audio
        found.append(audio[1])

    result["found"] = found
    if strict:
        return result  # resolution, codec and source only from a real description

    resolution = first(t, [
        (r"(?<![0-9])2\s?160\s*(p|pixels)|3\s?840\s*(x|×|pixels)|(?<![a-z0-9])(4k|uhd)(?![a-z0-9])", 2160),
        (r"(?<![0-9])1\s?080\s*(p|i|pixels)|1\s?920\s*(x|×|pixels)", 1080),
        (r"(?<![0-9])720\s*(p|pixels)|1\s?280\s*(x|×|pixels)", 720),
    ])
    if resolution:
        result["resolution"] = resolution

    codec = first(t, [
        (r"(?<![a-z0-9])(hevc|h\.?265|x265)(?![a-z0-9])", "HEVC"),
        (r"(?<![a-z0-9])av1(?![a-z0-9])", "AV1"),
        (r"(?<![a-z0-9])(avc|h\.?264|x264)(?![a-z0-9])", "AVC"),
    ])
    if codec:
        result["codec"] = codec

    source = first(t, [
        (r"(?<![a-z])remux(?![a-z])", (6, "REMUX")),
        (r"blu-?ray|bdrip", (5, "Blu-ray")),
        (r"web-?dl", (4, "WEB-DL")),
        (r"web-?rip", (3, "WEBRip")),
    ])
    if source:
        result["source"], result["sourceText"] = source
    return result


def lookup(url):
    try:
        raw = fetch(details_url(url))
        text, strict = page_text(details_url(url), raw)
        info = analyse(text, strict)
        info.update({"url": url, "ok": True})
    except Exception as e:  # noqa: BLE001
        info = {"url": url, "ok": False, "error": str(e)[:200]}
    with PRINT_LOCK:
        sys.stdout.write(json.dumps(info) + "\n")
        sys.stdout.flush()


def main():
    urls = [u for u in sys.argv[1:] if u.startswith(("http://", "https://"))]
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lookup, urls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
