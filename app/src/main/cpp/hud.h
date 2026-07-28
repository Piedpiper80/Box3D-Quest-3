// hud.h
//
// A debug text overlay for the perf readout — Phase 0 Step 2.
//
// Deliberately built out of the machinery that already exists: each lit pixel of
// each glyph is one more cube instance in the same buffer the physics bodies use,
// so the HUD needs no font texture, no second shader, no extra draw call and no
// new render pass. It costs a few hundred instances, which after Step 4 is free.
//
// The glyphs are placed in *world* space, in front of the head, rather than in
// view space. That matters: the instance buffer is uploaded once and drawn by
// both eyes, so anything eye-dependent would have to break that. A world-space
// panel is seen correctly from both eyes with no special handling, and gets
// proper stereo depth into the bargain.
#pragma once

#include "physics.h"

// Append the cube instances spelling `text` to `items`, starting at index
// `count`, and return the new count. Never writes past `maxItems`.
//
// The text plane is defined by `origin` (top-left of the first glyph), `right`
// and `up`, which should be unit vectors. `pixelSize` is the world size of one
// font pixel — glyphs are 5x7 pixels on a 6-pixel advance.
//
// Unsupported characters render as blank. Lower case is folded to upper.
int Hud_AppendText(RenderItem* items, int count, int maxItems, const float origin[3],
                   const float right[3], const float up[3], const float forward[3],
                   float pixelSize, const float color[3], const char* text);
