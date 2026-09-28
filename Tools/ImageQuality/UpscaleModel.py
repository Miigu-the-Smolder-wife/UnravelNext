"""CPU model of the temporal upscale (Passes/Shading/Upscale.hlsl), to tune its reconstruction without a GPU.

Scene: high-contrast anisotropic wood-grain-like noise up to ~0.65 cycles per output px (AMP: its log contrast) with
near-horizontal thin moulding lines, point-sampled (bilinear over a 4x supersampled grid) at the jittered internal
positions of a 2/3 upscale; the reconstruction as Upscale.hlsl (narrow output-pixel kernel K, wide internal kernel for a
young history, history weight cap in frames of the expected per-frame weight), compared with the native point samples by
UpscaleCompare.compare (energy 0.25-0.5 cycles/px, SSIM, edge contrast) plus p99 / mean abs error and the frame-to-frame
flicker of a still image.
Arguments: one or more "jitter,K,cap,V": jitter halton (64 Halton 2, 3) or gridP (a (1.5 P)^2 stratified grid), K, the
history cap in frames, V a horizontal pan in output px / frame (the history resampled each frame; keep V * FR within the
scene: V <= 0.45 at FR 160). Environment: CL history correction (none, low = the sigma box of 4f2c4eb, mm, gateM = min /
max widened by M x extent, fmmM = the whole history clipped to that, shader = the current Upscale.hlsl: still gate0.5,
moving fmm0.1, Lanczos-3), RS history resampling (cr, l3, t3, c-0.75), AMP, FR frames, LCF / LCM a lighting change
(frame, factor), SG jitter sign (-1: a sign error), LOCK share of screen-locked content.
Example: CL=shader python UpscaleModel.py halton,60,64,0 halton,60,4,0.45     (Needs numpy.)
The 84e789f finding: CL=low (the sigma box) still: SSIM 0.958, energy 1.16 (the grid pattern); CL=shader 0.9987, 0.95.
"""
import numpy as np, sys, os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import UpscaleCompare as uc
env=lambda k,d: type(d)(os.environ.get(k,d))
W=H=192; S=4; N=W*S
rng=np.random.default_rng(3)
yy,xx=np.mgrid[0:N,0:N]/S
# wood grain: strongly anisotropic noise (fine vertical streaks) up to ~0.6 cycles/px, attenuated above 0.5 (trilinear)
spec=np.fft.fft2(rng.standard_normal((N,N)))
fy=np.fft.fftfreq(N)[:,None]*S; fx=np.fft.fftfreq(N)[None,:]*S
r=np.sqrt((fx)**2+(fy*6)**2)   # streaks: high freq along x
filt=(r<0.65)*(r>0.03)*np.clip((0.65-r)/0.25,0,1)
tex=np.real(np.fft.ifft2(spec*filt)); tex=(tex-tex.mean())/tex.std()
scene=np.exp(float(os.environ.get("AMP","0.5"))*tex)*0.35
# mouldings: near-horizontal thin lines (slope 1/40), 0.8 px dark lines
for y0 in (40,44,90):
    scene-=0.25*(np.abs(yy-(y0+xx/40))<0.4)
scene=np.clip(scene,0.01,None)
def sample(px,py):
    gx=np.clip(px*S-0.5,0,N-1.001); gy=np.clip(py*S-0.5,0,N-1.001)
    x0=np.floor(gx).astype(int); y0=np.floor(gy).astype(int); a=gx-x0; b=gy-y0
    return (scene[y0,x0]*(1-a)+scene[y0,x0+1]*a)*(1-b)+(scene[y0+1,x0]*(1-a)+scene[y0+1,x0+1]*a)*b
oy,ox=np.mgrid[0:H,0:W]+0.5
native=sample(ox,oy)
def halton(i,b):
    f=1;r=0
    while i>0: f/=b; r+=f*(i%b); i//=b
    return r
