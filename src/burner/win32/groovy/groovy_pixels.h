// Groovy MiSTer - pixel packers: FBNeo's pVidImage -> the Groovy wire format.
//
// Dependency-free (stdint only) so the packers can be unit-tested standalone. The one thing
// that matters here is byte order, and getting it wrong is the single most common
// first-integration bug on this protocol, so each packer is a separate named function with
// a known-pixel test rather than an inline blob inside the sender.
//
// Source formats come from FBNeo's SetBurnHighCol (src/burner/misc.cpp):
//   nVidImageDepth == 32 -> HighCol24: (r<<16)|(g<<8)|b in a native-endian UINT32,
//                           i.e. little-endian bytes [B][G][R][X]
//   nVidImageDepth == 16 -> HighCol16: RRRRRGGGGGGBBBBB in a native-endian UINT16
//
// Wire formats (handoff doc 5.3) are BGR-ordered:
//   RGB888 : [B][G][R] per pixel, tightly packed, stride = hActive*3
//   RGB565 : little-endian uint16, R in 15:11, G 10:5, B 4:0  (== HighCol16 on x86)

#ifndef GROOVY_PIXELS_H
#define GROOVY_PIXELS_H

#include <stdint.h>
#include "groovy_modeline.h"	// RgbMode / BytesPerPixel - one definition, shared with the gate

namespace GroovyMiSTer {

// Each packer reads a w*h image at srcPitch bytes per row and writes tightly-packed rows.
// bFlip180 rotates 180 degrees (reverses row order and pixel order within each row), used
// for BDF_ORIENTATION_FLIPPED cocktail/inverted-monitor games.

// 32bpp XRGB8888 -> RGB888. The default path: drop every fourth byte.
void PackBGR888From32(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180);

// 16bpp RGB565 -> RGB565. Byte-identical: a per-row memcpy.
void PackRGB565From16(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180);

// 32bpp XRGB8888 -> RGB565. Quantizing; for a user who forces 565 on the wire.
void PackRGB565From32(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180);

// 16bpp RGB565 -> RGB888. Expanding (bit replication); for a user who forces a 16bpp
// local render but wants RGB888 on the wire.
void PackBGR888From565(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                       uint8_t* dst, bool bFlip180);

// Dispatch on the depth we actually got and the wire mode we want. Never assume the depth
// we asked the blitter for is the depth it produced.
// Returns bytes written, or 0 if the combination is unsupported (nothing is written then).
uint32_t PackFrame(int32_t srcDepth, int rgbMode,
                   const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                   uint8_t* dst, bool bFlip180);

// Known-pixel self-test over every packer. Returns 0 on success, or the number of failures;
// on failure *pszFirstFail (if non-NULL) points at a static description of the first one.
int SelfTestPixels(const char** pszFirstFail);

} // namespace GroovyMiSTer

#endif // GROOVY_PIXELS_H
