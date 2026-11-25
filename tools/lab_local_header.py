#!/usr/bin/env python3
#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
"""Turn lab_local.conf (sdkconfig syntax) into a C header for the relay firmware.

Usage: lab_local_header.py <lab_local.conf> <output.h>

Every CONFIG_LAB_* line becomes a #define of the same name. common/include/halow_lab_config.h
adopts CONFIG_LAB_<NAME> as LAB_<NAME>, exactly as it does for ESP-IDF's sdkconfig.h, so the relay
gets the same values as node A and node B. Booleans: =y becomes 1, =n is left out. The output is
only rewritten when its content changes, so an unchanged file does not trigger a rebuild.
"""

import re
import sys

LINE = re.compile(r'^\s*(CONFIG_LAB_[A-Z0-9_]+)\s*=\s*(.*?)\s*$')


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    src, out = argv[1], argv[2]

    defines = []
    with open(src, encoding='utf-8') as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            m = LINE.match(line)
            if not m:
                if line.startswith('CONFIG_'):
                    continue  # ESP-IDF only option, not used by the relay
                print(f'{src}:{lineno}: not a CONFIG_NAME=value line: {line}', file=sys.stderr)
                return 1
            name, value = m.groups()
            if value == 'n':
                continue
            if value == 'y':
                value = '1'
            elif not (value.startswith('"') and value.endswith('"')) and \
                    not re.fullmatch(r'-?(0x[0-9a-fA-F]+|[0-9]+)', value):
                print(f'{src}:{lineno}: {name}: value must be a "string", a number, y or n',
                      file=sys.stderr)
                return 1
            defines.append(f'#define {name} {value}')

    text = '\n'.join([
        '/* Generated from lab_local.conf by tools/lab_local_header.py. Do not edit. */',
        '#ifndef LAB_LOCAL_CONFIG_H',
        '#define LAB_LOCAL_CONFIG_H',
        *defines,
        '#endif',
        '',
    ])
    try:
        with open(out, encoding='utf-8') as f:
            if f.read() == text:
                return 0
    except FileNotFoundError:
        pass
    with open(out, 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
