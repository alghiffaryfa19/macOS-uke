#!/usr/bin/env python3
"""Write the three generated headers macOS-ForSheng's prepare() stages into the xnu tree
(panic QR encoder, Recovery UI page table, language trace table), from its pinned data files."""
import json
import sys
from pathlib import Path

FS = Path(sys.argv[1])
SRC = Path(sys.argv[2])


def hexarray(value):
    return '{' + ', '.join(f'0x{x:02x}' for x in bytes.fromhex(value)) + '}'


def pages(document):
    lines = ['// SPDX-License-Identifier: MIT', '// Generated from the pinned Recovery UI page manifest.',
             '#pragma once', 'static const struct sheng_ui_page sheng_ui_pages[] = {']
    for page in document['pages']:
        lines += ['    {', f'        .file = "{page["file"]}",',
                  f'        .file_offset = {page["file_offset"]}ULL,',
                  f'        .before_sha256 = {hexarray(page["before_sha256"])},',
                  f'        .after_sha256 = {hexarray(page["after_sha256"])},',
                  f'        .edit_count = {len(page["edits"])},', '        .edits = {']
        for edit in page['edits']:
            lines.append('            {' + f'.offset = {edit["offset"]}, .size = {len(bytes.fromhex(edit["before"]))}, '
                         + f'.before = {hexarray(edit["before"])}, .after = {hexarray(edit["after"])}' + '},')
        lines += ['        },', '    },']
    return '\n'.join(lines + ['};', ''])


def language(document):
    out = '#ifdef SHENG_LANGUAGE_TRACE_PAGE_TABLE\n'
    out += pages(document).replace('sheng_ui_pages[]', 'sheng_language_trace_pages[]')
    out += '#else\nstatic const struct sheng_language_point sheng_language_points[] = {\n'
    for p in document['points']:
        out += ('    {' + f'.rva = 0x{p["rva"]:x}, .tag = 0x{p["tag"]:x}, .destination = {p["destination"]}, '
                + f'.branch = {int(p["replay"] == "cbnz_x0")}, .name = "{p["name"]}"' + '},\n')
    out += '};\n'
    out += f'#define SHENG_LANGUAGE_UUID_OFFSET {document["uuid_command_offset"]}u\n'
    out += 'static const uint8_t sheng_language_uuid_command[24] = {'
    out += ', '.join(f'0x{x:02x}' for x in bytes.fromhex(document['uuid_command'])) + '};\n'
    return out + '#endif\n'


def qr():
    source = (FS / 'platform/sheng/macos26/log/qr/qrcodegen.c').read_text()
    includes = '#include <assert.h>\n#include <limits.h>\n#include <stdlib.h>\n#include <string.h>\n'
    assert source.count(includes) == 1
    prefix = ('/* Freestanding adaptation; the unmodified source and MIT notice are retained. */\n'
              '#include "pe_sheng_qr_runtime.h"\n#pragma push_macro("assert")\n#undef assert\n'
              '#define assert(condition) ((void)sizeof(condition))\n#define memset sheng_qr_memset\n'
              '#define memcpy sheng_qr_memcpy\n#define memmove sheng_qr_memmove\n#define strlen sheng_qr_strlen\n'
              '#define strchr sheng_qr_strchr\n#define abs(x) ((x) < 0 ? -(x) : (x))\n'
              '#define labs(x) ((x) < 0 ? -(x) : (x))\n')
    suffix = ('\n#undef labs\n#undef abs\n#undef strchr\n#undef strlen\n#undef memmove\n#undef memcpy\n'
              '#undef memset\n#pragma pop_macro("assert")\n')
    return source.replace(includes, prefix).replace('LONG_MAX', '__LONG_MAX__') + suffix


(SRC / 'pexpert/arm/pe_sheng_qr_encoder.h').write_text(qr())
(SRC / 'osfmk/vm/sheng_recovery_ui_pages.h').write_text(
    pages(json.loads((FS / 'config/macos26-recovery-ui-pages.json').read_text())))
(SRC / 'osfmk/vm/sheng_language_trace_pages.h').write_text(
    language(json.loads((FS / 'config/macos26-language-trace.json').read_text())))
print('generated 3 headers')
