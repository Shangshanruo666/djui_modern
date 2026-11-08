#!/usr/bin/env python3
"""Generate the embedded DJUI MODERN language table from lang/*.ini.

Usage:
    python src/pc/djui_modern/tools/make_lang_c.py \
        src/pc/djui_modern/lang src/pc/djui_modern/djui_modern_lang_data.h
"""

import configparser
import os
import sys


def c_string(value):
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    return '"%s"' % value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


def main():
    if len(sys.argv) != 3:
        print("usage: make_lang_c.py <lang-dir> <output-header>", file=sys.stderr)
        return 1

    lang_dir, output = sys.argv[1], sys.argv[2]
    files = sorted(name for name in os.listdir(lang_dir) if name.endswith(".ini"))
    tables = []

    for filename in files:
        parser = configparser.ConfigParser(interpolation=None)
        parser.optionxform = str
        parser.read(os.path.join(lang_dir, filename), encoding="utf-8")
        if not parser.has_section("DJUI_MODERN"):
            continue
        language = os.path.splitext(filename)[0]
        entries = list(parser.items("DJUI_MODERN"))
        table = "sLang_%s" % language
        text = []
        text.append("static const struct DjuiModernLangEntry %s[] = {" % table)
        for key, value in entries:
            text.append("    { %s, %s }," % (c_string(key), c_string(value)))
        text.append("};")
        text.append("")
        tables.append((language, table, len(entries), text))

    with open(output, "w", encoding="utf-8", newline="\n") as out:
        out.write("// 自动生成，请勿手工编辑。\n")
        out.write("#pragma once\n\n")
        out.write("struct DjuiModernLangEntry { const char* key; const char* value; };\n\n")
        for language, table, count, text in tables:
            out.write("\n".join(text))
            out.write("\n")
        out.write("struct DjuiModernLangTable { const char* name; const struct DjuiModernLangEntry* entries; unsigned int count; };\n\n")
        out.write("static const struct DjuiModernLangTable sDjuiModernLangTables[] = {\n")
        for language, table, count, _ in tables:
            out.write("    { %s, %s, %du },\n" % (c_string(language), table, count))
        out.write("};\n")

    print("wrote %s (%d languages)" % (output, len(tables)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
