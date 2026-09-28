// Deterministic admission: 32-byte records { key64, anchor64, entry, kind, rank, unique }.
// Kind 0 retains an entry, 1 requests a new key, 2 supplies a free entry id.
// Order: kind, key, anchor, entry. Equal requests are interchangeable.
#ifndef UNX_GI_ADMISSION_HLSLI
#define UNX_GI_ADMISSION_HLSLI
#include "Passes/GI/GiInternal.hlsli"
struct GiAdmissionRecord { uint4 keyAnchor; uint4 meta; };
uint giAdmissionArray(GiHeader h, uint requestCapacity, uint parity)
{
    return h.offAdmission + 256 + parity * (h.capacity + requestCapacity) * 32;
}
uint giAdmissionScan(GiHeader h, uint requestCapacity)
{
    return giAdmissionArray(h, requestCapacity, 2);
}
GiAdmissionRecord giAdmissionLoad(RWByteAddressBuffer b, uint a)
{
    GiAdmissionRecord r;
    r.keyAnchor = b.Load4(a); r.meta = b.Load4(a + 16); return r;
}
void giAdmissionStore(RWByteAddressBuffer b, uint a, GiAdmissionRecord r)
{
    b.Store4(a, r.keyAnchor); b.Store4(a + 16, r.meta);
}
bool giAdmissionBefore(GiAdmissionRecord a, GiAdmissionRecord b)
{
    if (a.meta.y != b.meta.y) return a.meta.y < b.meta.y;
    if (a.keyAnchor.y != b.keyAnchor.y) return a.keyAnchor.y < b.keyAnchor.y;
    if (a.keyAnchor.x != b.keyAnchor.x) return a.keyAnchor.x < b.keyAnchor.x;
    if (a.keyAnchor.w != b.keyAnchor.w) return a.keyAnchor.w < b.keyAnchor.w;
    if (a.keyAnchor.z != b.keyAnchor.z) return a.keyAnchor.z < b.keyAnchor.z;
    return a.meta.x < b.meta.x;
}
uint giAdmissionIndex(uint3 group, uint lane) { return (group.y * 65535u + group.x) * 256u + lane; }
#endif
