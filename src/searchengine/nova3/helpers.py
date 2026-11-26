# VERSION: 1.61

# Author:
#  Christophe DUMEZ (chris@qbittorrent.org)

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#    * Redistributions of source code must retain the above copyright notice,
#      this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in the
#      documentation and/or other materials provided with the distribution.
#    * Neither the name of the author nor the names of its contributors may be
#      used to endorse or promote products derived from this software without
#      specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

import datetime
import gzip
import html
import http.cookiejar
import io
import os
import socket
import ssl
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Mapping
from typing import Any, Optional, cast

import socks


def _getBrowserUserAgent() -> str:
    """
    Disguise as browser to circumvent website blocking
    """

    # Firefox release calendar
    # https://whattrainisitnow.com/calendar/
    # https://wiki.mozilla.org/index.php?title=Release_Management/Calendar&redirect=no

    baseDate = datetime.date(2024, 4, 16)
    baseVersion = 125

    nowDate = datetime.datetime.now(datetime.UTC).date()
    nowVersion = baseVersion + ((nowDate - baseDate).days // 30)

    return f"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:{nowVersion}.0) Gecko/20100101 Firefox/{nowVersion}.0"


_headers: dict[str, str] = {'User-Agent': _getBrowserUserAgent()}
_original_socket = socket.socket


def enable_socks_proxy(enable: bool) -> None:
    if enable:
        socksURL = os.environ.get("qbt_socks_proxy")
        if socksURL is not None:
            parts = urllib.parse.urlsplit(socksURL)
            resolveHostname = (parts.scheme == "socks4a") or (parts.scheme == "socks5h")
            if (parts.scheme == "socks4") or (parts.scheme == "socks4a"):
                socks.setdefaultproxy(socks.PROXY_TYPE_SOCKS4, parts.hostname, parts.port, resolveHostname)
                socket.socket = cast(type[socket.socket], socks.socksocket)  # type: ignore[misc]
            elif (parts.scheme == "socks5") or (parts.scheme == "socks5h"):
                socks.setdefaultproxy(socks.PROXY_TYPE_SOCKS5, parts.hostname, parts.port, resolveHostname, parts.username, parts.password)
                socket.socket = cast(type[socket.socket], socks.socksocket)  # type: ignore[misc]
    else:
        socket.socket = _original_socket  # type: ignore[misc]


# This is only provided for backward compatibility, new code should not use it
htmlentitydecode = html.unescape


def _cloudflare_fetch(url: str, request_data: Optional[Any] = None) -> Optional[str]:
    """Fetches a page (or sends a form, e.g. EZTV's search) through ~/.qbt-cf/cfpass.py, or None."""
    helper_python = os.path.expanduser("~/.qbt-cf/bin/python")
    helper = os.path.expanduser("~/.qbt-cf/cfpass.py")
    if not (os.path.exists(helper_python) and os.path.exists(helper)):
        return None
    import subprocess
    host = urllib.parse.urlparse(url).hostname or url
    if request_data is None:
        command = [helper_python, helper, "fetch", url]
        body = None
    else:
        body = request_data if isinstance(request_data, bytes) else str(request_data).encode()
        command = [helper_python, helper, "post", url, "-", url, "form"]
    try:
        result = subprocess.run(command, input=body, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=180)
    except Exception as e:  # noqa: BLE001
        print(f"Cloudflare helper could not run for {host}: {e!r}", file=sys.stderr)
        return None
    if result.returncode != 0:
        print(f"Cloudflare helper could not get past the check on {host}", file=sys.stderr)
        return None
    print(f"Got past Cloudflare on {host} with the helper", file=sys.stderr)
    return result.stdout.decode("utf-8", "replace")


def retrieve_url(url: str, custom_headers: Mapping[str, str] = {}, request_data: Optional[Any] = None, ssl_context: Optional[ssl.SSLContext] = None, unescape_html_entities: bool = True) -> str:
    """
    Return the content of the url page as a string
    """

    request = urllib.request.Request(url, request_data, {**_headers, **custom_headers})
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()), urllib.request.HTTPSHandler(context=ssl_context))

    try:
        with opener.open(request) as response:
            contentType = response.getheader('Content-Type', '')
            data: bytes = response.read()
    except urllib.error.URLError as errno:
        # Dio's build: a site behind Cloudflare answers 403/429/503. When the Cloudflare
        # helper is set up, get the page through it (it may open Chrome once to pass the check).
        if getattr(errno, 'code', None) in (403, 429, 503):
            page = _cloudflare_fetch(url, request_data)
            if page is not None:
                return html.unescape(page) if unescape_html_entities else page
        print(f"Connection error: {errno.reason}", file=sys.stderr)
        return ""

    # Check if it is gzipped
    if data[:2] == b'\x1f\x8b':
        # Data is gzip encoded, decode it
        with io.BytesIO(data) as compressedStream, gzip.GzipFile(fileobj=compressedStream) as gzipper:
            data = gzipper.read()

    charset = 'utf-8'
    try:
        charset = contentType.split('charset=', 1)[1]
    except IndexError:
        pass

    dataStr = data.decode(charset, 'replace')

    if unescape_html_entities:
        dataStr = html.unescape(dataStr)

    return dataStr


def download_file(url: str, referer: Optional[str] = None, ssl_context: Optional[ssl.SSLContext] = None) -> str:
    """
    Download file at url and write it to a file, return both the path to the file and the url
    """

    # Download url
    request = urllib.request.Request(url, headers=_headers)
    if referer is not None:
        request.add_header('referer', referer)

    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()), urllib.request.HTTPSHandler(context=ssl_context))
    with opener.open(request) as response:
        data = response.read()

    # Check if it is gzipped
    if data[:2] == b'\x1f\x8b':
        # Data is gzip encoded, decode it
        with io.BytesIO(data) as compressedStream, gzip.GzipFile(fileobj=compressedStream) as gzipper:
            data = gzipper.read()

    # Write it to a file
    fileHandle, path = tempfile.mkstemp()
    with os.fdopen(fileHandle, "wb") as file:
        file.write(data)

    # return file path
    return f"{path} {url}"
