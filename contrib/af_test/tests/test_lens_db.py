"""Tests for contrib/af_test/lens_db.py (shared AF lens profile database)."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import lens_db  # noqa: E402

L50 = "v1|EF50mm f/1.8 II|50|1|50|25|100|100|30"
L18 = "v1|EF-M18-55mm f/3.5-5.6 IS STM|20|1|25|30|4e0000|90|18"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


class LensDbTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "db.txt")

    def tearDown(self):
        self.tmp.cleanup()

    def card(self, name, text):
        p = os.path.join(self.tmp.name, name, "ML", "SETTINGS", "AFLENS.TXT")
        write(p, text)
        return os.path.join(self.tmp.name, name)

    def run_tool(self, *argv):
        return lens_db.main(["--db", self.db] + list(argv))

    def test_parse_tolerant(self):
        self.assertIsNone(lens_db.parse_line("# comment"))
        self.assertIsNone(lens_db.parse_line("v2|x|1|1|1|1|1|1|1"))
        self.assertIsNone(lens_db.parse_line("v1|x|abc|1|1|1|1|1|1"))
        self.assertIsNone(lens_db.parse_line("v1|x|50|1|0|1|1|1|10"))      # W50 out of range
        e = lens_db.parse_line(L18)
        self.assertEqual((e["name"], e["mm"], e["w50"], e["done"], e["n"]), ("EF-M18-55mm f/3.5-5.6 IS STM", 20, 25, 0x4e0000, 1))

    def test_import_creates_and_merges_median(self):
        a = self.card("a", L50 + "\n" + L18 + "\n")
        b = self.card("b", "v1|EF50mm f/1.8 II|50|1|56|30|100|100|30\n")
        c = self.card("c", "v1|EF50mm f/1.8 II|50|1|52|20|100|100|30\n")
        for card in (a, b, c):
            self.assertEqual(self.run_tool("import", "--src", os.path.basename(card), card), 0)
        db = lens_db.read_db(self.db)
        e = db[("EF50mm f/1.8 II", 50, 1)]
        self.assertEqual(e["n"], 3)
        self.assertEqual(e["w50"], 52)               # median of 50, 56, 52
        self.assertEqual(e["floor_pct"], 25)
        self.assertEqual(e["fine"], lens_db.fine_from_w50(52))
        self.assertEqual(e["src"], ["a", "b", "c"])
        self.assertEqual(db[("EF-M18-55mm f/3.5-5.6 IS STM", 20, 1)]["n"], 1)

    def test_outlier_not_merged(self):
        a = self.card("a", L50 + "\n")
        self.run_tool("import", "--src", "a", a)
        bad = self.card("b", "v1|EF50mm f/1.8 II|50|1|150|30|100|100|30\n")
        self.assertEqual(self.run_tool("import", "--src", "b", bad), 1)    # outlier flagged -> exit 1
        e = lens_db.read_db(self.db)[("EF50mm f/1.8 II", 50, 1)]
        self.assertEqual((e["w50"], e["n"]), (50, 1))

    def test_reimport_idempotent_keys_and_file_roundtrip(self):
        a = self.card("a", L50 + "\n")
        self.run_tool("import", "--src", "a", a)
        before = open(self.db).read()
        db = lens_db.read_db(self.db)
        lens_db.write_db(self.db, db)
        self.assertEqual(open(self.db).read(), before)

    def test_export_header(self):
        a = self.card("a", L50 + "\n" + 'v1|Odd "name"\\|x|10|1|20|10|100|100|14\n'.replace("\\|", "_"))
        self.run_tool("import", "--src", "a", a)
        out = os.path.join(self.tmp.name, "h.h")
        self.assertEqual(self.run_tool("export-header", "--out", out), 0)
        text = open(out).read()
        self.assertIn("#define AFPR_BUILTIN_N 2", text)
        self.assertIn('{ "EF50mm f/1.8 II", 50, 1, 50, 25, 30 },', text)
        self.assertIn('Odd \\"name\\"_x', text)
        self.assertIn('{ "", 0, 0, 0, 0, 0 }', text)

    def test_export_empty_has_sentinel(self):
        text = lens_db.header_text({})
        self.assertIn("#define AFPR_BUILTIN_N 0", text)
        self.assertIn('{ "", 0, 0, 0, 0, 0 }', text)

    def test_shipped_header_matches_repo_file(self):
        repo = lens_db.DEFAULT_DB
        hdr = lens_db.DEFAULT_HEADER
        if os.path.exists(repo) and os.path.exists(hdr):
            self.assertEqual(open(hdr).read(), lens_db.header_text(lens_db.read_db(repo)),
                             "af_lens_db.h is stale: run lens_db.py export-header")


if __name__ == "__main__":
    unittest.main()
