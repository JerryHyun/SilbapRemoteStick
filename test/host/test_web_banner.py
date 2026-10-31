"""Regression guard for the setup-applied banner: /api/status returns setup_applied only on its first read, and the
boot sequence reads /api/status twice (syncClock() posts /api/time and refreshes). The SPA must latch the flag for the
page load, otherwise renderBanners() clears the banner on the second response."""
import os, re, sys
root = sys.argv[1]
def rd(p):
    with open(os.path.join(root, p), encoding="utf-8") as f: return f.read()
js = rd("web/app.js")
bad = 0
def fail(m):
    global bad
    bad += 1
    print("FAIL", m)

if not re.search(r"^var setupAppliedOnce = false;", js, re.M):
    fail("app.js has no module-level setupAppliedOnce latch")
m = re.search(r"function refreshStatus\(\)\s*\{(.*?)\n\}", js, re.S)
if not m:
    fail("refreshStatus() not found")
elif not re.search(r"if \(s\.setup_applied\)\s*setupAppliedOnce = true;", m.group(1)):
    fail("refreshStatus() does not latch setup_applied")
m = re.search(r"function renderBanners\(\)\s*\{(.*?)\n\}", js, re.S)
if not m:
    fail("renderBanners() not found")
else:
    body = m.group(1)
    if not re.search(r"if \(setupAppliedOnce\) banner\(box, t\('banner\.setup_applied'\)\);", body):
        fail("renderBanners() does not render from the latch")
    if re.search(r"ST\.setup_applied", body):
        fail("renderBanners() still renders the per-response setup_applied field")
print("setup_applied latch ok" if not bad else "setup_applied latch broken")
sys.exit(1 if bad else 0)
