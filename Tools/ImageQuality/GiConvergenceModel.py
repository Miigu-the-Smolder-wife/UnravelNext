"""Scalar CPU model of the GI cache's multi-bounce convergence in a closed room (GiIntegrate.hlsl, GiInternal.hlsli
giHistoryAlpha / giHistoryNext / Jacobi length, the steady window of gi.history_updates_max_static), for the run-to-run
brightness spread and the drift within a run of a still scene (84e789f: 4.4 % between two runs, bath 1080p).

C cells each updated every P frames (7.8 k updates per frame over ~94 k entries: P ~ 12), an update = S + rho x the mean of
64 other cells' stored values, times (1 + noise x N(0, 1)) (noise: the per-update spread, 19 % measured in the bathhouse),
then the history rules (Jacobi replacement while the reads are young and the cell's phase is below its Jacobi length
ceil(log 0.004 / log share), then the running mean capped at hmax, or hstatic while steady). C stands for the number of
cells that carry the room's multi-bounce light independently (few coarse bounce cells: small C).
Usage: python ConvergenceModel.py [C=3000] [P=12] [rho=0.8] [S=0.2] [noise=0.19] [frames=1000]   (8 runs, needs numpy)
Findings 2026-09-29: rho 0.8 / C 3000: converged, 0.5 % between runs, 0.2 % drift. rho 0.9 / C 100: 4.5 % between runs
and a 10.9 % swing within one run over frames 600-1000 (the long Jacobi phase, ~53 replacement updates, passes each
update's noise through the bounce feedback); with the Jacobi phase fixed at 8 the level stays 15 % low (the running mean
of a weakly contracting iteration converges as n^-(1-rho)) - the reason the Jacobi length exists.
"""
# Scalar model of the GI cache's multi-bounce convergence in a closed room (GiIntegrate / giHistoryAlpha rules).
import numpy as np, sys
def run(seed, C=3000, P=12, frames=1000, rho=0.8, S=0.2, noise=0.19, jac=8, hmax=32, hstatic=256, coarse=0):
    rng=np.random.default_rng(seed)
    E=np.zeros(C); J=np.full(C,jac); n=np.zeros(C,int); phase=np.zeros(C,int); fast=np.zeros(C); spr=np.zeros(C); hist0=np.ones(C,bool)
    offs=rng.integers(0,P,C); traj=[]
    for f in range(frames):
        sel=np.where((f+offs)%P==0)[0]
        if len(sel):
            # 64 bounce reads of random cells' stored values; young share = reads of cells still in Jacobi phase
            reads=rng.integers(0,C,(len(sel),64))
            vals=E[reads]; young=(phase[reads]<J[reads]).mean(1)
            est=S+rho*vals.mean(1)
            share=np.clip(rho*vals.mean(1)/np.maximum(est,1e-9),0,0.9995)
            wanted=np.where(share>0,np.ceil(np.log(0.004)/np.log(np.maximum(share,1e-6))),0).astype(int)
            Js=J[sel]; Jn=np.where(phase[sel]<np.maximum(Js,jac),np.maximum(np.maximum(Js,wanted),jac),np.maximum(Js,jac))
            J[sel]=np.minimum(Jn,4080)
            est=est*(1+noise*rng.standard_normal(len(sel)))
            h=~hist0[sel]
            held=E[sel]; fo=fast[sel]; so=spr[sel]
            steady=h&(np.abs(fo-held)<=3*so/np.sqrt(31)+0.01*held)
            cap=np.where(steady,hstatic,hmax)
            mean=np.maximum(1/(n[sel]+1),1/cap)
            isyoung=phase[sel]<J[sel]
            alpha=np.where(isyoung, mean+(1-mean)*young, mean)
            E[sel]=E[sel]+(est-E[sel])*alpha
            nn=np.where(isyoung, (n[sel]+1)*(1-young)+young, n[sel]+1)
            n[sel]=np.clip(np.round(nn),1,cap-1)
            phase[sel]=np.where(young>0,phase[sel]+1,np.maximum(phase[sel]+1,J[sel]))
            sp=noise*est
            fast[sel]=np.where(h, fo+(est-fo)/16, est); spr[sel]=np.where(h, so+(sp-so)/16, sp); hist0[sel]=False
        traj.append(E.mean())
    return np.array(traj)