def jitters(mode, ratio):
    if mode=='halton':
        return [(halton(k,2)-0.5, halton(k,3)-0.5) for k in range(1,65)]
    p=int(mode[4:]); G=int(round(ratio*p))
    # G x G stratified cells visited in an R2 order of the cell index (well spread over any window of frames)
    cells=[]
    a1,a2=0.7548776662466927,0.5698402909980532
    used=set(); t=0
    while len(cells)<G*G:
        t+=1; u=((0.5+a1*t)%1,(0.5+a2*t)%1); c=(int(u[0]*G),int(u[1]*G))
        if c in used: continue
        used.add(c); cells.append(c)
    return [(-0.5+(cx+0.5)/G, -0.5+(cy+0.5)/G) for cx,cy in cells]
def bil(img,px,py):
    h,w=img.shape
    gx=np.clip(px-0.5,0,w-1.001); gy=np.clip(py-0.5,0,h-1.001)
    x0=np.floor(gx).astype(int); y0=np.floor(gy).astype(int); a=gx-x0; b=gy-y0
    return (img[y0,x0]*(1-a)+img[y0,x0+1]*a)*(1-b)+(img[y0+1,x0]*(1-a)+img[y0+1,x0+1]*a)*b
def tm(c): return c/(1+c)
def itm(c): return c/np.maximum(1-c,1/65504)
def run(frames, iw, mode, K, cap, V=0.0):
    sx=W/iw
    J=jitters(mode, sx)
    x=ox/sx; y=oy/sx
    hist=np.zeros((H,W)); n=np.zeros((H,W)); outs=[]
    E=(np.pi/K)/(sx*sx)
    for f in range(frames):
        jx,jy=J[f%len(J)]
        iy,ix=np.mgrid[0:iw,0:iw]
        SG=float(os.environ.get('SG','1')); LOCK=float(os.environ.get('LOCK','0'))
        img=sample((ix+0.5-SG*jx)*sx+f*V,(iy+0.5-SG*jy)*sx)
        LCF=int(os.environ.get('LCF','-1'))
        if LCF>=0 and f>=LCF: img=img*float(os.environ.get('LCM','1.3'))
        if LOCK: img=(1-LOCK)*img+LOCK*sample((ix+0.5)*sx,(iy+0.5)*sx)
        if V and f>0:
            # Catmull-Rom history shift
            pos=np.arange(W)+V; i0=np.floor(pos).astype(int); t=pos-i0
            RS=os.environ.get('RS','l3' if os.environ.get('CL','low')=='shader' else 'cr')
            if RS=='cr':
                ks=(-1,0,1,2); wts=[-0.5*t**3+t**2-0.5*t, 1.5*t**3-2.5*t**2+1, -1.5*t**3+2*t**2+0.5*t, 0.5*t**3-0.5*t**2]
            elif RS=='t3':
                ks=(-1,0,1,2); L=lambda x: np.where(np.abs(x)<3, np.sinc(x)*np.sinc(x/3),0.0); wts=[L(t-k) for k in ks]; sw=sum(wts); wts=[w_/sw for w_ in wts]
            elif RS.startswith('l'):
                a=int(RS[1:]); ks=range(-a+1,a+1)
                L=lambda x: np.where(np.abs(x)<a, np.sinc(x)*np.sinc(x/a),0.0); wts=[L(t-k) for k in ks]; sw=sum(wts); wts=[w_/sw for w_ in wts]
            else:
                A=float(RS[1:]); ks=(-1,0,1,2)
                def C(x):
                    x=np.abs(x); return np.where(x<=1,(A+2)*x**3-(A+3)*x**2+1,np.where(x<2,A*x**3-5*A*x**2+8*A*x-4*A,0.0))
                wts=[C(t-k) for k in ks]
            o=np.zeros_like(hist); on=np.zeros_like(n)
            for k,wk in zip(ks,wts): o+=hist[:,np.clip(i0+k,0,W-1)]*wk; on+=n[:,np.clip(i0+k,0,W-1)]*wk
            hist=o; n=np.maximum(on,0)
        kcx=np.floor(x+jx).astype(int); kcy=np.floor(y+jy).astype(int)
        s=np.zeros_like(x); ws=np.zeros_like(x); sw=np.zeros_like(x); ww=np.zeros_like(x)
        m1=np.zeros_like(x); m2=np.zeros_like(x); lo=np.full_like(x,1e30); hi=np.full_like(x,-1e30)
        SMP=[]
        for dy in (-1,0,1):
            for dx in (-1,0,1):
                kx=np.clip(kcx+dx,0,iw-1); ky=np.clip(kcy+dy,0,iw-1)
                c=tm(img[ky,kx]); ddx=kx+0.5-jx-x; ddy=ky+0.5-jy-y
                m1+=c; m2+=c*c; lo=np.minimum(lo,c); hi=np.maximum(hi,c)
                SMP.append((c,(kx+0.5-jx)*sx,(ky+0.5-jy)*sx,np.exp(-2.29*(ddx*ddx+ddy*ddy))))
                w=np.exp(-K*((ddx*sx)**2+(ddy*sx)**2)); s+=w*c; ws+=w
                w2=np.exp(-2.29*(ddx*ddx+ddy*ddy)); sw+=w2*c; ww+=w2
        wide=sw/ww; sharp=np.where(ws>1e-5,s/np.maximum(ws,1e-9),wide)
        cur=wide+(sharp-wide)*np.clip(n/(4*E),0,1)
        hc=tm(hist)
        CL=os.environ.get('CL','low')
        if CL!='none' and f>0:
            mean=m1/9; sig=np.sqrt(np.maximum(m2/9-mean*mean,0))
            blo=np.maximum(lo,mean-float(os.environ.get('SIG','1'))*sig); bhi=np.minimum(hi,mean+float(os.environ.get('SIG','1'))*sig)
            if CL=='mm': blo,bhi=lo,hi
            o=0.5*sx; acc=np.zeros_like(hc)
            for (ax,ay) in ((-o,-o),(o,-o),(-o,o),(o,o)): acc+=bil(hc,ox+ax,oy+ay)
            low=acc/4; delta=np.clip(low,blo,bhi)-low
            rej=np.clip(np.abs(delta)/np.maximum(bhi-blo,1e-4),0,1)
            if CL in ('low','mm'): hc=(1-rej)*(hc+delta)+rej*np.clip(hc,blo,bhi)
            if CL.startswith('res'):
                # residual of this frame's samples against the history at the samples' own positions
                tau=float(CL[3:] or '0.02')
                rs=np.zeros_like(hc); rw=np.zeros_like(hc); ra=np.zeros_like(hc)
                for (c,px,py,wv) in SMP:
                    r_=c-bil(hc,px,py); rs+=wv*r_; rw+=wv; ra+=wv*np.abs(r_)
                R=rs/rw
                Rd=np.sign(R)*np.maximum(np.abs(R)-tau,0)   # dead zone: the history's own blur of sub-pixel detail
                e=np.maximum(hi-lo,1e-4)
                rej=np.clip(np.abs(Rd)/e-0.5,0,1)*0  # (disocclusion: tested separately)
                hc=hc+float(os.environ.get('G','1'))*Rd
            if CL.startswith('rg'):
                t=float(CL[2:] or '0.25')
                rs=np.zeros_like(hc); rw=np.zeros_like(hc)
                for (c,px,py,wv) in SMP:
                    rs+=wv*(c-bil(hc,px,py)); rw+=wv
                R=rs/rw; e=np.maximum(hi-lo,1e-4)
                Rd=np.sign(R)*np.maximum(np.abs(R)-t*e,0)
                hc=hc+Rd
            if os.environ.get('NC'):
                a_,b_=map(float,os.environ['NC'].split(':'))
                rs=np.zeros_like(hc); rw=np.zeros_like(hc)
                for (c,px,py,wv) in SMP: rs+=wv*(c-bil(hc,px,py)); rw+=wv
                Rr=np.abs(rs/rw)/np.maximum(hi-lo,1e-4)
                n=n*(1-np.clip((Rr-a_)/b_,0,1))
            if CL=='shader':
                e=np.maximum(hi-lo,1e-4)
                d2=np.clip(low,lo-0.5*e,hi+0.5*e)-low; rej=np.clip(np.abs(d2)/e,0,1)
                still=(1-rej)*(hc+d2)+rej*np.clip(hc,lo,hi)
                mv=min(abs(V)/0.25,1.0)
                hc=still*(1-mv)+np.clip(hc,lo-0.1*e,hi+0.1*e)*mv
            if CL.startswith('gate'):
                # min/max box widened by a margin: only a history low-pass clearly outside what this frame's samples span moves it
                mg=float(CL[4:] or '0.25'); e=np.maximum(hi-lo,1e-4)
                glo=lo-mg*e; ghi=hi+mg*e
                d2=np.clip(low,glo,ghi)-low
                rej=np.clip(np.abs(d2)/e,0,1)
                hc=(1-rej)*(hc+d2)+rej*np.clip(hc,lo,hi)
            elif CL=='full': hc=np.clip(hc,blo,bhi)
            elif CL=='fullmm': hc=np.clip(hc,lo,hi)
            elif CL.startswith('fmm'):
                mg=float(CL[3:]); e=hi-lo; hc=np.clip(hc,lo-mg*e,hi+mg*e)
        alpha=ws/np.maximum(ws+n,1e-9)
        res=np.where(n>0, hc*(1-alpha)+cur*alpha, cur)
        hist=itm(res); n=np.minimum(n+ws, cap*E)
        if f>=frames-16: outs.append(hist.copy())
    return hist, outs
