#ifndef UNX_VSM_REQUEST_HLSLI
#define UNX_VSM_REQUEST_HLSLI

// Elect one writer for every distinct (page, request bits) in the active wave,
// including divergent neighbour-page requests and waves wider than 32 lanes.
bool vsmRequestLane(uint page, uint bits)
{
    const uint4 peers = WaveMatch(uint2(page, bits));
    const uint first = peers.x ? uint(firstbitlow(peers.x)) :
                       peers.y ? 32u + uint(firstbitlow(peers.y)) :
                       peers.z ? 64u + uint(firstbitlow(peers.z)) :
                                 96u + uint(firstbitlow(peers.w));
    return WaveGetLaneIndex() == first;
}

#endif
