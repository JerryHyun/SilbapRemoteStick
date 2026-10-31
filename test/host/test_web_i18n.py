import json, os, re, sys
web = sys.argv[1]
def load(l):
    with open(os.path.join(web, "i18n", l + ".json"), encoding="utf-8") as f:
        return json.load(f)
en = load("en")
bad = 0
def fail(m):
    global bad
    bad += 1
    print("FAIL", m)
for l in ("ko", "zh", "ja"):
    d = load(l)
    for k in en:
        if k not in d: fail(f"{l}.json lacks key {k}")
    for k in d:
        if k not in en: fail(f"{l}.json has key {k} not in en.json")
def rd(n):
    with open(os.path.join(web, n), encoding="utf-8") as f: return f.read()
html, js = rd("index.html"), rd("app.js")
setup = rd("setup.html")
used = set(re.findall(r'data-i18n(?:-[a-z]+)?="([^"]+)"', html)) | set(re.findall(r"""\bt\(\s*['"]([^'"]+)['"]""", js))
# The setup portal page uses data-i="key" and its own t() wrapper; both must resolve in every language too.
used |= set(re.findall(r'data-i="([^"]+)"', setup)) | set(re.findall(r"""\bt\(\s*['"]([^'"]+)['"]""", setup))
if not used: fail("no i18n keys found in index.html/app.js/setup.html")
for k in sorted(used):
    if k not in en: fail(f"key {k} used but missing from en.json")
    if not k.startswith("setup."): continue
    for l in ("ko", "zh", "ja"):
        if k not in load(l): fail(f"{l}.json lacks setup key {k}")
# Keys that reach t()/showMsg() through a variable or a ternary: every string literal in app.js that looks like a
# key of a known family ("err.io", "settings.saved", ...) must exist in en.json (catches typos in those paths).
families = {k.split(".")[0] for k in en if "." in k}
for lit in sorted(set(re.findall(r"""['"]([a-z_]+\.[a-z_0-9]+)['"]""", js + "\n" + setup))):
    if lit.split(".")[0] in families and lit not in en:
        fail(f"app.js/setup.html uses key {lit} that is missing from en.json")
# The SPA must not make any external request: no absolute URL anywhere in the web assets.
URL_RE = r"""(?:https?:)?//[a-z0-9.-]+\.[a-z]{2,}[^\s'"`)]*"""
for n in ("index.html", "app.js", "app.css", "setup.html"):
    for u in re.findall(URL_RE, rd(n), re.I):
        fail(f"{n} contains an external URL: {u}")
print(f"{len(en)} keys, {len(used)} used")
sys.exit(1 if bad else 0)
