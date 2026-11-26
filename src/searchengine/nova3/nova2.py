# VERSION: 1.55

# Author:
#  Fabien Devaux <fab AT gnux DOT info>
# Contributors:
#  Christophe Dumez <chris@qbittorrent.org> (qbittorrent integration)
#  Thanks to gab #gcu @ irc.freenode.net (multipage support on PirateBay)
#  Thanks to Elias <gekko04@users.sourceforge.net> (torrentreactor and isohunt search engines)

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

import importlib
import multiprocessing as MP
import pathlib
import sys
import traceback
import urllib.parse
import xml.etree.ElementTree as ET
from abc import ABC, abstractmethod
from collections.abc import Iterable
from enum import Enum
from glob import glob
from os import path
from typing import Optional

# qbt tend to run this script in 'isolate mode' so append the current path manually
current_path = str(pathlib.Path(__file__).parent.resolve())
if current_path not in sys.path:
    sys.path.append(current_path)

import helpers

# enable SOCKS proxy for all plugins by default
helpers.enable_socks_proxy(True)

THREADED: bool = True
try:
    MAX_THREADS: int = MP.cpu_count()
except NotImplementedError:
    MAX_THREADS = 1  # pyright: ignore[reportConstantRedefinition]

Category = Enum('Category', ['all', 'anime', 'books', 'games', 'movies', 'music', 'pictures', 'software', 'tv'])

EngineModuleName = str  # the filename of the engine plugin


class Engine(ABC):
    """
    The base class for a search engine

    An engine must implement a ``search()`` method taking a **space-free** string
    as argument (for example ``family+guy``)

    If the site requires special handling for downloading .torrent files, then a
    customized ``download_torrent()`` method must be implemented. For example:
    .. code-block:: python

        def download_torrent(self, url: str) -> None:
            print(helpers.download_file(url))

    """

    name: str
    url: str
    supported_categories: dict[str, str]

    @abstractmethod
    def search(self, query: str, category: str = Category.all.name) -> None:
        """
        Run the search query

        Usually it would call ``novaprinter.prettyPrinter()`` with a ``dict`` as an argument
        (or actually ``novaprinter.SearchResults``).
        Refer to ``novaprinter.SearchResults`` for the keys.

        When printing the search results, it is recommended to list the results by decreasing
        number of seeds, or some other criteria that is sensible to the end user.
        """

        #novaprinter.prettyPrinter()
        raise NotImplementedError


# global state
engine_dict: dict[EngineModuleName, Optional[type[Engine]]] = {}


def list_engines() -> list[EngineModuleName]:
    """
    List all engines

    Including broken engines that might fail on import.

    :return: A list of all engines' module name
    """

    names: list[EngineModuleName] = []

    for engine_path in glob(path.join(path.dirname(__file__), 'engines', '*.py')):
        engine_module_name = path.basename(engine_path).split('.')[0].strip()
        if len(engine_module_name) == 0 or engine_module_name.startswith('_'):
            continue
        names.append(engine_module_name)

    return sorted(names)


def import_engine(engine_module_name: EngineModuleName) -> Optional[type[Engine]]:
    if engine_module_name in engine_dict:
        return engine_dict[engine_module_name]

    # when import fails, return `None`
    engine_class = None
    try:
        # import engines.[engine_module_name]
        engine_module = importlib.import_module(f"engines.{engine_module_name}")
        engine_class = getattr(engine_module, engine_module_name)
    except Exception:  # noqa: BLE001, S110
        pass

    engine_dict[engine_module_name] = engine_class
    return engine_class


def get_capabilities(engines: Iterable[EngineModuleName]) -> str:
    """
    Return capabilities in XML format

    For example:
    .. code-block:: xml

        <capabilities>
          <engine_module_name>
            <name>long name</name>
            <url>http://example.com</url>
            <categories>movies music games</categories>
          </engine_module_name>
        </capabilities>

    """

    capabilities_element = ET.Element('capabilities')

    for engine_module_name in engines:
        engine_class = import_engine(engine_module_name)
        if engine_class is None:
            continue

        engine_module_element = ET.SubElement(capabilities_element, engine_module_name)

        ET.SubElement(engine_module_element, 'name').text = engine_class.name
        ET.SubElement(engine_module_element, 'url').text = engine_class.url

        supported_categories: set[str] = set()
        if hasattr(engine_class, "supported_categories"):
            for cat in engine_class.supported_categories:
                if cat in Category.__members__:
                    if cat != Category.all.name:
                        supported_categories.add(cat)
                else:
                    print(f"Search engine has invalid category. Search engine: '{engine_class.name}'. Invalid category: '{cat}'", file=sys.stderr)
        ET.SubElement(engine_module_element, 'categories').text = " ".join(sorted(supported_categories))

    ET.indent(capabilities_element)
    return ET.tostring(capabilities_element, 'unicode')