def cut_model(seed, new=17000, live=94000, R=7812, P=12, noise=0.19, t0share=0.6, prior=False, kappa=2.0, delta=0.03,
              tiers=False, cachefilter=False, cells_per_tile=4, frames=(1, 4, 16, 64, 256)):
    """Redesign V2 1.1 (P1) prediction for a cut: 'new' cells appear at frame 0 (N_new ~ N_vis), the other live cells are
    converged. Per new cell: its estimate after n updates of 64 rays has relative noise 'noise' / sqrt(n); with the parent
    prior (1.1b) it starts from the parent's converged value, off by the parent-child difference (relative, N(0, delta)),
    worth kappa updates: E = (n E_meas + kappa E_prior) / (n + kappa). The cache filter (1.1c) averages an updated cell with
    its 6 same-level neighbours, weight n_j / (n_j + 4): the neighbours are new too (a cut), their independent noise adds
    up to 6 n_j / (n_j + 4) more update-equivalents (their own content differences are ignored: an optimistic bound).
    Updates: without tiers (the current rule, 2 tiers by readers and age), a new cell gets its first update within
    ceil(new / R) frames and then one every P frames; with tiers (1.1a), cells with < 4 updates share t0share of R (T0),
    cells with 4-16 updates the rest first (T1), the converged cells the remainder.
    Returns, per requested frame, the P95 over tiles (cells_per_tile independent new cells averaged) of the relative
    error of the tile mean - the unit of the baseline's 16 x 16 tile P95 (the GI term alone: direct light adds none, so
    on screen the error is this times the indirect share of the pixel)."""
    rng = np.random.default_rng(seed)
    n = np.zeros(new)
    order = rng.permutation(new)  # first-update order of the new cells
    rank = np.empty(new, int); rank[order] = np.arange(new)
    out = {}
    for f in range(1, max(frames) + 1):
        if tiers:
            budget = R
            young = np.where(n < 4)[0]
            t0 = min(len(young), int(t0share * R))
            if t0:
                pick = young[np.argsort(n[young] + rank[young] / new)[:t0]]
                n[pick] += 1; budget -= t0
            mid = np.where((n >= 4) & (n < 16))[0]
            t1 = min(len(mid), budget)
            if t1:
                pick = mid[np.argsort(n[mid] + rank[mid] / new)[:t1]]
                n[pick] += 1; budget -= t1
            if budget > 0:  # T3: the whole live set by age
                rest = np.where(n >= 16)[0]
                n[rest] += (rng.random(len(rest)) < budget / max(live, 1))
        else:
            first = rank < f * R  # first updates in creation order at R per frame, then one every P frames
            due = first & ((f - 1 - rank // R) % P == 0)
            n[due] += 1
        if f in frames:
            meas = noise / np.sqrt(np.maximum(n, 1e-9)) * rng.standard_normal(new)
            if prior:
                par = delta * rng.standard_normal(new)
                est_err = (n * meas + kappa * par) / (n + kappa)
            else:
                est_err = np.where(n > 0, meas, 1.0)  # no update yet: no value (the reader's fallback: counted as 100 %)
            if cachefilter:
                w = n / (n + 4)
                est_err = np.where(n > 0, est_err / np.sqrt(1 + 6 * w), est_err)
            m = cells_per_tile
            tiles = est_err[: (new // m) * m].reshape(-1, m).mean(axis=1)
            out[f] = float(np.percentile(np.abs(tiles), 95))
    return out
def split_model(seed, C=3000, P=12, frames=2000, rho=0.9, S=0.2, noiseS=0.19, noiseB=0.05, capS=256, bwin=4, start=0.0):
    """Redesign V2.2 11.2 (P1'-b): per cell the non-bounce part S (unbiased from the first update: running mean, weight
    max(1 / (n + 1), 1 / capS)) and the bounce part B = rho x the mean of 64 other cells' stored E = S + B, blended with
    max(1 / (n + 1), 1 / bwin) (bwin 1: Jacobi replacement). Weights are functions of the cell's update count only.
    start: the cells' initial E as a fraction of the fixed point (a history: 0 = cold cache, 0.6 = cells that converged
    their mean on a darker iteration and carry n = capS); returns the mean E over frames. The fixed point is S / (1 - rho)
    for every start; the error contracts by 1 - a_B (1 - rho) per update (a_B = 1 / bwin once n >= bwin)."""
    rng = np.random.default_rng(seed)
    fixed = S / (1 - rho)
    Sm = np.full(C, S if start > 0 else 0.0); Bm = np.full(C, start * fixed - S if start > 0 else 0.0)
    n = np.full(C, capS if start > 0 else 0)
    offs = rng.integers(0, P, C); traj = []
    for f in range(frames):
        sel = np.where((f + offs) % P == 0)[0]
        if len(sel):
            E = Sm + Bm
            reads = rng.integers(0, C, (len(sel), 64))
            b = rho * E[reads].mean(1) * (1 + noiseB * rng.standard_normal(len(sel)))
            s = S * (1 + noiseS * rng.standard_normal(len(sel)))
            aS = np.maximum(1 / (n[sel] + 1), 1 / capS); aB = np.maximum(1 / (n[sel] + 1), 1 / bwin)
            Sm[sel] += (s - Sm[sel]) * aS; Bm[sel] += (b - Bm[sel]) * aB
            n[sel] = np.minimum(n[sel] + 1, 4095)
        traj.append((Sm + Bm).mean())
    return np.array(traj)
if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == 'split':
    kw = dict(a.split('=') for a in sys.argv[2:]); kw = {k: (float(v) if '.' in v else int(v)) for k, v in kw.items()}
    fixed = kw.get('S', 0.2) / (1 - kw.get('rho', 0.9))
    for bw in (1, 2, 4):
        T = {st: np.array([split_model(r, **{**kw, 'bwin': bw, 'start': st}) for r in range(4)]) for st in (0.0, 0.6)}
        cells = []
        for f in (120, 300, 600, 1200, 1999):
            if f < T[0.0].shape[1]:
                a, b = T[0.0][:, f].mean(), T[0.6][:, f].mean()
                cells.append(f"{f}: cold {100 * (a / fixed - 1):+5.1f} % / history {100 * (b / fixed - 1):+5.1f} % (runs {100 * T[0.0][:, f].std() / a:.2f} %)")
        print(f"bwin {bw}: " + "; ".join(cells))
    sys.exit(0)
if __name__=='__main__' and len(sys.argv) > 1 and sys.argv[1] == 'cut':
    kw = dict(a.split('=') for a in sys.argv[2:]); kw = {k: (float(v) if '.' in v else int(v)) for k, v in kw.items()}
    for name, opts in (("current (no tiers, no prior)", {}), ("tiers", dict(tiers=True)), ("tiers + prior", dict(tiers=True, prior=True)),
                       ("tiers + prior + cache filter", dict(tiers=True, prior=True, cachefilter=True))):
        r = cut_model(0, **{**kw, **opts})
        print(f"{name:32s} " + "  ".join(f"{f}: {v * 100:5.1f} %" for f, v in r.items()))
    sys.exit(0)
if __name__=='__main__':
    kw=dict(a.split('=') for a in sys.argv[1:]); kw={k:(float(v) if '.' in v else int(v)) for k,v in kw.items()}
    T=np.array([run(s,**kw) for s in range(8)])
    for f in (100,300,600,900,999):
        print(f"frame {f:4d}: mean {T[:,f].mean():.4f} (fixed point {kw.get('S',0.2)/(1-kw.get('rho',0.8)):.3f})  run-to-run std {T[:,f].std()/T[:,f].mean()*100:.2f} %")
    d=np.diff(T[0,600:1000]); print('within one run, frames 600-1000: range %.2f %%'%((T[0,600:].max()-T[0,600:].min())/T[0,600:].mean()*100))
