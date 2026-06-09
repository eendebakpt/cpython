from unittest import TestCase

from _pyrepl.utils import (
    str_width,
    wlen,
    prev_next_window,
    gen_colors,
    gen_colors_with_boundaries,
    IncrementalColorizer,
    ColorSpan,
    Span,
)


# A spread of inputs exercising strings, brackets, comments, soft keywords,
# multi-line constructs and unterminated literals.
COLOR_SAMPLES = [
    "",
    "x = 1",
    "# just a comment\n",
    "result = sorted([x for x in range(100)], key=lambda v: (v % 7, -v))",
    "def fib(n):\n    a, b = 0, 1\n    for _ in range(n):\n"
    "        a, b = b, a + b\n    return a\n",
    "match command.split():\n    case [action]:\n        pass\n"
    "    case [action, obj]:\n        go(obj)\n",
    's = """a\nmultiline\nstring with def class for"""\nx = 1\n',
    "d = {\n  'a': 1,\n  'b': [1,2,\n        3],\n}\n",
    "type Alias = list[int]\nclass C:\n    def m(self): return 'hi'  # c\n",
    "f'{x!r:>{width}}' + b'bytes' + 0x1F + 3.14e2",
    'unterminated = "oops',
    "lazy import foo\nfrom bar import baz\n",
    "def f():\n    x = 1\ndef g():\n    y = 2\n",
]


