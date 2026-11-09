# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
import prepare_reader

FIXTURE = '''#include "core/loader/loader.h"
void KeyManager::ReloadKeys() { ReadEmulatorProfile(); }
int UnchangedCrypto(int value) { return value ^ 0x55; }
template <size_t Size>
void KeyManager::WriteKeyToFile(KeyCategory c, std::string_view n, const std::array<u8, Size>& k) { PersistKeys(c, n, k); }
'''

class ReaderAdapterTests(unittest.TestCase):
    def test_only_profile_loading_and_persistence_are_replaced(self):
        adapted = prepare_reader.adapt(FIXTURE)
        self.assertNotIn('ReadEmulatorProfile()', adapted)
        self.assertNotIn('PersistKeys(c, n, k)', adapted)
        self.assertIn('int UnchangedCrypto(int value) { return value ^ 0x55; }', adapted)
        self.assertIn('LoadFromFile(file, false)', adapted)
        self.assertIn('file.parent_path() / "title.keys"', adapted)
        self.assertIn('dev_mode = false', adapted)
    def test_missing_or_ambiguous_upstream_functions_fail(self):
        with self.assertRaises(RuntimeError): prepare_reader.adapt(FIXTURE.replace('ReloadKeys', 'ChangedKeys'))
        with self.assertRaises(RuntimeError): prepare_reader.adapt(FIXTURE + 'void KeyManager::ReloadKeys() {}')
    def test_missing_upstream_include_fails(self):
        with self.assertRaises(RuntimeError): prepare_reader.adapt(FIXTURE.replace('core/loader/loader.h', 'other.h'))

if __name__ == '__main__': unittest.main()
