"""Recovery accepts both emitted stub formats and rejects non-stub bodies."""
import unittest
from recover_midfunction_entries import STUB

class StubFormatTest(unittest.TestCase):
    def test_both_formats(self):
        for logging in ('', 'static int _seen; if (!_seen) { _seen = 1; recomp_stub_ran(0x000110D0u, "not detected"); } '):
            source='void sub_000110D0(void) { '+logging+'g_esp += 4; /* 0x000110D0: not detected */ }'
            self.assertEqual(STUB.fullmatch(source).group(2),'000110D0')

    def test_mismatched_address_or_extra_code_rejected(self):
        source='void sub_000110D0(void) { static int _seen; if (!_seen) { _seen = 1; recomp_stub_ran(0x000110D0u, "not detected"); } g_esp += 4; /* 0x000110D0: not detected */ }'
        self.assertIsNone(STUB.fullmatch(source.replace('0x000110D0u','0x000110D1u')))
        self.assertIsNone(STUB.fullmatch(source.replace('g_esp += 4;','eax = 0; g_esp += 4;')))

if __name__=='__main__':unittest.main()
