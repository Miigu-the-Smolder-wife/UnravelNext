"""CPU model of the temporal upscale (Upscale.hlsl), used to tune its reconstruction without a GPU.

A band-limited synthetic scene (<= 0.45 cycles/output px) with thin bright lines and a hard edge is point-sampled at
the jittered internal positions (Halton 2, 3), reconstructed as Upscale.hlsl does, and compared with the native point
samples by UpscaleCompare.compare (energy 0.25-0.5 cycles/px, SSIM). Environment: KS kernel K (output px), CYC Halton
cycle, CAP history cap (frames with CAPF=1), V pan speed (output px/frame, history resampled), IW internal size (of 240),
RS history resampling (cr Catmull-Rom, l3 Lanczos-3, c-0.75 cubic). Variants: old (974c6bb) or kernel:clamp:blend,
e.g. s:low:R = the current Upscale.hlsl (narrow kernel, low-frequency history correction, wide kernel for young history).
Example: KS=60 CYC=64 CAP=64 CAPF=1 python UpscaleModel.py old s:low:R
"""
import numpy as np, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import UpscaleCompare as uc

rng=np.random.default_rng(1)
W=H=240; S=4  # output size, supersampled scene grid
# band-limited scene (<= 0.5 cycles/output px) + thin lines + edges
N=W*S
spec=np.fft.fft2(rng.standard_normal((N,N)))
fy=np.fft.fftfreq(N)[:,None]*S; fx=np.fft.fftfreq(N)[None,:]*S
r=np.sqrt(fx*fx+fy*fy)
tex=np.real(np.fft.ifft2(spec*((r<0.45)*(r>0.02))))
tex=(tex-tex.min())/(tex.max()-tex.min())
yy,xx=np.mgrid[0:N,0:N]/S
scene=0.3+0.5*tex
scene+=0.8*(np.abs(((yy+0.3*xx)%12)-6)<0.35)   # thin bright lines ~0.7 px wide
scene*= np.where(xx>150,0.4,1.0)                 # hard edge
def sample(px,py):
    # bilinear lookup of the scene at output-pixel coordinates px,py (arrays)
    gx=np.clip(px*S-0.5,0,N-1.001); gy=np.clip(py*S-0.5,0,N-1.001)
    x0=np.floor(gx).astype(int); y0=np.floor(gy).astype(int); fx_=gx-x0; fy_=gy-y0
    a=scene[y0,x0]; b=scene[y0,x0+1]; c=scene[y0+1,x0]; d=scene[y0+1,x0+1]
    return (a*(1-fx_)+b*fx_)*(1-fy_)+(c*(1-fx_)+d*fx_)*fy_
oy,ox=np.mgrid[0:H,0:W]+0.5
native=sample(ox,oy)
def halton(i,b):
    f=1;r=0
    while i>0:
        f/=b; r+=f*(i%b); i//=b
    return r
def tm(c): return c/(1+c)
def itm(c): return c/np.maximum(1-c,1/65504)

