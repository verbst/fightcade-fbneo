// Groovy MiSTer - pixel packers. See groovy_pixels.h for the format contract.
//
// No burner.h / windows.h / groovymister.h here on purpose: this file builds standalone so
// SelfTestPixels() can be run outside the emulator.

#include "groovy_pixels.h"
#include <string.h>

namespace GroovyMiSTer {

// Row iteration helper. Forward: rows top-to-bottom. Flipped: bottom-to-top (the
// within-row reversal is handled per-packer, since the pixel stride differs).
static inline const uint8_t* RowPtr(const uint8_t* src, int32_t srcPitch, int32_t y,
                                    int32_t h, bool bFlip180)
{
	return src + (int32_t)(bFlip180 ? (h - 1 - y) : y) * srcPitch;
}

void PackBGR888From32(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180)
{
	for (int32_t y = 0; y < h; y++) {
		const uint8_t* s = RowPtr(src, srcPitch, y, h, bFlip180);
		if (bFlip180) {
			s += (int32_t)(w - 1) * 4;
			for (int32_t x = 0; x < w; x++, s -= 4) {
				dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];	// [B][G][R], drop [X]
				dst += 3;
			}
		} else {
			for (int32_t x = 0; x < w; x++, s += 4) {
				dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];
				dst += 3;
			}
		}
	}
}

void PackRGB565From16(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180)
{
	const size_t rowBytes = (size_t)w * 2;
	for (int32_t y = 0; y < h; y++) {
		const uint8_t* s = RowPtr(src, srcPitch, y, h, bFlip180);
		if (bFlip180) {
			const uint16_t* p = (const uint16_t*)s + (w - 1);
			uint16_t* d = (uint16_t*)dst;
			for (int32_t x = 0; x < w; x++) *d++ = *p--;
		} else {
			memcpy(dst, s, rowBytes);	// HighCol16 == wire RGB565, byte for byte
		}
		dst += rowBytes;
	}
}

void PackRGB565From32(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                      uint8_t* dst, bool bFlip180)
{
	for (int32_t y = 0; y < h; y++) {
		const uint8_t* s = RowPtr(src, srcPitch, y, h, bFlip180);
		const int32_t step = bFlip180 ? -4 : 4;
		if (bFlip180) s += (int32_t)(w - 1) * 4;
		uint16_t* d = (uint16_t*)dst;
		for (int32_t x = 0; x < w; x++, s += step) {
			const uint32_t b = s[0], g = s[1], r = s[2];
			*d++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		}
		dst += (size_t)w * 2;
	}
}

