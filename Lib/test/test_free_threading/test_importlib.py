import unittest
from importlib import reload, import_module
from threading import Thread, Barrier

from test.support import threading_helper


class ImportlibThreading(unittest.TestCase):

    @threading_helper.reap_threads
    @threading_helper.requires_working_threading()
    def test_reload(self):
        modules = [
            "collections.abc",
            "collections",
            'sys', 'os','sqlite3'
        ]

        number_of_threads = 2 * len(modules)
        number_of_iterations = 600
        barrier = Barrier(number_of_threads)
        def work(mod):
            barrier.wait()
            for ii in range(number_of_iterations):
                m = import_module(mod)
                reload(m)

        worker_threads = []
        for ii in range(number_of_threads):
            mod = modules[ii % len(modules)]

            worker_threads.append(
                Thread(target=work, args=[mod]))
        for t in worker_threads:
            t.start()
        for t in worker_threads:
            t.join()


if __name__ == "__main__":
    unittest.main()
