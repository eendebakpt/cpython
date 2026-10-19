import gc, time, sys
class Node:
    def __init__(self):
        self.other = None
live = []
def work(n):
    for i in range(n):
        a, b = Node(), Node()
        a.other = b; b.other = a
        if i % 50 == 0:
            live.append(a)          # 2% of the cycles stay alive
t0 = time.perf_counter()
work(1_000_000)
t = time.perf_counter() - t0
st = gc.get_stats()
print(f"impl={gc.get_implementation()} wall={t:.3f}s")
for g, s in enumerate(st):
    print(f"  gen{g}: collections={s['collections']} collected={s['collected']} candidates={s['candidates']} duration={s['duration']:.4f}s max_pause={s['max_pause']*1e3:.2f}ms")