void PackBGR888From565(const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                       uint8_t* dst, bool bFlip180)
{
	for (int32_t y = 0; y < h; y++) {
		const uint16_t* s = (const uint16_t*)RowPtr(src, srcPitch, y, h, bFlip180);
		const int32_t step = bFlip180 ? -1 : 1;
		if (bFlip180) s += (w - 1);
		for (int32_t x = 0; x < w; x++, s += step) {
			const uint32_t p  = *s;
			const uint32_t r5 = (p >> 11) & 0x1F;
			const uint32_t g6 = (p >>  5) & 0x3F;
			const uint32_t b5 =  p        & 0x1F;
			// Bit replication, so 0x1F -> 0xFF rather than 0xF8: full-range whites.
			dst[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
			dst[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
			dst[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
			dst += 3;
		}
	}
}

uint32_t PackFrame(int32_t srcDepth, int rgbMode,
                   const uint8_t* src, int32_t srcPitch, int32_t w, int32_t h,
                   uint8_t* dst, bool bFlip180)
{
	if (!src || !dst || w <= 0 || h <= 0) return 0;

	if (srcDepth == 32 && rgbMode == RGB_888) {
		PackBGR888From32(src, srcPitch, w, h, dst, bFlip180);
		return (uint32_t)w * (uint32_t)h * 3u;
	}
	if (srcDepth == 16 && rgbMode == RGB_565) {
		PackRGB565From16(src, srcPitch, w, h, dst, bFlip180);
		return (uint32_t)w * (uint32_t)h * 2u;
	}
	if (srcDepth == 32 && rgbMode == RGB_565) {
		PackRGB565From32(src, srcPitch, w, h, dst, bFlip180);
		return (uint32_t)w * (uint32_t)h * 2u;
	}
	if (srcDepth == 16 && rgbMode == RGB_888) {
		PackBGR888From565(src, srcPitch, w, h, dst, bFlip180);
		return (uint32_t)w * (uint32_t)h * 3u;
	}
	return 0;	// 15/24bpp source or RGBA8888 wire mode: not supported, caller must refuse
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

#define GM_CHECK(cond, msg) do { if (!(cond)) { if (!firstFail) firstFail = (msg); fails++; } } while (0)

int SelfTestPixels(const char** pszFirstFail)
{
	int fails = 0;
	const char* firstFail = 0;

	// One known pixel per format. Pure red, so a channel swap cannot hide:
	// if R and B were transposed the wire byte pattern differs unmistakably.
	{
		// 2x2 XRGB8888 source, little-endian bytes [B][G][R][X].
		// (0,0) red, (1,0) green, (0,1) blue, (1,1) white
		const uint8_t src32[2 * 2 * 4] = {
			0x00, 0x00, 0xFF, 0x00,   0x00, 0xFF, 0x00, 0x00,
			0xFF, 0x00, 0x00, 0x00,   0xFF, 0xFF, 0xFF, 0x00
		};
		uint8_t dst[2 * 2 * 3];
		PackBGR888From32(src32, 2 * 4, 2, 2, dst, false);
		// wire is [B][G][R]: red -> 00 00 FF
		GM_CHECK(dst[0] == 0x00 && dst[1] == 0x00 && dst[2] == 0xFF, "PackBGR888From32: red pixel wrong (R/B swapped?)");
		GM_CHECK(dst[3] == 0x00 && dst[4] == 0xFF && dst[5] == 0x00, "PackBGR888From32: green pixel wrong");
		GM_CHECK(dst[6] == 0xFF && dst[7] == 0x00 && dst[8] == 0x00, "PackBGR888From32: blue pixel wrong");

		// 180 flip: last source pixel (white) must land first.
		uint8_t dstf[2 * 2 * 3];
		PackBGR888From32(src32, 2 * 4, 2, 2, dstf, true);
		GM_CHECK(dstf[0] == 0xFF && dstf[1] == 0xFF && dstf[2] == 0xFF, "PackBGR888From32: flip180 did not reverse");
		GM_CHECK(dstf[9] == 0x00 && dstf[10] == 0x00 && dstf[11] == 0xFF, "PackBGR888From32: flip180 last pixel wrong");
	}

	{
		// 16bpp RGB565 -> RGB565 must be byte-identical.
		const uint16_t src16[4] = { 0xF800 /*red*/, 0x07E0 /*green*/, 0x001F /*blue*/, 0xFFFF /*white*/ };
		uint8_t dst[4 * 2];
		PackRGB565From16((const uint8_t*)src16, 2 * 2, 2, 2, dst, false);
		GM_CHECK(memcmp(dst, src16, sizeof(src16)) == 0, "PackRGB565From16: not byte-identical");

		uint8_t dstf[4 * 2];
		PackRGB565From16((const uint8_t*)src16, 2 * 2, 2, 2, dstf, true);
		const uint16_t* df = (const uint16_t*)dstf;
		GM_CHECK(df[0] == 0xFFFF && df[3] == 0xF800, "PackRGB565From16: flip180 did not reverse");
	}

	{
		// 32 -> 565 quantize. Pure red must be 0xF800, not 0x001F.
		const uint8_t src32[2 * 4] = { 0x00, 0x00, 0xFF, 0x00,   0xFF, 0x00, 0x00, 0x00 };
		uint8_t dst[2 * 2];
		PackRGB565From32(src32, 2 * 4, 2, 1, dst, false);
		const uint16_t* d = (const uint16_t*)dst;
		GM_CHECK(d[0] == 0xF800, "PackRGB565From32: red not 0xF800 (R/B swapped?)");
		GM_CHECK(d[1] == 0x001F, "PackRGB565From32: blue not 0x001F");
	}

	{
		// 565 -> 888 expand, with bit replication so full-scale stays full-scale.
		const uint16_t src16[2] = { 0xF800 /*red*/, 0xFFFF /*white*/ };
		uint8_t dst[2 * 3];
		PackBGR888From565((const uint8_t*)src16, 2 * 2, 2, 1, dst, false);
		GM_CHECK(dst[0] == 0x00 && dst[1] == 0x00 && dst[2] == 0xFF, "PackBGR888From565: red wrong (R/B swapped?)");
		GM_CHECK(dst[3] == 0xFF && dst[4] == 0xFF && dst[5] == 0xFF, "PackBGR888From565: white not full range (missing bit replication?)");
	}

	{
		// Dispatcher: sizes, and refusal of unsupported combinations.
		uint8_t src[4 * 4 * 4] = { 0 };
		uint8_t dst[4 * 4 * 4];
		GM_CHECK(PackFrame(32, RGB_888, src, 4 * 4, 4, 4, dst, false) == 4u * 4u * 3u, "PackFrame: 32->888 size wrong");
		GM_CHECK(PackFrame(16, RGB_565, src, 4 * 2, 4, 4, dst, false) == 4u * 4u * 2u, "PackFrame: 16->565 size wrong");
		GM_CHECK(PackFrame(32, RGB_565, src, 4 * 4, 4, 4, dst, false) == 4u * 4u * 2u, "PackFrame: 32->565 size wrong");
		GM_CHECK(PackFrame(16, RGB_888, src, 4 * 2, 4, 4, dst, false) == 4u * 4u * 3u, "PackFrame: 16->888 size wrong");
		GM_CHECK(PackFrame(15, RGB_888, src, 4 * 2, 4, 4, dst, false) == 0u, "PackFrame: 15bpp source should be refused");
		GM_CHECK(PackFrame(32, RGB_A888, src, 4 * 4, 4, 4, dst, false) == 0u, "PackFrame: RGBA8888 wire mode should be refused");
		GM_CHECK(PackFrame(32, RGB_888, 0, 4 * 4, 4, 4, dst, false) == 0u, "PackFrame: NULL src should be refused");
	}

	if (pszFirstFail) *pszFirstFail = firstFail;
	return fails;
}

#undef GM_CHECK

} // namespace GroovyMiSTer
