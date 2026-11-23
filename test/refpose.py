"""Reference Source AccumulatePose on raw MDL data (full frame rate, interpolated) for checking the engine."""
import sys, math, struct
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), '..', 'tools'))
import numpy as np
from vpk import GameFS
from anim import AnimatedModel, pose_matrices, AL_SPLINE, AL_XFADE, AL_NOBLEND, SEQ_DELTA, SEQ_POST, SEQ_REALTIME

def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return np.array([aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
                     aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz])

def qscale(q, s):
    sinom = min(1.0, math.sqrt(q[0]**2 + q[1]**2 + q[2]**2))
    sinsom = math.sin(math.asin(sinom) * s)
    k = sinsom / (sinom + 1.192092896e-07)
    r = math.sqrt(max(0.0, 1 - sinsom * sinsom))
    return np.array([q[0]*k, q[1]*k, q[2]*k, -r if q[3] < 0 else r])

def slerp(a, b, t):
    d = float(np.dot(a, b))
    if d < 0: b = -b; d = -d
    if d > 0.9999: r = a*(1-t) + b*t; return r/np.linalg.norm(r)
    th = math.acos(d); s = math.sin(th)
    return a*math.sin((1-t)*th)/s + b*math.sin(t*th)/s

def raw(am, lab, cycle):
    f, s = am.seqs[lab]
    ad = f.animdesc(f.seq_anim(s))
    nf = max(1, ad['nframes'])
    fr = cycle * (nf - 1)
    a = int(math.floor(fr)); b = min(nf - 1, a + 1); w = fr - a
    pa, qa = f.sample(ad, a, am.skel, raw_delta=True)
    pb, qb = f.sample(ad, b, am.skel, raw_delta=True)
    q = np.array([slerp(qa[i]/np.linalg.norm(qa[i]), qb[i]/np.linalg.norm(qb[i]), w) for i in range(len(am.skel))])
    return pa*(1-w) + pb*w, q, s, ad

def accumulate(am, lab, cycle, weight, time, q, p, depth=0):
    if weight <= 0: return
    f, s = am.seqs[lab]
    ad = f.animdesc(f.seq_anim(s))
    if s['flags'] & SEQ_REALTIME:
        nf = ad['nframes']; cps = ad['fps'] / (nf - 1) if nf > 1 else 0
        cycle = time * cps; cycle -= math.floor(cycle)
    p2, q2, _, _ = raw(am, lab, cycle)
    for b, wt in am.mask(lab):
        s2 = weight * wt
        if s['flags'] & SEQ_DELTA:
            d = qscale(q2[b], min(1, s2))
            q[b] = qmul(q[b], d) if s['flags'] & SEQ_POST else qmul(d, q[b])
            p[b] = p[b] + p2[b] * s2
        elif s2 >= 1:
            q[b] = q2[b]; p[b] = p2[b]
        else:
            q[b] = slerp(q[b], q2[b], s2); p[b] = p[b]*(1-s2) + p2[b]*s2
    if depth >= 4: return
    for sub, fl, st, pk, tl, en in am.layers(lab):
        lw, lc = weight, cycle
        if st != en:
            idx, k = cycle, 1.0
            if idx < st or idx >= en: continue
            if idx < pk and st != pk: k = (idx - st) / (pk - st)
            elif idx > tl and en != tl: k = (en - idx) / (en - tl)
            if fl & AL_SPLINE: k = 3*k*k - 2*k*k*k
            if (fl & AL_XFADE) and idx > tl: lw = (k*weight) / (1 - weight + k*weight)
            elif fl & AL_NOBLEND: lw = k
            else: lw = weight * k
            lc = (cycle - st) / (en - st)
        accumulate(am, sub, lc, lw, time, q, p, depth + 1)

def pose(am, base, bcycle, gest=None, gcycle=0, gtime=0, gw=1.0):
    q = np.array([b.quat for b in am.skel], dtype=float); p = np.array([b.pos for b in am.skel], dtype=float)
    accumulate(am, base, bcycle, 1.0, 0.5, q, p)
    if gest: accumulate(am, gest, gcycle, gw, gtime, q, p)
    return pose_matrices(am.skel, p, q)
