"""The embedded-asset list lives in tools/embed_web.py, main/CMakeLists.txt and main/web.c; they must agree."""
import ast, os, re, sys
root = sys.argv[1]
def rd(p):
    with open(os.path.join(root, p), encoding="utf-8") as f: return f.read()
py = set(ast.literal_eval(re.search(r"^FILES = (\[.*?\])", rd("tools/embed_web.py"), re.S | re.M).group(1)))
cm = set(re.search(r"set\(WEB_NAMES ([^)]*)\)", rd("main/CMakeLists.txt")).group(1).split())
c = rd("main/web.c")
syms = set()
for m in re.findall(r"^ASSET\(.*$", c, re.M):
    for s in re.findall(r"ASSET\((\w+)\)", m):
        b = s[:-3] if s.endswith("_gz") else s
        stem, _, ext = b.rpartition("_")
        syms.add(stem + "." + ext)
paths = {"index.html" if p == "/" else p[len("/assets/"):] for p in re.findall(r'^\s*A\("([^"]+)"', c, re.M)}
base = lambda names: {n.split("/")[-1] for n in names}
bad = 0
for label, got, want in (("CMakeLists WEB_NAMES", cm, py), ("web.c ASSET() symbols", syms, base(py)), ("web.c ASSETS[] paths", paths, py)):
    if got != want:
        bad = 1
        print("FAIL", label, "differs from embed_web.py FILES:", sorted(got ^ want))
if not py: bad = 1; print("FAIL no assets found")
print(len(py), "assets")
sys.exit(bad)