class TestUtils(TestCase):
    def test_str_width(self):
        characters = [
            'a',
            '1',
            '_',
            '!',
            '\x1a',
            '\u263A',
            '\uffb9',
            '\N{LATIN SMALL LETTER E WITH ACUTE}',  # é
            '\N{LATIN SMALL LETTER E WITH CEDILLA}', # ȩ
            '\u00ad',
        ]
        for c in characters:
            self.assertEqual(str_width(c), 1)

        zero_width_characters = [
            '\N{COMBINING ACUTE ACCENT}',
            '\N{ZERO WIDTH JOINER}',
        ]
        for c in zero_width_characters:
            with self.subTest(character=c):
                self.assertEqual(str_width(c), 0)

        characters = [chr(99989), chr(99999)]
        for c in characters:
            self.assertEqual(str_width(c), 2)

    def test_wlen(self):
        for c in ['a', 'b', '1', '!', '_']:
            self.assertEqual(wlen(c), 1)
        self.assertEqual(wlen('\x1a'), 2)

        char_east_asian_width_N = chr(3800)
        self.assertEqual(wlen(char_east_asian_width_N), 1)
        char_east_asian_width_W = chr(4352)
        self.assertEqual(wlen(char_east_asian_width_W), 2)

        self.assertEqual(wlen('hello'), 5)
        self.assertEqual(wlen('hello' + '\x1a'), 7)
        self.assertEqual(wlen('e\N{COMBINING ACUTE ACCENT}'), 1)
        self.assertEqual(wlen('a\N{ZERO WIDTH JOINER}b'), 2)

    def test_prev_next_window(self):
        def gen_normal():
            yield 1
            yield 2
            yield 3
            yield 4

        pnw = prev_next_window(gen_normal())
        self.assertEqual(next(pnw), (None, 1, 2))
        self.assertEqual(next(pnw), (1, 2, 3))
        self.assertEqual(next(pnw), (2, 3, 4))
        self.assertEqual(next(pnw), (3, 4, None))
        with self.assertRaises(StopIteration):
            next(pnw)

        def gen_short():
            yield 1

        pnw = prev_next_window(gen_short())
        self.assertEqual(next(pnw), (None, 1, None))
        with self.assertRaises(StopIteration):
            next(pnw)

        def gen_raise():
            yield from gen_normal()
            1/0

        pnw = prev_next_window(gen_raise())
        self.assertEqual(next(pnw), (None, 1, 2))
        self.assertEqual(next(pnw), (1, 2, 3))
        self.assertEqual(next(pnw), (2, 3, 4))
        self.assertEqual(next(pnw), (3, 4, None))
        with self.assertRaises(ZeroDivisionError):
            next(pnw)

    def test_gen_colors_keyword_highlighting(self):
        cases = [
            # no highlights
            ("a.set", [(".", "op")]),
            ("obj.list", [(".", "op")]),
            ("obj.match", [(".", "op")]),
            ("b. \\\n format", [(".", "op")]),
            ("lazy", []),
            ("lazy()", [('(', 'op'), (')', 'op')]),
            # highlights
            ("set", [("set", "builtin")]),
            ("list", [("list", "builtin")]),
            ("    \n dict", [("dict", "builtin")]),
            (
                "    lazy import",
                [("lazy", "soft_keyword"), ("import", "keyword")],
            ),
            (
                "lazy from cool_people import pablo",
                [
                    ("lazy", "soft_keyword"),
                    ("from", "keyword"),
                    ("import", "keyword"),
                ],
            ),
            (
                "if sad: lazy import happy",
                [
                    ("if", "keyword"),
                    (":", "op"),
                    ("lazy", "soft_keyword"),
                    ("import", "keyword"),
                ],
            ),
            (
                "pass; lazy import z",
                [
                    ("pass", "keyword"),
                    (";", "op"),
                    ("lazy", "soft_keyword"),
                    ("import", "keyword"),
                ],
            ),
        ]
        for code, expected_highlights in cases:
            with self.subTest(code=code):
                colors = list(gen_colors(code))
                # Extract (text, tag) pairs for comparison
                actual_highlights = []
                for color in colors:
                    span_text = code[color.span.start:color.span.end + 1]
                    actual_highlights.append((span_text, color.tag))
                self.assertEqual(actual_highlights, expected_highlights)

    def test_gen_colors_with_boundaries_matches_gen_colors(self):
        # The spans must be identical to gen_colors(); the boundaries must be
        # valid restart points (re-tokenizing from a boundary, shifted back,
        # reproduces the tail of the full highlighting).
        for code in COLOR_SAMPLES:
            with self.subTest(code=code):
                ref = list(gen_colors(code))
                spans, boundaries = gen_colors_with_boundaries(code)
                self.assertEqual(spans, ref)
                self.assertIn(0, boundaries)
                self.assertEqual(boundaries, sorted(boundaries))
                for b in boundaries:
                    # a boundary is the start of a top-level (column 0) line
                    self.assertTrue(b >= len(code) or code[b] not in " \t")
                    shifted = [
                        ColorSpan(Span(s.span.start + b, s.span.end + b), s.tag)
                        for s in gen_colors(code[b:])
                    ]
                    tail = [s for s in ref if s.span.start >= b]
                    self.assertEqual(shifted, tail)

    def test_incremental_colorizer_matches_full(self):
        # Typing each sample character by character must, at every prefix, give
        # exactly the same spans as a full gen_colors() of that prefix.
        for code in COLOR_SAMPLES:
            with self.subTest(code=code):
                colorizer = IncrementalColorizer()
                for i in range(len(code) + 1):
                    prefix = code[:i]
                    self.assertEqual(
                        colorizer.colorize(prefix), list(gen_colors(prefix))
                    )

    def test_incremental_colorizer_edits(self):
        # Inserting, deleting and replacing the whole buffer must all keep the
        # cached result equal to a fresh full highlight.
        colorizer = IncrementalColorizer()
        buffer = "def f():\n    return 1\ndef g():\n    return 2\n"
        edits = [
            buffer,
            buffer + "x = ",          # append (typing a new top-level line)
            buffer + "x = [1, 2]",
            buffer[:10] + "pass\n" + buffer[10:],   # insert in the middle
            "y = 1",                  # replace whole buffer (e.g. history nav)
            "",                       # clear
            's = """multi\nline\nstr"""\n',
        ]
        for buf in edits:
            with self.subTest(buf=buf):
                self.assertEqual(colorizer.colorize(buf), list(gen_colors(buf)))

    def test_incremental_colorizer_exact_cache_hit(self):
        # Re-colorizing the same buffer returns an equal but independent list
        # (callers consume it in place).
        colorizer = IncrementalColorizer()
        buf = "x = [1, 2, 3]"
        first = colorizer.colorize(buf)
        second = colorizer.colorize(buf)
        self.assertEqual(first, second)
        self.assertIsNot(first, second)
