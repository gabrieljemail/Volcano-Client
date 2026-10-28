#!/usr/bin/env python3
"""Shortens `//` comments to the part before a dash.

    // Short useful comment - then a long tail that drifted out of date
    // continuing on more lines.

becomes

    // Short useful comment

The tail after the dash is dropped, and so are the `//` lines that continue the same comment block. Works on `//`
comments only (full-line or trailing); `/* */` blocks, `///` doc lines, URLs and code are left alone.

    python tools/trim_comments.py            # dry run: prints what would change
    python tools/trim_comments.py --apply    # rewrites the files
    python tools/trim_comments.py --apply src/renderer   # only under a path
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTENSIONS = {'.cpp', '.hpp', '.h', '.c', '.cc'}

# An em/en dash, or a hyphen with a space either side, inside a // comment.
SEPARATOR = re.compile(r'\s+(?:—|–|-)\s+|\s*—\s*|\s*–\s*')
LINE_COMMENT = re.compile(r'^(\s*)//(?!/)(\s?)(.*)$')


def split_trailing(line):
    """Returns (code, comment_text) for a line with a trailing // comment, else None. Skips // inside strings."""
    in_string = None
    i = 0
    while i < len(line) - 1:
        c = line[i]
        if in_string:
            if c == '\\':
                i += 2
                continue
            if c == in_string:
                in_string = None
        elif c in '"\'':
            in_string = c
        elif c == '/' and line[i + 1] == '/' and (i + 2 >= len(line) or line[i + 2] != '/'):
            return line[:i], line[i + 2:]
        i += 1
    return None


def trim_text(text):
    """The comment text up to its first separator, or None if there is nothing to trim."""
    if 'http://' in text or 'https://' in text or 'NOLINT' in text:
        return None
    for match in SEPARATOR.finditer(text):
        before, after = text[:match.start()], text[match.end():]
        if len(before.strip()) < 8 or len(after.strip()) < 3:
            continue  # "- item" bullets, short math, "x - y"
        em = '—' in match.group(0) or '–' in match.group(0)
        if not em:
            # A hyphen is only a separator between words of prose, not in an expression.
            if re.search(r'[=(<>*/+\[{]\s*\S*$', before) or re.match(r'^\s*[\d(=*/+\[]', after):
                continue
            if before.count('(') != before.count(')'):
                continue
        return before.rstrip().rstrip(',;:')
    return None


def process(lines):
    out = []
    changed = []
    i = 0
    while i < len(lines):
        line = lines[i]
        raw = line.rstrip('\n')
        newline = line[len(raw):]

        full = LINE_COMMENT.match(raw)
        if full and not raw.rstrip().endswith('\\'):
            # A block of consecutive full-line comments at the same indent.
            indent = full.group(1)
            j = i
            block = []
            while j < len(lines):
                m = LINE_COMMENT.match(lines[j].rstrip('\n'))
                if not m or m.group(1) != indent:
                    break
                block.append((lines[j], m))
                j += 1
            cut = None
            for k, (_, m) in enumerate(block):
                trimmed = trim_text(m.group(3))
                if trimmed is not None:
                    cut = (k, trimmed)
                    break
            if cut is None:
                out.extend(b[0] for b in block)
            else:
                k, trimmed = cut
                out.extend(b[0] for b in block[:k])
                kept = f'{indent}// {trimmed}' + (newline or '\n')
                out.append(kept)
                changed.append((i + 1 + k, block[k][0].rstrip('\n'), kept.rstrip('\n'), len(block) - k - 1))
            i = j
            continue

        trailing = split_trailing(raw)
        if trailing and not raw.rstrip().endswith('\\'):
            code, text = trailing
            trimmed = trim_text(text.strip())
            if trimmed is not None and code.strip():
                new = f'{code}// {trimmed}'
                out.append(new + newline)
                changed.append((i + 1, raw, new, 0))
                i += 1
                continue
        out.append(line)
        i += 1
    return out, changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    apply = '--apply' in sys.argv
    base = ROOT / (args[0] if args else 'src')
    files = [base] if base.is_file() else sorted(p for p in base.rglob('*') if p.suffix in EXTENSIONS)

    total_files = total_lines = total_dropped = 0
    for path in files:
        data = path.read_bytes()
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError:
            continue
        lines = text.splitlines(keepends=True)
        out, changed = process(lines)
        if not changed:
            continue
        total_files += 1
        total_lines += len(changed)
        total_dropped += len(lines) - len(out)
        if apply:
            path.write_bytes(''.join(out).encode('utf-8'))
        elif '--quiet' not in sys.argv:
            print(f'{path.relative_to(ROOT)}')
            for number, before, after, dropped in changed[:6]:
                print(f'  {number}: {before.strip()}\n   -> {after.strip()}' + (f'   (+{dropped} lines dropped)' if dropped else ''))
    verb = 'rewrote' if apply else 'would change'
    print(f'\n{verb} {total_lines} comments in {total_files} files, removing {total_dropped} lines.')


if __name__ == '__main__':
    main()
