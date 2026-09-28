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
if __name__=='__main__':
    kw=dict(a.split('=') for a in sys.argv[1:]); kw={k:(float(v) if '.' in v else int(v)) for k,v in kw.items()}
    T=np.array([run(s,**kw) for s in range(8)])
    for f in (100,300,600,900,999):
        print(f"frame {f:4d}: mean {T[:,f].mean():.4f} (fixed point {kw.get('S',0.2)/(1-kw.get('rho',0.8)):.3f})  run-to-run std {T[:,f].std()/T[:,f].mean()*100:.2f} %")
    d=np.diff(T[0,600:1000]); print('within one run, frames 600-1000: range %.2f %%'%((T[0,600:].max()-T[0,600:].min())/T[0,600:].mean()*100))
