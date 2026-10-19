import json
import sys
for i, l in enumerate(open(sys.argv[1])):
    try:
        json.loads(l)
    except json.JSONDecodeError as e:
        print(i, e, repr(l[max(0, e.pos - 80):e.pos + 40]))
