"""Regression guard for the OTA arm button: the server gate uses the password state latched at boot
(ota_gate(s_pw_at_boot, ...)), so the SPA must key the arm button and its hint off /api/status's pw_at_boot, not the
live pw_set. Setting a password mid-boot then hides neither the button nor the hint, and OTA stays reachable."""
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

m = re.search(r"function otaBootPressNeeded\(\)\s*\{(.*?)\n\}", js, re.S)
if not m:
    fail("app.js has no otaBootPressNeeded() helper")
else:
    body = m.group(1)
    if not re.search(r"ST\.pw_at_boot === undefined \? !ST\.pw_set : !ST\.pw_at_boot", body):
        fail("otaBootPressNeeded() does not read pw_at_boot with a pw_set fallback for older firmware")
m = re.search(r"function otaCard\(view\)\s*\{(.*?)\n\}", js, re.S)
if not m:
    fail("otaCard() not found")
else:
    body = m.group(1)
    if not re.search(r"if \(otaBootPressNeeded\(\)\) \{", body):
        fail("otaCard() does not gate the arm button on otaBootPressNeeded()")
    if re.search(r"!\s*ST\.pw_set", body):
        fail("otaCard() still gates the arm button on the live pw_set")
    arm = re.search(r"if \(otaBootPressNeeded\(\)\) \{.*?\n  \}", body, re.S)
    if not arm:
        fail("otaCard() arm block not found")
    elif not re.search(r"t\('settings\.ota_arm_hint'\)", arm.group(0)):
        fail("the arm hint is not inside the same block as the arm button")
print("ota arm boot-time password gate ok" if not bad else "ota arm boot-time password gate broken")
sys.exit(1 if bad else 0)
