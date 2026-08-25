import unittest
from unittest import TestCase

from test.support import threading_helper, import_helper

_testcapi = import_helper.import_module("_testcapi")

threading_helper.requires_working_threading(module=True)

NUM_MUTATORS = 2
NUM_CALLERS = 6
ITERS = 1000
MIN_KEYS, MAX_KEYS = 4, 3000


class TestKwargsUnpackRace(TestCase):
    def test_mutate_kwargs_during_unpack(self):
        # gh-86199: unpacking a shared kwargs dict (here forwarded straight
        # into the unpack by pyobject_fastcalldict, as a C extension calling
        # PyObject_Call would) must be safe against another thread resizing
        # it; otherwise _PyStack_UnpackDict overflows its argument buffer.
        fastcalldict = _testcapi.pyobject_fastcalldict

        def target(**kwargs):
            return len(kwargs)

        # A few resident keys keep the size above 0, which has its own path.
        shared = {f"k{i}": i for i in range(MIN_KEYS)}

        def resize_kwargs():
            for _ in range(ITERS):
                for i in range(MIN_KEYS, MAX_KEYS):
                    shared[f"k{i}"] = i
                for i in range(MAX_KEYS - 1, MIN_KEYS - 1, -1):
                    shared.pop(f"k{i}", None)

        def call_target():
            for _ in range(ITERS):
                try:
                    fastcalldict(target, (), shared)
                except Exception:
                    pass

        threading_helper.run_concurrently(
            [resize_kwargs] * NUM_MUTATORS + [call_target] * NUM_CALLERS)


if __name__ == "__main__":
    unittest.main()
