#!/usr/bin/env python3
"""Gzip the web/ assets into <out>/<name>.gz (deterministic: mtime 0) for EMBED_FILES.
usage: embed_web.py <web_dir> <out_dir>"""
import gzip, os, sys

# Keep in sync with WEB_NAMES in main/CMakeLists.txt and the ASSET()/ASSETS[] table in main/web.c
# (test/host/test_web_assets.py checks all three).
FILES = ["index.html", "setup.html", "app.js", "app.css", "i18n/ko.json", "i18n/en.json", "i18n/zh.json", "i18n/ja.json"]

def main():
    web, out = sys.argv[1], sys.argv[2]
    total = 0
    for rel in FILES:
        with open(os.path.join(web, rel), "rb") as f:
            raw = f.read()
        dst = os.path.join(out, rel + ".gz")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(gzip.compress(raw, 9, mtime=0))
        sz = os.path.getsize(dst)
        total += sz
        print(f"{rel}: {len(raw)} -> {sz}")
    print(f"total gz: {total}")

if __name__ == "__main__":
    main()