def phase_err(a,b,sx):
    # mean error per output phase class (period 3 at 2/3): its spread = the periodic pattern
    e=b-a; P=3
    m=np.array([[e[i::P,j::P].mean() for j in range(P)] for i in range(P)])
    # local periodic pattern: fraction of error power at k/3 frequencies
    F=np.abs(np.fft.fft2(e-e.mean()))**2; f1=np.fft.fftfreq(e.shape[0])
    mask=np.zeros_like(F,bool)
    for fa in (0,1/3,-1/3):
        for fb in (0,1/3,-1/3):
            if fa==0 and fb==0: continue
            ia=np.argmin(np.abs(f1-fa)); ib=np.argmin(np.abs(f1-fb)); mask[ia,ib]=True
    return float(F[mask].sum()/F.sum()), float(np.abs(e).mean())
if __name__=='__main__':
    iw=int(W*2/3); sx=W/iw
    for spec_ in sys.argv[1:]:
        mode,K,cap,V=spec_.split(','); K=float(K); cap=float(cap); V=float(V)
        fr=env('FR',160)
        out,outs=run(fr,iw,mode,K,cap,V)
        nat=sample(ox+(fr-1)*V,oy)*(float(os.environ.get('LCM','1.3')) if int(os.environ.get('LCF','-1'))>=0 else 1)
        a=tm(nat)[16:-16,16:-16]; b=tm(out)[16:-16,16:-16]
        r=uc.compare(a,b); per,mae=phase_err(a,b,sx)
        flick=float(np.mean(np.std(np.array(outs),axis=0))) if V==0 else 0
        pe=np.abs(b-a); print(f"{spec_:28s} p99 {np.percentile(pe,99):.4f} hf {r['hf_ratio']:.3f} ssim {r['ssim']:.4f} edge {r['edge_ratio']:.3f} mae {mae:.4f} periodic {per:.3f} flicker {flick:.4f}")
