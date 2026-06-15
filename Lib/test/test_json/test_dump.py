from io import StringIO
from test.test_json import PyTest, CTest

from test.support import bigmemtest, _1G

class TestDump:
    def test_dump(self):
        sio = StringIO()
        self.json.dump({}, sio)
        self.assertEqual(sio.getvalue(), '{}')

    def test_dumps(self):
        self.assertEqual(self.dumps({}), '{}')

    def test_dumps_dict(self):
        self.assertEqual(self.dumps({'x': 1, 'y': 2}),
                         '{"x": 1, "y": 2}')
        self.assertEqual(self.dumps(frozendict({'x': 1, 'y': 2})),
                         '{"x": 1, "y": 2}')
        lst = [{'x': 1}, frozendict(y=2)]
        self.assertEqual(self.dumps(lst),
                         '[{"x": 1}, {"y": 2}]')
        data = {'x': dict(a=1), 'y': frozendict(b=2)}
        self.assertEqual(self.dumps(data),
                         '{"x": {"a": 1}, "y": {"b": 2}}')

    def test_dump_skipkeys(self):
        v = {b'invalid_key': False, 'valid_key': True}
        with self.assertRaises(TypeError):
            self.json.dumps(v)

        s = self.json.dumps(v, skipkeys=True)
        o = self.json.loads(s)
        self.assertIn('valid_key', o)
        self.assertNotIn(b'invalid_key', o)

    def test_dump_skipkeys_indent_empty(self):
        v = {b'invalid_key': False}
        self.assertEqual(self.json.dumps(v, skipkeys=True, indent=4), '{}')

    def test_skipkeys_indent(self):
        v = {b'invalid_key': False, 'valid_key': True}
        self.assertEqual(self.json.dumps(v, skipkeys=True, indent=4), '{\n    "valid_key": true\n}')

    def test_encode_truefalse(self):
        self.assertEqual(self.dumps(
                 {True: False, False: True}, sort_keys=True),
                 '{"false": true, "true": false}')
        self.assertEqual(self.dumps(
                {2: 3.0, 4.0: 5, False: 1, 6: True}, sort_keys=True),
                '{"false": 1, "2": 3.0, "4.0": 5, "6": true}')

    # Issue 16228: Crash on encoding resized list
    def test_encode_mutated(self):
        a = [object()] * 10
        def crasher(obj):
            del a[-1]
        self.assertEqual(self.dumps(a, default=crasher),
                 '[null, null, null, null, null]')

    # Issue 24094
    def test_encode_evil_dict(self):
        class D(dict):
            def keys(self):
                return L

        class X:
            def __hash__(self):
                del L[0]
                return 1337

            def __lt__(self, o):
                return 0

        L = [X() for i in range(1122)]
        d = D()
        d[1337] = "true.dat"
        self.assertEqual(self.dumps(d, sort_keys=True), '{"1337": "true.dat"}')

    # gh-145244: UAF on borrowed key when default callback mutates dict
    def test_default_clears_dict_key_uaf(self):
        class Evil:
            pass

        class AlsoEvil:
            pass

        # Use a non-interned string key so it can actually be freed
        key = "A" * 100
        target = {key: Evil()}
        del key

        def evil_default(obj):
            if isinstance(obj, Evil):
                target.clear()
                return AlsoEvil()
            raise TypeError("not serializable")

        with self.assertRaises(TypeError):
            self.json.dumps(target, default=evil_default,
                            check_circular=False)

    def test_dumps_str_subclass(self):
        # Don't call obj.__str__() on str subclasses

        # str subclass which returns a different string on str(obj)
        class StrSubclass(str):
            def __str__(self):
                return "StrSubclass"

        obj = StrSubclass('ascii')
        self.assertEqual(self.dumps(obj), '"ascii"')
        self.assertEqual(self.dumps([obj]), '["ascii"]')
        self.assertEqual(self.dumps({'key': obj}), '{"key": "ascii"}')

        obj = StrSubclass('escape\n')
        self.assertEqual(self.dumps(obj), '"escape\\n"')
        self.assertEqual(self.dumps([obj]), '["escape\\n"]')
        self.assertEqual(self.dumps({'key': obj}), '{"key": "escape\\n"}')

        obj = StrSubclass('nonascii:é')
        self.assertEqual(self.dumps(obj, ensure_ascii=False),
                         '"nonascii:é"')
        self.assertEqual(self.dumps([obj], ensure_ascii=False),
                         '["nonascii:é"]')
        self.assertEqual(self.dumps({'key': obj}, ensure_ascii=False),
                         '{"key": "nonascii:é"}')
        self.assertEqual(self.dumps(obj), '"nonascii:\\u00e9"')
        self.assertEqual(self.dumps([obj]), '["nonascii:\\u00e9"]')
        self.assertEqual(self.dumps({'key': obj}),
                         '{"key": "nonascii:\\u00e9"}')

    # The tests below exercise JSONEncoder.iterencode() -- the streaming
    # encoder.  dumps()/encode() use a separate one-shot code path, so these
    # behaviours are not covered by the dumps()-based tests above.

    def test_iterencode_streams_in_chunks(self):
        # A non-trivial structure is yielded as several chunks, not buffered
        # into a single string.
        obj = {"key": list(range(10))}
        chunks = list(self.json.JSONEncoder().iterencode(obj))
        self.assertGreater(len(chunks), 1)
        self.assertEqual("".join(chunks), self.dumps(obj))

    def test_iterencode_matches_encode(self):
        # The streaming iterator must produce exactly the same output as the
        # one-shot encoder for representative inputs and options.  This cross
        # checks the streaming path without duplicating the encoder tests.
        cases = [
            None, True, False, 0, -1, 2.5, "txt", "esc\"\n\t\\",
            [], {}, [1, [2, [3, []]]], {"a": {"b": {"c": 1}}},
            {"nums": [1, 2.0, 3], "nested": {"x": [True, None]}},
            list(range(50)), {str(i): i for i in range(20)},
        ]
        for kw in ({}, {"indent": 2}, {"sort_keys": True},
                   {"separators": (",", ":")}):
            enc = self.json.JSONEncoder(**kw)
            for obj in cases:
                with self.subTest(obj=obj, options=kw):
                    streamed = "".join(enc.iterencode(obj))
                    self.assertEqual(streamed, enc.encode(obj))

    def test_iterencode_default_streams_container(self):
        # A container returned by default() is streamed chunk-by-chunk, not
        # buffered into a single chunk.
        class Wrapped:
            def __init__(self, data):
                self.data = data
        def default(o):
            if isinstance(o, Wrapped):
                return o.data
            raise TypeError
        obj = Wrapped({"a": list(range(10)), "b": Wrapped([1, 2, 3])})
        enc = self.json.JSONEncoder(default=default)
        chunks = list(enc.iterencode(obj))
        self.assertGreater(len(chunks), 1)
        self.assertEqual("".join(chunks), enc.encode(obj))

    def test_iterencode_circular_via_default(self):
        # A default() result that refers back to the object passed to
        # default() must be reported as a circular reference.
        class Wrapped:
            pass
        w = Wrapped()
        def default(o):
            return [w]
        enc = self.json.JSONEncoder(default=default)
        with self.assertRaisesRegex(ValueError, "Circular reference"):
            list(enc.iterencode(w))

    def test_iterencode_dict_mutated_during_streaming(self):
        # Mutating a dict mid-stream must not crash the interpreter.  The C
        # iterator snapshots the dict's items; the Python iterator raises
        # RuntimeError.  Either outcome is acceptable.
        d = {"k%d" % i: i for i in range(10)}
        it = self.json.JSONEncoder().iterencode(d)
        head = next(it)
        d.clear()
        d["late"] = 1
        try:
            result = head + "".join(it)
        except RuntimeError:
            return  # Python backend: dict changed size during iteration
        # C backend: encodes the snapshot taken before the mutation.
        self.assertTrue(result.startswith("{") and result.endswith("}"))

    def test_iterencode_mapping_items_mutated_during_streaming(self):
        # gh-142831: a dict subclass whose items() returns a list the mapping
        # retains -- shrunk mid-stream by a default() callback -- must not
        # crash.  The encoder must snapshot into a list it owns exclusively.
        sentinel = object()

        class Evil(dict):
            backing = None
            def items(self):
                Evil.backing = list(dict.items(self))
                return Evil.backing

        def default(o):
            if o is sentinel:
                Evil.backing.clear()  # invalidate the items list mid-stream
                return None
            raise TypeError

        d = Evil()
        d["bad"] = sentinel  # first item, so default() fires before the rest
        for i in range(30):
            d["k%d" % i] = i
        result = "".join(self.json.JSONEncoder(default=default).iterencode(d))
        self.assertTrue(result.startswith("{") and result.endswith("}"))

    def test_iterencode_mapping_non_2_tuple_items(self):
        # A mapping whose items() does not yield 2-tuples must raise rather
        # than crash.
        class Weird(dict):
            def items(self):
                return [(1, 2, 3)]
        with self.assertRaises((ValueError, TypeError)):
            "".join(self.json.JSONEncoder().iterencode(Weird({"a": 1})))


class TestPyDump(TestDump, PyTest): pass

class TestCDump(TestDump, CTest):

    # The size requirement here is hopefully over-estimated (actual
    # memory consumption depending on implementation details, and also
    # system memory management, since this may allocate a lot of
    # small objects).

    @bigmemtest(size=_1G, memuse=1)
    def test_large_list(self, size):
        N = int(30 * 1024 * 1024 * (size / _1G))
        l = [1] * N
        encoded = self.dumps(l)
        self.assertEqual(len(encoded), N * 3)
        self.assertEqual(encoded[:1], "[")
        self.assertEqual(encoded[-2:], "1]")
        self.assertEqual(encoded[1:-2], "1, " * (N - 1))