KS=float(__import__("os").environ.get("KS","2.29"))
def run(frames, w, h, variant):
    sx=W/w; sy=H/h
    cycle=int(__import__('os').environ.get('CYC','0')) or min(64,max(8,int(np.ceil(8*W*H/(w*h)))))
    hist=np.zeros((H,W)); n=np.zeros((H,W))
    # output pixel centre in internal pixel coords
    x=(ox)/sx; y=(oy)/sy
    for f in range(frames):
        k=f%cycle+1; jx=halton(k,2)-0.5; jy=halton(k,3)-0.5
        iy,ix=np.mgrid[0:h,0:w]
        # sample at internal pixel k sees the scene at unjittered position (k+0.5-j) internal px
        V=float(__import__('os').environ.get('V','0'))
        shift=f*V
        img=sample((ix+0.5-jx)*sx+shift,(iy+0.5-jy)*sy)
        if V!=0 and f>0: hist=cr_shift(hist, V)
        kcx=np.floor(x+jx).astype(int); kcy=np.floor(y+jy).astype(int)
        sumW=np.zeros_like(x); wsum=np.zeros_like(x); wmax=np.zeros_like(x)
        sumS=np.zeros_like(x); wsumS=np.zeros_like(x); wmaxS=np.zeros_like(x)
        m1=np.zeros_like(x); m2=np.zeros_like(x); lo=np.full_like(x,1e30); hi=np.full_like(x,-1e30)
        for dy in (-1,0,1):
            for dx in (-1,0,1):
                kx=np.clip(kcx+dx,0,w-1); ky=np.clip(kcy+dy,0,h-1)
                c=tm(img[ky,kx])
                m1+=c; m2+=c*c; lo=np.minimum(lo,c); hi=np.maximum(hi,c)
                ddx=kx+0.5-jx-x; ddy=ky+0.5-jy-y
                ww=np.exp(-2.29*(ddx*ddx+ddy*ddy))
                sumW+=ww*c; wsum+=ww; wmax=np.maximum(wmax,ww)
                ws=np.exp(-KS*((ddx*sx)**2+(ddy*sy)**2))
                sumS+=ws*c; wsumS+=ws; wmaxS=np.maximum(wmaxS,ws)
        cur_w=sumW/np.maximum(wsum,1e-6)
        cur_s=np.where(wsumS>1e-4,sumS/np.maximum(wsumS,1e-9),cur_w)
        mean=m1/9; sig=np.sqrt(np.maximum(m2/9-mean*mean,0))
        blo=np.maximum(lo,mean-sig); bhi=np.minimum(hi,mean+sig)
        hcur=tm(hist)
        kv,cv,bv=variant.split(':') if ':' in variant else ('x','x','x')
        if ':' in variant:
            cur = cur_s if kv=='s' else cur_w
            if bv=='r': cur=cur_s*np.clip(n/4,0,1)+cur_w*(1-np.clip(n/4,0,1))
            if bv=='R':
                Ef=(np.pi/KS)/(sx*sy); t_=np.clip(n/(4*Ef),0,1); cur=cur_s*t_+cur_w*(1-t_); wm=wsumS
            wm = wmaxS if kv=='s' else wmax
            if bv=='a': wm = np.minimum(wsumS,1.0)
            if bv=='A': wm = wsumS
            if cv=='none': hc=hcur
            elif cv=='old': hc=np.clip(hcur,blo,bhi)
            elif cv=='mm': hc=np.clip(hcur,lo,hi)
            elif cv=='low':
                o=0.5*sx; acc=np.zeros_like(hcur)
                for (ax,ay) in ((-o,-o),(o,-o),(-o,o),(o,o)): acc+=bil(hcur, ox+ax, oy+ay)
                hlow=acc/4; delta=np.clip(hlow,blo,bhi)-hlow
                rej=np.clip(np.abs(delta)/np.maximum(bhi-blo,1e-4),0,1)
                hc=(1-rej)*(hcur+delta)+rej*np.clip(hcur,blo,bhi)
            elif cv=='lowmm':
                o=0.5*sx; acc=np.zeros_like(hcur)
                for (ax,ay) in ((-o,-o),(o,-o),(-o,o),(o,o)): acc+=bil(hcur, ox+ax, oy+ay)
                hlow=acc/4; delta=np.clip(hlow,lo,hi)-hlow
                hc=hcur+delta
        elif variant=='old':
            hc=np.clip(hcur,blo,bhi)  # (1-channel: clip = clamp)
            cur=cur_w; wm=wmax
        else:
            # low-frequency correction: history blurred over ~1 internal px vs clipped
            hb=hcur.copy()
            o=0.5*sx
            acc=np.zeros_like(hb)
            for (ax,ay) in ((-o,-o),(o,-o),(-o,o),(o,o)):
                acc+=bil(hcur, ox+ax, oy+ay)
            hlow=acc/4
            hlow_c=np.clip(hlow,blo,bhi)
            delta=hlow_c-hlow
            extent=np.maximum(bhi-blo,1e-4) if variant!='new_mm' else np.maximum(hi-lo,1e-4)
            rej=np.clip(np.abs(delta)/extent,0,1)
            hc=(1-rej)*(hcur+delta)+rej*np.clip(hcur,blo,bhi)
            cur=cur_s*np.clip(n/4,0,1)+cur_w*(1-np.clip(n/4,0,1)); wm=wmaxS
        if f==0: n[:]=0
        alpha=wm/(wm+n)
        res=np.maximum(hc*(1-alpha)+cur*alpha,0)
        E=(np.pi/KS)/(sx*sy); hist=itm(res); n=np.minimum(n+wm,float(__import__('os').environ.get('CAP','24'))*(E if __import__('os').environ.get('CAPF') else 1))
    return hist
def cr_shift(img, v):
    mode=__import__('os').environ.get('RS','cr')
    if mode!='cr': return gen_shift(img,v,mode)
    return cr_shift0(img,v)
def gen_shift(img,v,mode):
    h,w=img.shape
    pos=np.arange(w)+v; i0=np.floor(pos).astype(int); t=pos-i0
    if mode.startswith('l'):
        a=int(mode[1:]); ks=range(-a+1,a+1)
        def L(x): return np.where(np.abs(x)<a, np.sinc(x)*np.sinc(x/a),0.0)
        wts=[L(t-k) for k in ks]
    else:
        A=float(mode[1:]); ks=(-1,0,1,2)
        def C(x):
            x=np.abs(x); return np.where(x<=1,(A+2)*x**3-(A+3)*x**2+1,np.where(x<2,A*x**3-5*A*x**2+8*A*x-4*A,0.0))
        wts=[C(t-k) for k in ks]
    s_=sum(wts); out=np.zeros_like(img)
    for k,wk in zip(ks,wts): out+=img[:,np.clip(i0+k,0,w-1)]*wk/s_
    return out
def cr_shift0(img, v):
    # history at x+v (the content moved by -v on screen): Catmull-Rom along x
    h,w=img.shape
    pos=np.arange(w)+v; i0=np.floor(pos).astype(int); t=pos-i0
    wts=[-0.5*t**3+t**2-0.5*t, 1.5*t**3-2.5*t**2+1, -1.5*t**3+2*t**2+0.5*t, 0.5*t**3-0.5*t**2]
    out=np.zeros_like(img)
    for k,wk in zip((-1,0,1,2),wts): out+=img[:,np.clip(i0+k,0,w-1)]*wk
    return out
def bil(img,px,py):
    h,w=img.shape
    gx=np.clip(px-0.5,0,w-1.001); gy=np.clip(py-0.5,0,h-1.001)
    x0=np.floor(gx).astype(int); y0=np.floor(gy).astype(int); fx_=gx-x0; fy_=gy-y0
    return (img[y0,x0]*(1-fx_)+img[y0,x0+1]*fx_)*(1-fy_)+(img[y0+1,x0]*(1-fx_)+img[y0+1,x0+1]*fx_)*fy_
w=int(__import__("os").environ.get("IW","160"));h=w
import os

for v in sys.argv[1:] or ['old','new']:
    out=run(120,w,h,v)
    V=float(__import__('os').environ.get('V','0')); FR=120
    nat=sample(ox+(FR-1)*V,oy)
    a=tm(nat); b=tm(out)
    c=(20,20,W-40,H-40)
    r=uc.compare(a[20:-20,20:-20],b[20:-20,20:-20])
    print(v, {k:round(val,4) for k,val in r.items()})
# bilinear upsample of a single internal frame for reference
