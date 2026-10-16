"""verify_dump.py: the damage a bad dump leaves is found, and whole files are not reported."""
from paths import ROOT
import io
import random
import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import verify_dump

# Compresses like code (about 60%), never like random bytes.
CODE = bytes(random.Random(1).choice(b'\x48\x89\xe5\x8b\x45\xfc\x0f\x1f\x00\xc3\x55\x31\xc0') for _ in range(0x4000))


def random_bytes(count, seed=2):
    return random.Random(seed).randbytes(count)


def sfo_bytes(**values):
    """A minimal param.sfo (PSF 1.1) holding string values."""
    entries, keys, data = b'', b'', b''
    for key, value in values.items():
        text = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x204, len(text), len(text), len(data))
        keys += key.encode() + b'\0'
        data += text
    keys_offset = 20 + len(entries)
    return (struct.pack('<4sIIII', b'\0PSF', 0x101, keys_offset, keys_offset + len(keys), len(values))
            + entries + keys + data)


def executable(body, functions):
    """A plaintext SELF whose code segment holds body (at address 0), then an unwind table listing
    the functions [(start, end)]."""
    hdr = (len(body) + 15) & ~15
    fdes = hdr + 12 + 8 * len(functions)
    image = bytearray(body) + bytes(fdes + 16 * len(functions) - len(body))
    struct.pack_into('<4BiI', image, hdr, 1, 0x1b, 0x03, 0x3b, 0, len(functions))
    for i, (start, end) in enumerate(functions):
        fde = fdes + 16 * i
        struct.pack_into('<ii', image, hdr + 12 + 8 * i, start - hdr, fde - hdr)
        struct.pack_into('<IIiI', image, fde, 12, 0, start - (fde + 8), end - start)
    data = bytearray(0x100)
    data[:4] = verify_dump.SELF_MAGIC
    struct.pack_into('<H', data, 24, 1)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x100, len(image), len(image))
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 64, b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 128, 1, 5, 0x4000, 0, 0, len(image), len(image), 0x4000)
    struct.pack_into('<IIQQQQQQ', data, 184, 0x6474e550, 4, 0x4000 + hdr, hdr, hdr, 12, 12, 4)
    return bytes(data) + bytes(image)


def linked_executable(targets):
    """A plaintext SELF with one relocation table (R_X86_64_RELATIVE entries at these targets) in
    its dynamic linking data; the image is 0x100 bytes at address 0."""
    lib = bytearray(0x100)
    for i, target in enumerate(targets):
        struct.pack_into('<QQq', lib, 24 * i, target, 8, 0)
    for i, (tag, value) in enumerate(((0x6100002f, 0), (0x61000031, 24 * len(targets)), (0, 0))):
        struct.pack_into('<QQ', lib, 0xc0 + 16 * i, tag, value)
    data = bytearray(0x200)
    data[:4] = verify_dump.SELF_MAGIC
    struct.pack_into('<H', data, 24, 2)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 0x100, 0x100)
    struct.pack_into('<QQQQ', data, 64, 0x800 | 2 << 20, 0x300, 0x100, 0x100)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 96, b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 160, 1, 5, 0x1000, 0, 0, 0x100, 0x100, 0x1000)
    struct.pack_into('<IIQQQQQQ', data, 216, 2, 4, 0x20c0, 0, 0, 0x30, 0x30, 8)
    struct.pack_into('<IIQQQQQQ', data, 272, 0x61000000, 4, 0x2000, 0, 0, 0x100, 0, 16)
    return bytes(data) + bytes(range(256)) + bytes(lib)


class TextTests(unittest.TestCase):
    def test_zero_filled_block_ends_are_holes(self):
        text = (b'level-name airport-1-low\r\n' * 6000)[:0x20000]
        damaged = bytearray(text)
        damaged[0xee53:0x10000] = bytes(0x10000 - 0xee53)
        self.assertEqual(verify_dump.text_holes(bytes(damaged)), [(0xee53, 0x10000)])
        self.assertEqual(verify_dump.describe_holes(bytes(damaged), [(0xee53, 0x10000)]), 'zero-filled at 0xee53..0x10000')
        self.assertEqual(verify_dump.text_holes(text), [])

    def test_random_bytes_are_one_hole_and_stray_controls_are_not(self):
        text = bytearray(b'{"a": [1, 2, 3]}\n' * 1000)
        text[0x1000:0x2000] = random_bytes(0x1000)
        holes = verify_dump.text_holes(bytes(text))
        self.assertEqual(len(holes), 1)
        self.assertTrue(0x1000 <= holes[0][0] < 0x1100 and 0x1f00 < holes[0][1] <= 0x2000)
        self.assertEqual(verify_dump.text_holes(b'page one\x0cpage two\x1a\x00'), [])  # form feed, EOF mark

    def test_utf16_text_has_no_holes(self):
        self.assertEqual(verify_dump.text_holes('﻿Credits\r\n'.encode('utf-16-le') * 100), [])


