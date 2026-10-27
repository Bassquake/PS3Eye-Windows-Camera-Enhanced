// Pixel conversions for the PS3 Eye virtual camera. Portable C++, no OS dependencies.
#pragma once

#include <cstdint>

namespace ps3eye {

// Bilinear demosaic of the PS3 Eye's GRBG Bayer pattern
//   row 0: G R G R ...
//   row 1: B G B G ...
// into 32-bit B,G,R,X pixels (Media Foundation RGB32 byte order).
// dstStride may be negative (bottom-up buffers); dst then points at the first row in memory order
// as returned by IMF2DBuffer2::Lock2DSize (scanline 0).
void BayerToBGRX(const uint8_t* bayer, int width, int height, uint8_t* dst, long dstStride);

// BT.601 limited-range ("studio swing"), the usual webcam encoding.
void BGRXToNV12(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);
void BGRXToYUY2(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Planar 4:2:0 (Y plane, then U, then V), as DirectShow's I420/IYUV. dstStride is the Y stride;
// the chroma planes use half of it.
void BGRXToI420(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Copies BGRX rows into a destination with a different stride.
void CopyBGRX(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Packs BGRX into 24-bit B,G,R. dstStride may be negative (bottom-up DIBs).
void BGRXToBGR(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Halves a BGRX image in both directions (2x2 box filter): 640x480 -> 320x240.
// width/height are the source size and must be even.
void HalveBGRX(const uint8_t* src, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Doubles a BGRX image in both directions (each pixel becomes 2x2): 320x240 -> 640x480.
// width/height are the source size.
void DoubleBGRX(const uint8_t* src, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Picture adjustments done on the PC after demosaicing (the sensor's own versions don't work
// in raw Bayer mode). Same scales as ps3eye::Settings.
struct ImageAdjust
{
	int brightness = 128;   // 0..255, 128 = unchanged
	int contrast = 128;     // 0..255, 128 = unchanged (x1), 255 = about x2
	int saturation = 128;   // 0..255, 128 = unchanged, 0 = grey
	int hue = 0;            // degrees
	int sharpness = 0;      // 0..63, 0 = off

	bool IsNeutral() const;
};

// Applies 'adjust' to a BGRX image in place. Does nothing when it's neutral.
void AdjustBGRX(uint8_t* bgrx, int width, int height, long stride, const ImageAdjust& adjust);

// Fills a BGRX image with the "no camera" placeholder: dark grey with a camera icon
// crossed out, so apps keep running while the PS3 Eye is unplugged.
void DrawNoSignal(uint8_t* bgrx, int width, int height, long stride);

} // namespace ps3eye
