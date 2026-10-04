#ifndef UNX_TSR_TILES_H
#define UNX_TSR_TILES_H
// Output footprint per group. Shared by the dispatch and shader definitions;
// evaluation tests override TSR_TILE while retaining the complete filter halo.
#define UNX_TSR_REJECT_TILE 16
#define UNX_TSR_FLICKER_TILE 16
#define UNX_TSR_THIN_TILE 16
#endif
