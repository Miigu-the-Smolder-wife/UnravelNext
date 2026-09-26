# Debug module (track E, A15): debug drawing, buffer visualization, HUD. GDI rasterizes the debug font atlas.
target_link_libraries(${UNX_MODULE_TARGET} PRIVATE gdi32)
