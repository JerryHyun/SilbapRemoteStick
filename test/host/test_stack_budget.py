"""Fails when a main/*.c function puts a >= 1 KiB char array on the stack.

Why: every HTTP handler runs on the esp_http_server task, whose stack is 8192 bytes
(main/web.c, `c.stack_size`). POST /api/upload used to hold 7.5 KiB of locals there (a
4096-byte receive chunk plus paths and a reply buffer) and the response was written from a
smashed frame: the file was stored but the connection died (http=000), so the SPA retried and
the card ended up with "name.dst" and "name (2).dst". Real stack use can only be measured on
the target; this check catches the cheap, common mistake -- a big array declared as an
ordinary local instead of `static` (safe here: one request at a time) or malloc'd (webdav.c's
dav_t).

What it does NOT catch:
  * arrays under 1024 bytes, or of types other than char (int / uint8_t scratch is invisible);
  * totals: ten 900-byte locals in one function sum to 9 KiB and still pass;
  * struct members (webdav.c's dav_t is a struct, deliberately heap-allocated);
  * callee frames on top of the handler (httpd_resp_send, esp_log, FatFs ...);
  * the other tasks (refresh 8192, dns53 4096) -- only the httpd stack matters here.

Sizes are resolved from integer literals and `#define NAME <expression>` in main/ or a component
include dir, and simple arithmetic of those. A `sizeof`, a pointer cast or a length taken from a
variable is reported as "unresolved (skipped)" and never fails the build, so a macro this check
cannot evaluate does not block a legitimate change.

Usage: test_stack_budget.py <repo root>
"""

import os
import re
import sys

LIMIT = 1024  # bytes
EXPR = r"[A-Za-z_0-9\s+*()-]+"   # literals, identifiers, simple arithmetic
DECL = re.compile(r"\bchar\s+([A-Za-z_]\w*)\s*\[\s*([^\]]*?)\s*\]")


def strip_comments(text):
    """Blanks out comments but keeps line numbers (block comments become newlines)."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def sources(root):
    """(path, is_c_file) for the .c/.h files whose #defines can size a local, plus every main/*.c."""
    out = [(os.path.join(root, "main", n), True) for n in sorted(os.listdir(os.path.join(root, "main")))
           if n.endswith(".c")]
    for n in sorted(os.listdir(os.path.join(root, "main"))):
        if n.endswith(".h"):
            out.append((os.path.join(root, "main", n), False))
    comp = os.path.join(root, "components")
    if os.path.isdir(comp):
        for c in sorted(os.listdir(comp)):
            for sub in ("include", "src"):
                d = os.path.join(comp, c, sub)
                if not os.path.isdir(d):
                    continue
                for n in sorted(os.listdir(d)):
                    if n.endswith(".h"):
                        out.append((os.path.join(d, n), False))
    managed = os.path.join(root, "managed_components")   # e.g. mdns' MDNS_NAME_BUF_LEN
    if os.path.isdir(managed):
        for c in sorted(os.listdir(managed)):
            for sub in ("include", "include/mdns"):
                d = os.path.join(managed, c, sub)
                if not os.path.isdir(d):
                    continue
                for n in sorted(os.listdir(d)):
                    if n.endswith(".h"):
                        out.append((os.path.join(d, n), False))
    return out


def size_of(expr, syms):
    e = expr.strip()
    if not e:
        return None  # char name[] = "..." : sized by the initialiser
    e = re.sub(r"\b(\d+)[uU]\b", r"\1", e)   # 32u
    if re.fullmatch(r"\d+", e):
        return int(e)
    if not re.fullmatch(EXPR, e):
        return None  # sizeof, casts, commas, ...
    try:  # digits, known names and + * ( ) only
        val = eval(e, {"__builtins__": {}}, dict(syms))  # noqa: S307 - no calls reachable
    except Exception:
        return None
    return int(val) if isinstance(val, int) else None


def aggregate_mask(text):
    """text[i] is True when offset i sits inside a struct/union body (a member is not a stack local)."""
    mask = bytearray(len(text))
    depth = 0
    open_at = []          # depths at which an aggregate body is open
    pending = False       # saw "struct"/"union" and no "{" / ";" yet: the next "{" opens a body
    i = 0
    while i < len(text):
        if text.startswith("struct", i) or text.startswith("union", i):
            pending = True
        ch = text[i]
        if ch == "{":
            depth += 1
            if pending:
                open_at.append(depth)
                pending = False
        elif ch == "}":
            if open_at and open_at[-1] == depth:
                open_at.pop()
            depth -= 1
        elif ch == ";":
            pending = False
        if open_at:
            mask[i] = 1
        i += 1
    return mask


def stmt_start(text, pos):
    """Offset of the start of the declaration statement containing pos (back to ';', '{' or '}')."""
    i = pos
    while i > 0 and text[i - 1] not in ";{}":
        i -= 1
    return i


def main(root):
    files = sources(root)
    raw = {}
    for path, _ in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = strip_comments(f.read())
        for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*$", text, re.M):
            if re.fullmatch(EXPR, m.group(2)):
                raw[m.group(1)] = m.group(2)
    syms = {}
    for _ in range(4):                      # a define may name a later one; four passes settles nesting
        for k, v in raw.items():
            n = size_of(v, syms)
            if n is not None:
                syms[k] = n

    offenders, unresolved = [], []
    for path, _ in files:
        if not path.endswith(".c"):
            continue
        with open(path, encoding="utf-8") as f:
            text = strip_comments(f.read())
        members = aggregate_mask(text)
        for m in DECL.finditer(text):
            if re.search(r"\b(static|extern)\b", text[stmt_start(text, m.start()):m.start()]):
                continue                    # not on the stack
            if members[m.start()]:
                continue                    # a struct/union member
            lineno = text.count("\n", 0, m.start()) + 1
            n = size_of(m.group(2), syms)
            if n is None:
                if m.group(2).strip():
                    unresolved.append("%s:%d: char %s[%s]" % (path, lineno, m.group(1), m.group(2)))
                continue
            if n >= LIMIT:
                offenders.append("%s:%d: char %s[%s] = %d bytes on the stack"
                                 % (path, lineno, m.group(1), m.group(2), n))
    for u in unresolved:
        print("note (size not resolved, skipped):", u)
    if offenders:
        print("FAIL large stack arrays (the esp_http_server task has 8192 bytes):")
        for o in offenders:
            print("  ", o)
        print("Make them static (one request at a time, say so in a comment) or malloc them.")
        return 1
    print("ok: no char array >= %d bytes on the stack in main/*.c" % LIMIT)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
