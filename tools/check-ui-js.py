#!/usr/bin/env python3
"""Parse the web UI's inline JavaScript straight out of the built firmware.

Why this exists
---------------
The whole UI is ONE inline <script> assembled from C string literals in
web_server.c. The C compiler cannot see inside those strings, so a JavaScript
syntax error builds perfectly, flashes perfectly, and then kills EVERY handler
on the page — tabs go dead while the live <img src=/stream> keeps working,
because that is HTML and not JS.

Parsing the SERVED page catches it, but only after flashing a broken image to
the device. The page is a string constant in the image, so it can be parsed
from build/BirdBox.bin instead and the whole round trip skipped. Run this
between `idf.py build` and the OTA upload.

Caught in practice: a duplicated `drowi()` tail left behind by an edit that
replaced two lines of a three-line function (0.96.0). That reached the device
before anyone noticed.

Usage
-----
    python tools/check-ui-js.py [build/BirdBox.bin]

Exit code 0 = the script parses. Non-zero = do not flash.
"""
import sys
import re

try:
    import esprima
except ImportError:
    sys.exit("need esprima:  pip install esprima")


def extract_scripts(blob: bytes):
    """Every <script>...</script> body in the image, as text.

    The image holds the page as one long NUL-terminated C string, so the
    markers are plain ASCII and can be found without decoding the whole blob.
    """
    out = []
    for m in re.finditer(rb'<script[^>]*>', blob):
        end = blob.find(b'</script>', m.end())
        if end < 0:
            continue
        out.append(blob[m.end():end].decode('utf-8', 'replace'))
    return out


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/BirdBox.bin'
    try:
        blob = open(path, 'rb').read()
    except OSError as e:
        print('cannot read %s: %s' % (path, e))
        return 2

    scripts = extract_scripts(blob)
    if not scripts:
        print('FAIL: no <script> block found in %s' % path)
        print('      (wrong file, or the UI is no longer inline)')
        return 2

    bad = 0
    for i, src in enumerate(scripts):
        try:
            esprima.parseScript(src)
            print('block %d: %6d chars  OK' % (i, len(src)))
        except Exception as e:                      # esprima raises its own type
            bad += 1
            print('block %d: %6d chars  *** SYNTAX ERROR: %s' % (i, len(src), e))
            line = getattr(e, 'lineNumber', None)
            col = getattr(e, 'column', None)
            # The script is effectively one line, so a line number is useless;
            # show the text around the column instead.
            if col:
                lo, hi = max(0, col - 220), min(len(src), col + 220)
                print()
                print('  ...%s...' % src[lo:hi].replace('\n', ' '))
                print('     %s^ here (col %d)' % (' ' * min(220, col - lo), col))
            elif line:
                print('  line %d' % line)

    print()
    if bad:
        print('%d block(s) FAILED — do NOT flash this image.' % bad)
        print('A JS syntax error disables every handler on the page.')
        return 1

    print('all %d block(s) parse. Safe to flash.' % len(scripts))
    print()
    print('NOTE: parsing proves SYNTAX, not behaviour. It will happily accept')
    print("getAttribute('href ') or a handler that references an undefined name.")
    print('For a behavioural change, also assert the emitted token and exercise')
    print('the endpoint it calls.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
