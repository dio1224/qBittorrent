import io, os, sys, time, contextlib, datetime, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
stub = os.path.expanduser("~/.qbt-cf/teststub")
os.makedirs(stub, exist_ok=True)
with open(os.path.join(stub, "novaprinter.py"), "w") as f:
    f.write("RESULTS = []\ndef prettyPrinter(d):\n    RESULTS.append(d)\n")
sys.path[:0] = [stub, HERE]
import novaprinter, leetx, exttorrents

print("Python used for this test:", sys.executable, sys.version.split()[0])
try:
    v = subprocess.run(["defaults", "read", "/Applications/qBittorrent.app/Contents/Info.plist", "CFBundleShortVersionString"], capture_output=True, text=True).stdout.strip()
    print("qBittorrent version:", v or "not found in /Applications")
except Exception:
    pass
engines = os.path.expanduser("~/Library/Application Support/qBittorrent/nova3/engines")
print("qBittorrent plugin folder exists:", os.path.isdir(engines))

all_ok = True
for cls, query, cat in ((leetx.leetx, "ubuntu", "all"), (leetx.leetx, "ubuntu", "software"),
                        (exttorrents.exttorrents, "ubuntu", "all"), (exttorrents.exttorrents, "ubuntu", "software")):
    novaprinter.RESULTS.clear()
    t0 = time.time()
    cls().search(query, cat)
    res = list(novaprinter.RESULTS)
    print("\n%s | '%s' in %s: %d results in %.0fs" % (cls.name, query, cat, len(res), time.time() - t0))
    if not res:
        all_ok = False
        continue
    for r in res[:3]:
        d = datetime.date.fromtimestamp(r["pub_date"]) if r["pub_date"] > 0 else "?"
        print("   %-55s %8.1f MB  seeds %-5s %s" % (r["name"][:55], r["size"] / 1048576, r["seeds"], d))
    if cat == "all":
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            cls().download_torrent(res[0]["link"])
        out = buf.getvalue().strip()
        ok = out.startswith("magnet:?xt=urn:btih:")
        all_ok = all_ok and ok
        print("   magnet for first result:", "OK " + out[:60] + "..." if ok else "MISSING")
print("\nALL TESTS PASSED" if all_ok else "\nSOME TESTS FAILED")
sys.exit(0 if all_ok else 1)