def run_search(search_params: tuple[type[Engine], str, Category]) -> bool:
    """
    Run search in engine

    :param search_params: A tuple with engine, query and category.
    :return: ``False`` if any exceptions occurred. ``True`` otherwise.
    """

    engine_class, what, cat = search_params

    # Dio's build: count this engine's results and keep its error messages, then
    # report one status line per engine so qBittorrent can show which sites failed.
    module = sys.modules.get(engine_class.__module__)
    counter = _ResultCounter()
    patched = []
    for owner in (module, sys.modules.get('novaprinter')):
        original = getattr(owner, 'prettyPrinter', None) if owner is not None else None
        if callable(original) and not isinstance(original, _CountingPrinter):
            setattr(owner, 'prettyPrinter', _CountingPrinter(original, counter))
            patched.append((owner, original))
    real_stderr = sys.stderr
    captured = _TeeStderr(real_stderr)
    sys.stderr = captured
    success = True
    try:
        engine = engine_class()
        # avoid exceptions due to invalid category
        if hasattr(engine, 'supported_categories'):
            if cat.name in engine.supported_categories:
                engine.search(what, cat.name)
        else:
            engine.search(what)
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        success = False
    finally:
        sys.stderr = real_stderr
        for owner, original in patched:
            setattr(owner, 'prettyPrinter', original)
        _report_status(engine_class, counter.count, captured.text(), success)
    return success


class _ResultCounter:
    def __init__(self) -> None:
        self.count = 0


class _CountingPrinter:
    def __init__(self, printer, counter: _ResultCounter) -> None:
        self._printer = printer
        self._counter = counter

    def __call__(self, *args, **kwargs):
        result = self._printer(*args, **kwargs)
        self._counter.count += 1
        return result


class _TeeStderr:
    """Passes everything through to stderr and keeps a copy (first 4000 characters)."""
    def __init__(self, stream) -> None:
        self._stream = stream
        self._parts: list[str] = []
        self._size = 0

    def write(self, text: str) -> int:
        if self._size < 4000:
            self._parts.append(text)
            self._size += len(text)
        return self._stream.write(text)

    def flush(self) -> None:
        self._stream.flush()

    def text(self) -> str:
        return ''.join(self._parts)

    def __getattr__(self, name):
        return getattr(self._stream, name)


_BLOCKED_HINTS = ('forbidden', '403', 'cloudflare', 'just a moment', 'captcha', 'challenge', 'access denied', 'too many requests', '429')
_DOWN_HINTS = ('timed out', 'timeout', 'gateway', '502', '503', '504', 'bad gateway', 'service unavailable',
               'nodename nor servname', 'name or service not known', 'connection refused', 'unreachable',
               'temporary failure', 'connection reset', 'remote end closed', 'certificate')


def _report_status(engine_class, count: int, errors: str, success: bool) -> None:
    lines = [line.strip() for line in errors.splitlines()
             if line.strip() and not line.startswith('Got past Cloudflare')]
    if not success and lines:
        message = lines[-1]  # the exception itself
    else:
        message = next((line for line in lines if not line.startswith(('Traceback', 'File '))), '')
    lowered = '\n'.join(lines).lower()
    if count > 0:
        state = 'ok'
    elif not success:
        state = 'error'
    elif not lines:
        state = 'empty'
    elif any(hint in lowered for hint in _BLOCKED_HINTS):
        state = 'blocked'
    elif any(hint in lowered for hint in _DOWN_HINTS):
        state = 'down'
    else:
        state = 'error'
    name = getattr(engine_class, 'name', engine_class.__name__)
    clean = ' '.join(message.split())[:180].replace('\t', ' ')
    sys.stderr.write(f"DIO-STATUS\t{engine_class.__name__}\t{name}\t{count}\t{state}\t{clean}\n")
    sys.stderr.flush()


if __name__ == "__main__":
    def main() -> int:
        # https://docs.python.org/3/library/sys.html#sys.exit
        class ExitCode(Enum):
            OK = 0
            AppError = 1
            ArgError = 2

        found_engines = list_engines()

        prog_name = sys.argv[0]
        prog_usage = (f"Usage: {prog_name} all|engine1[,engine2]* <category> <keywords>\n"
                      f"To list available engines: {prog_name} --capabilities [--names]\n"
                      f"Found engines: {','.join(found_engines)}")

        if "--capabilities" in sys.argv:
            if "--names" in sys.argv:
                print(",".join(e for e in found_engines if import_engine(e) is not None))
                return ExitCode.OK.value

            print(get_capabilities(found_engines))
            return ExitCode.OK.value
        elif len(sys.argv) < 4:
            print(prog_usage, file=sys.stderr)
            return ExitCode.ArgError.value

        # get unique engines
        engs = {arg.strip().lower() for arg in sys.argv[1].split(',')}
        engines = found_engines if 'all' in engs else [e for e in found_engines if e in engs]

        cat = sys.argv[2].lower()
        try:
            category = Category[cat]
        except KeyError:
            print(f"Invalid category: {cat}", file=sys.stderr)
            return ExitCode.ArgError.value

        what = urllib.parse.quote(' '.join(sys.argv[3:]))
        params = ((engine_class, what, category) for e in engines if (engine_class := import_engine(e)) is not None)

        search_success = False
        if THREADED:
            processes = max(min(len(engines), MAX_THREADS), 1)
            # cannot use `forkserver` as it will interfere with `helpers.enable_socks_proxy()`
            with MP.get_context("spawn").Pool(processes) as pool:
                search_success = all(pool.map(run_search, params))
        else:
            search_success = all(map(run_search, params))

        return ExitCode.OK.value if search_success else ExitCode.AppError.value

    sys.exit(main())
