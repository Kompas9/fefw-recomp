#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Adapt only Lichii key-file IO for the extraction helper, leaving crypto unchanged."""
import sys
from pathlib import Path

def replace_function(text, signature, replacement):
    if text.count(signature) != 1:
        raise RuntimeError('Lichii key manager changed; review the helper IO adapter: ' + signature)
    start = text.index(signature)
    brace = text.index('{', start)
    # The pinned functions have balanced braces (including fmt placeholders).
    depth, end = 1, brace + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[:start] + replacement + text[end:]

def adapt(text):
    declaration = '#include "core/loader/loader.h"'
    if text.count(declaration) != 1: raise RuntimeError('Lichii key manager includes changed')
    text = text.replace(declaration, declaration + '\n\n// Provided only by the extraction frontend.\nextern std::filesystem::path FefwArchiveKeyFile();')
    text = replace_function(text, 'void KeyManager::ReloadKeys()', r'''void KeyManager::ReloadKeys() {
    // Explicit user-selected files only: never read an emulator profile or auto-generated keys.
    dev_mode = false;
    const auto file = ::FefwArchiveKeyFile();
    LoadFromFile(file, false);
    LoadFromFile(file.parent_path() / "title.keys", true);
}''')
    return replace_function(text, 'void KeyManager::WriteKeyToFile(', r'''void KeyManager::WriteKeyToFile(KeyCategory, std::string_view, const std::array<u8, Size>&) {
    // Ticket/derived keys stay in memory for this extraction process; do not persist them.
}''')

if __name__ == '__main__':
    Path(sys.argv[2]).write_text(adapt(Path(sys.argv[1]).read_text(encoding='utf-8')), encoding='utf-8')
