"""py tests\fix_chunithm_test.py -- the CHUNITHM lane rule (tools\fix_chunithm.chunithm_lanes)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import fix_chunithm as fx

L = lambda notes: [lane for _, lane in fx.chunithm_lanes(notes)]
# Single notes: the centre of (cell, width) cut in five.
assert L([(0, 0, 4)]) == [1] and L([(0, 0, 8)]) == [2] and L([(0, 0, 16)]) == [3]
assert L([(0, 4, 5)]) == [3] and L([(0, 4, 12)]) == [4] and L([(0, 12, 4)]) == [5] and L([(0, 2, 2)]) == [1]
# Collisions, as the converter resolved them (the most frequent cases in the library).
assert sorted(L([(0, 6, 4), (0, 6, 4)])) == [2, 3]
assert sorted(L([(0, 5, 3), (0, 8, 3)])) == [3, 4]
assert sorted(L([(0, 12, 4), (0, 12, 4)])) == [4, 5]
assert sorted(L([(0, 0, 4), (0, 0, 4)])) == [1, 2]
assert sorted(L([(0, 4, 4), (0, 4, 4)])) == [2, 3]
assert sorted(L([(0, 8, 4), (0, 8, 4)])) == [3, 4]
assert sorted(L([(0, 0, 2), (0, 2, 2)])) == [1, 2]
# Different ticks never interact.
assert L([(0, 6, 4), (1, 6, 4)]) == [3, 3]
print("fix_chunithm_test OK")
