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

// Copies BGRX rows into a destination with a different stride.
void CopyBGRX(const uint8_t* bgrx, int width, int height, long srcStride, uint8_t* dst, long dstStride);

// Fills a BGRX image with the "no camera" placeholder: dark grey with a camera icon
// crossed out, so apps keep running while the PS3 Eye is unplugged.
void DrawNoSignal(uint8_t* bgrx, int width, int height, long stride);

} // namespace ps3eye
