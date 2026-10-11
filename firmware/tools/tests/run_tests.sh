#!/usr/bin/env bash
# Host-side checks for the tuning dial. These need no device: they extract the
# frequency parser straight out of the firmware source and assert against real
# station names collected from the public directory.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - "$root/main/radio_catalog.cc" "$work/freq.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()
start = text.index('// Chinese FM broadcasting occupies')
end = text.index('void make_display_name(')
open(dst, 'w', encoding='utf-8').write(
    '#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace freq {\n'
    + text[start:end] + '\n}\n')
PY

c++ -std=c++17 -O1 -I "$work" -o "$work/freq" "$here/test_frequency_parsing.cc"
"$work/freq"
c++ -std=c++17 -O1 -o "$work/dial" "$here/test_dial_mapping.cc"
"$work/dial"