class ExecutableTests(unittest.TestCase):
    def test_random_page_inside_a_function_is_found(self):
        body = bytearray(CODE)
        body[0x135c:0x235c] = random_bytes(0x1000)
        problems = verify_dump.executable_problems(executable(body, [(0, 0x2000), (0x2000, 0x4000)]))
        self.assertEqual(len(problems), 1)
        start, end = (int(x, 16) for x in problems[0].rsplit(' ', 1)[1].split('..'))
        self.assertTrue(problems[0].startswith('random bytes in its code near'))
        # Spans follow the window grid: within one step of the damaged page.
        self.assertLessEqual(abs(start - 0x135c), verify_dump.STEP)
        self.assertLessEqual(abs(end - 0x235c), verify_dump.STEP)

    def test_whole_code_and_constant_tables_after_it_are_fine(self):
        body = CODE + random_bytes(0x1000)  # a table of random constants after the last function
        self.assertEqual(verify_dump.executable_problems(executable(body, [(0, 0x2000), (0x2000, 0x4000)])), [])

    def test_relocations_outside_the_image_are_counted(self):
        self.assertEqual(verify_dump.executable_problems(linked_executable([0x10, 0x18])), [])
        problems = verify_dump.executable_problems(linked_executable([0x10, 0xeb4764c05941220d, 0x5c91f3af23b0b396]))
        self.assertEqual(problems, ['2 relocation entries point outside the image (first at ELF offset 0x2018)'])

    def test_not_an_executable_is_reported(self):
        self.assertIn('not a usable plaintext executable', verify_dump.executable_problems(verify_dump.SELF_MAGIC + bytes(60))[0])


class FolderTests(unittest.TestCase):
    def folder(self, tmp):
        game = Path(tmp)
        for path, data in {'eboot.bin': executable(CODE, [(0, 0x4000)]),
                           'sce_sys/param.sfo': sfo_bytes(TITLE_ID='CUSA02344', TITLE='Uncharted'),
                           'sce_module/libc.prx': executable(CODE, [(0, 0x4000)]),
                           'u1data/sound/a.xvag': b'XVAG' + random_bytes(5000),
                           'u1data/build/main/level.pak': b'y\n\x00\x00' + bytes(9000),
                           'u1data/data/credits.txt': b'Bluepoint Games\r\n' * 100,
                           'u1data/empty.bin': b''}.items():
            (game / path).parent.mkdir(parents=True, exist_ok=True)
            (game / path).write_bytes(data)
        return game

    def test_whole_dump_has_no_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            checked, findings = verify_dump.verify(self.folder(tmp))
            self.assertEqual((checked, findings), (7, []))

    def test_damaged_files_are_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.folder(tmp)
            (game / 'u1data/build/main/level.pak').write_bytes(bytes(9004))
            (game / 'u1data/sound/a.xvag').write_bytes(random_bytes(5004))
            (game / 'sce_module/libc.prx').unlink()
            checked, findings = verify_dump.verify(game)
            head = (game / 'u1data/sound/a.xvag').read_bytes()[:4].hex()
            self.assertEqual(findings, [('sce_module/libc.prx', 'missing'),
                                        ('u1data/build/main/level.pak', 'all 9,004 bytes are zero'),
                                        ('u1data/sound/a.xvag', f"starts with {head}, not b'XVAG'")])

    def test_reserved_area_is_zero_by_design(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.folder(tmp)
            (game / 'sce_sys/psreserved.dat').write_bytes(bytes(0x8000))
            (game / 'sce_sys/nptitle.dat').write_bytes(bytes(0x100))
            checked, findings = verify_dump.verify(game)
            self.assertEqual((checked, findings), (9, [('sce_sys/nptitle.dat', 'all 256 bytes are zero')]))

    def test_exit_status_and_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.folder(tmp)
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(verify_dump.main([str(game)]), 0)
            self.assertIn('no damage found', out.getvalue())
            (game / 'u1data/data/credits.txt').write_bytes(b'Bluepoint Games\r\n' * 10 + bytes(200))
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(verify_dump.main([str(game), '--json', str(Path(tmp) / 'report.json')]), 1)
            self.assertIn('u1data/data/credits.txt: zero-filled at 0xaa..0x172', out.getvalue())
            self.assertIn('"path": "u1data/data/credits.txt"', (Path(tmp) / 'report.json').read_text(encoding='utf-8'))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(verify_dump.main([str(Path(tmp) / 'nothing-here')]), 2)


if __name__ == '__main__':
    unittest.main()
