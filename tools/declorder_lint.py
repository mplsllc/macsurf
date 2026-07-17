#!/usr/bin/env python3
"""Declaration-order lint for C89 / CW8.

Catches: a static function CALLED before it is DEFINED, with no forward
declaration in between.

C89 permits an implicit declaration, so the first call silently invents
`int f()`. The real definition then conflicts, and CW8 reports it as:

    Error: identifier 'foo(...)' redeclared
           was declared as: 'int (...)'
           now declared as: 'void (struct gui_window *, float, int)'

followed by a cascade of "undefined identifier" for every parameter, because
CW8 discards the parameter list it could not reconcile.

WHY THIS EXISTS AS A SEPARATE TOOL: gcc in C89 mode accepts the implicit
declaration, so the Retro68 pre-flight does not flag it -- and for the files
where this bites most (main.c, macos9_download.c) Retro68 cannot compile at all,
because its multiversal header set lacks OpenTransport.h / Script.h. Those files
have NO Linux compile coverage whatsoever, so a text-level check is the only
pre-CW8 signal available for them.

Deliberately conservative: it only looks at `static` functions defined in the
same file, and only flags a call that appears textually before the definition
with no earlier declaration. Comments and strings are stripped first so that a
mention inside a comment is not mistaken for a call.

Usage: tools/declorder_lint.py <file.c> [...]
Exit 0 = clean, 1 = findings.
"""
import re
import sys


def strip_comments_and_strings(src):
    """Blank out comments and string/char literals, preserving line structure."""
    out = []
    i = 0
    n = len(src)
    state = None  # None | 'line' | 'block' | 'str' | 'chr'
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if state is None:
            if c == '/' and nxt == '/':
                state = 'line'; out.append('  '); i += 2; continue
            if c == '/' and nxt == '*':
                state = 'block'; out.append('  '); i += 2; continue
            if c == '"':
                state = 'str'; out.append(' '); i += 1; continue
            if c == "'":
                state = 'chr'; out.append(' '); i += 1; continue
            out.append(c); i += 1; continue
        # inside something we are blanking
        if state == 'line':
            if c == '\n':
                state = None; out.append('\n')
            else:
                out.append(' ')
            i += 1; continue
        if state == 'block':
            if c == '*' and nxt == '/':
                state = None; out.append('  '); i += 2; continue
            out.append('\n' if c == '\n' else ' '); i += 1; continue
        if state in ('str', 'chr'):
            if c == '\\':
                out.append('  '); i += 2; continue
            if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
                state = None
            out.append('\n' if c == '\n' else ' ')
            i += 1; continue
    return ''.join(out)


# NOTE: `\s*\{` not `[ \t]*\{`. MacSurf's house style puts the opening brace on
# its OWN line for top-level functions, so requiring brace-on-same-line made this
# lint match nothing and report every file clean -- it failed its own negative
# control on the very bug it was written for.
DEF_RE = re.compile(
    r'^[ \t]*static[ \t]+(?:[A-Za-z_][A-Za-z0-9_ \t\*]*?)\b([A-Za-z_][A-Za-z0-9_]*)'
    r'[ \t]*\([^;]*?\)\s*\{', re.M | re.S)
DECL_RE = re.compile(
    r'^[ \t]*(?:static|extern)?[ \t]*[A-Za-z_][A-Za-z0-9_ \t\*]*?\b'
    r'([A-Za-z_][A-Za-z0-9_]*)[ \t]*\([^;{]*\)[ \t]*;', re.M | re.S)

KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'defined'}


def line_of(src, pos):
    return src.count('\n', 0, pos) + 1


def check(path):
    raw = open(path, errors='replace').read()
    src = strip_comments_and_strings(raw)

    # static functions defined in this file: name -> line of definition
    defs = {}
    for m in DEF_RE.finditer(src):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        defs.setdefault(name, line_of(src, m.start()))

    # any prototype (a declaration ending in ';') -> earliest line
    decls = {}
    for m in DECL_RE.finditer(src):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        ln = line_of(src, m.start())
        if name not in decls or ln < decls[name]:
            decls[name] = ln

    findings = []
    for name, defline in defs.items():
        declline = decls.get(name)
        if declline is not None and declline < defline:
            continue  # properly forward-declared
        # earliest call site
        for m in re.finditer(r'\b' + re.escape(name) + r'[ \t]*\(', src):
            ln = line_of(src, m.start())
            if ln >= defline:
                break
            # skip the definition itself and any prototype
            findings.append((ln, name, defline))
            break

    for ln, name, defline in sorted(findings):
        print("%s:%d: '%s' is CALLED here but not DEFINED until line %d, "
              "and has no forward declaration."
              % (path, ln, name, defline))
        print("    C89 will implicitly declare it `int %s()` at this call, and "
              "CW8 will then reject the real" % name)
        print("    definition with \"identifier '%s(...)' redeclared / was "
              "declared as: 'int (...)'\"." % name)
        print("    Fix: add a forward declaration above line %d." % ln)
    return len(findings)


def main():
    if len(sys.argv) < 2:
        print("usage: %s <file.c> [...]" % sys.argv[0], file=sys.stderr)
        return 2
    total = 0
    for p in sys.argv[1:]:
        n = check(p)
        total += n
        if n == 0:
            print("decl-order clean: %s" % p)
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
