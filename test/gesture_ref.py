"""Compare the engine's layered pose (test/posedump) with refpose.py's Source AccumulatePose on the raw MDL.
usage: gesture_ref.py <hl2 dir> <pack.hgm> <skeleton index> <model.mdl> <gesture>   (prints max bone error per cycle)"""
import subprocess
import sys
import os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refpose import GameFS, AnimatedModel, pose  # noqa: E402

hl2, pack, skel, mdl, gest = sys.argv[1:6]
am = AnimatedModel(GameFS(hl2), mdl)
idle = am.find("ACT_IDLE")


def origins(sk):
    out = []
    for i, b in enumerate(am.skel):
        inv = np.linalg.inv(np.vstack([b.pose_to_bone, [0, 0, 0, 1]]))
        out.append((np.vstack([sk[i], [0, 0, 0, 1]]) @ inv)[:3, 3])
    return np.array(out)


worst = 0.0
for c in (0.0, 0.05, 0.2, 0.4, 0.6, 0.75, 0.9):
    r = pose(am, idle, 0.0, gest, c, c)
    o = subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), "posedump"), pack, skel, idle, gest,
                        str(c), str(c)], capture_output=True, text=True).stdout.split("\n")
    nb = int(o[0])
    e = np.array([[float(x) for x in ln.split()] for ln in o[1:1 + nb * 3]]).reshape(nb, 3, 4)
    d = np.linalg.norm(origins(r) - origins(e), axis=1)
    worst = max(worst, float(d.max()))
    print(f"cycle {c:.2f}: max bone error {d.max():.2f} units")
print(f"worst {worst:.2f}")
