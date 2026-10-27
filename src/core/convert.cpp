#include "convert.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps3eye {

namespace {

inline uint8_t ToY(int r, int g, int b) { return (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
inline uint8_t ToU(int r, int g, int b) { return (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
inline uint8_t ToV(int r, int g, int b) { return (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

// Mirror an out-of-range index by two so the Bayer colour parity is preserved.
inline int Reflect(int i, int n)
{
	if (i < 0)
		return (n > 1) ? 1 : 0;
	if (i >= n)
		return (n > 1) ? n - 2 : 0;
	return i;
}

} // namespace

void BayerToBGRX(const uint8_t* bayer, int w, int h, uint8_t* dst, long dstStride)
{
	for (int y = 0; y < h; y++)
	{
		const uint8_t* up = bayer + (size_t)Reflect(y - 1, h) * w;
		const uint8_t* row = bayer + (size_t)y * w;
		const uint8_t* dn = bayer + (size_t)Reflect(y + 1, h) * w;
		uint8_t* out = dst + (long)y * dstStride;
		const bool oddRow = (y & 1) != 0;

		for (int x = 0; x < w; x++)
		{
			const int xl = x == 0 ? Reflect(-1, w) : x - 1;
			const int xr = x == w - 1 ? Reflect(w, w) : x + 1;
			int r, g, b;
			const int c = row[x];

			if (!oddRow)
			{
				if (!(x & 1)) // G on a G-R row
				{
					g = c;
					r = (row[xl] + row[xr] + 1) >> 1;
					b = (up[x] + dn[x] + 1) >> 1;
				}
				else // R
				{
					r = c;
					g = (row[xl] + row[xr] + up[x] + dn[x] + 2) >> 2;
					b = (up[xl] + up[xr] + dn[xl] + dn[xr] + 2) >> 2;
				}
			}
			else
			{
				if (!(x & 1)) // B
				{
					b = c;
					g = (row[xl] + row[xr] + up[x] + dn[x] + 2) >> 2;
					r = (up[xl] + up[xr] + dn[xl] + dn[xr] + 2) >> 2;
				}
				else // G on a B-G row
				{
					g = c;
					r = (up[x] + dn[x] + 1) >> 1;
					b = (row[xl] + row[xr] + 1) >> 1;
				}
			}

			out[0] = (uint8_t)b;
			out[1] = (uint8_t)g;
			out[2] = (uint8_t)r;
			out[3] = 0xFF;
			out += 4;
		}
	}
}

void BGRXToNV12(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	uint8_t* uvPlane = dst + (long)h * dstStride;
	for (int y = 0; y < h; y += 2)
	{
		const uint8_t* s0 = src + (long)y * srcStride;
		const uint8_t* s1 = (y + 1 < h) ? s0 + srcStride : s0;
		uint8_t* y0 = dst + (long)y * dstStride;
		uint8_t* y1 = (y + 1 < h) ? y0 + dstStride : nullptr;
		uint8_t* uv = uvPlane + (long)(y / 2) * dstStride;

		for (int x = 0; x < w; x += 2)
		{
			const uint8_t* p00 = s0 + x * 4;
			const uint8_t* p01 = (x + 1 < w) ? p00 + 4 : p00;
			const uint8_t* p10 = s1 + x * 4;
			const uint8_t* p11 = (x + 1 < w) ? p10 + 4 : p10;

			y0[x] = ToY(p00[2], p00[1], p00[0]);
			if (x + 1 < w)
				y0[x + 1] = ToY(p01[2], p01[1], p01[0]);
			if (y1)
			{
				y1[x] = ToY(p10[2], p10[1], p10[0]);
				if (x + 1 < w)
					y1[x + 1] = ToY(p11[2], p11[1], p11[0]);
			}

			int r = (p00[2] + p01[2] + p10[2] + p11[2] + 2) >> 2;
			int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) >> 2;
			int b = (p00[0] + p01[0] + p10[0] + p11[0] + 2) >> 2;
			uv[x] = ToU(r, g, b);
			uv[x + 1] = ToV(r, g, b);
		}
	}
}

void BGRXToYUY2(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	for (int y = 0; y < h; y++)
	{
		const uint8_t* s = src + (long)y * srcStride;
		uint8_t* d = dst + (long)y * dstStride;
		for (int x = 0; x < w; x += 2)
		{
			const uint8_t* p0 = s + x * 4;
			const uint8_t* p1 = (x + 1 < w) ? p0 + 4 : p0;
			int r = (p0[2] + p1[2] + 1) >> 1;
			int g = (p0[1] + p1[1] + 1) >> 1;
			int b = (p0[0] + p1[0] + 1) >> 1;
			d[0] = ToY(p0[2], p0[1], p0[0]);
			d[1] = ToU(r, g, b);
			d[2] = ToY(p1[2], p1[1], p1[0]);
			d[3] = ToV(r, g, b);
			d += 4;
		}
	}
}

void BGRXToI420(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	const long cStride = dstStride / 2;
	uint8_t* uPlane = dst + (long)h * dstStride;
	uint8_t* vPlane = uPlane + (long)((h + 1) / 2) * cStride;
	for (int y = 0; y < h; y += 2)
	{
		const uint8_t* s0 = src + (long)y * srcStride;
		const uint8_t* s1 = (y + 1 < h) ? s0 + srcStride : s0;
		uint8_t* y0 = dst + (long)y * dstStride;
		uint8_t* y1 = (y + 1 < h) ? y0 + dstStride : nullptr;
		uint8_t* u = uPlane + (long)(y / 2) * cStride;
		uint8_t* v = vPlane + (long)(y / 2) * cStride;

		for (int x = 0; x < w; x += 2)
		{
			const uint8_t* p00 = s0 + x * 4;
			const uint8_t* p01 = (x + 1 < w) ? p00 + 4 : p00;
			const uint8_t* p10 = s1 + x * 4;
			const uint8_t* p11 = (x + 1 < w) ? p10 + 4 : p10;

			y0[x] = ToY(p00[2], p00[1], p00[0]);
			if (x + 1 < w)
				y0[x + 1] = ToY(p01[2], p01[1], p01[0]);
			if (y1)
			{
				y1[x] = ToY(p10[2], p10[1], p10[0]);
				if (x + 1 < w)
					y1[x + 1] = ToY(p11[2], p11[1], p11[0]);
			}

			int r = (p00[2] + p01[2] + p10[2] + p11[2] + 2) >> 2;
			int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) >> 2;
			int b = (p00[0] + p01[0] + p10[0] + p11[0] + 2) >> 2;
			u[x / 2] = ToU(r, g, b);
			v[x / 2] = ToV(r, g, b);
		}
	}
}

void CopyBGRX(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	for (int y = 0; y < h; y++)
		memcpy(dst + (long)y * dstStride, src + (long)y * srcStride, (size_t)w * 4);
}

void BGRXToBGR(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	for (int y = 0; y < h; y++)
	{
		const uint8_t* s = src + (long)y * srcStride;
		uint8_t* d = dst + (long)y * dstStride;
		for (int x = 0; x < w; x++)
		{
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			s += 4;
			d += 3;
		}
	}
}

void HalveBGRX(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	for (int y = 0; y + 1 < h; y += 2)
	{
		const uint8_t* s0 = src + (long)y * srcStride;
		const uint8_t* s1 = s0 + srcStride;
		uint8_t* d = dst + (long)(y / 2) * dstStride;
		for (int x = 0; x + 1 < w; x += 2)
		{
			for (int c = 0; c < 3; c++)
				d[c] = (uint8_t)((s0[c] + s0[4 + c] + s1[c] + s1[4 + c] + 2) >> 2);
			d[3] = 0xFF;
			s0 += 8;
			s1 += 8;
			d += 4;
		}
	}
}

void DoubleBGRX(const uint8_t* src, int w, int h, long srcStride, uint8_t* dst, long dstStride)
{
	for (int y = 0; y < h; y++)
	{
		const uint8_t* s = src + (long)y * srcStride;
		uint8_t* d0 = dst + (long)(y * 2) * dstStride;
		uint8_t* d1 = d0 + dstStride;
		for (int x = 0; x < w; x++)
		{
			for (int c = 0; c < 3; c++)
				d0[c] = d0[4 + c] = d1[c] = d1[4 + c] = s[c];
			d0[3] = d0[7] = d1[3] = d1[7] = 0xFF;
			s += 4;
			d0 += 8;
			d1 += 8;
		}
	}
}

bool ImageAdjust::IsNeutral() const
{
	return brightness == 128 && contrast == 128 && saturation == 128 && hue % 360 == 0 && sharpness == 0;
}

void AdjustBGRX(uint8_t* img, int w, int h, long stride, const ImageAdjust& a)
{
	// Brightness, contrast, saturation and hue are all linear in R,G,B, so together they're one
	// colour matrix plus an offset: luma Y = .299R + .587G + .114B gets contrast and brightness,
	// the colour differences U = B - Y, V = R - Y get rotated (hue) and scaled (saturation).
	if (a.brightness != 128 || a.contrast != 128 || a.saturation != 128 || a.hue % 360 != 0)
	{
		const double c = a.contrast / 128.0, s = a.saturation / 128.0, b = a.brightness - 128;
		const double rad = a.hue * 3.14159265358979323846 / 180.0, cs = std::cos(rad) * s, sn = std::sin(rad) * s;
		auto apply = [&](double r, double g, double bl, double out[3])
		{
			const double y = 0.299 * r + 0.587 * g + 0.114 * bl, u = bl - y, v = r - y;
			const double y2 = (y - 128) * c + 128 + b, u2 = u * cs - v * sn, v2 = u * sn + v * cs;
			out[0] = y2 + u2;                                   // B
			out[1] = y2 - (0.299 * v2 + 0.114 * u2) / 0.587;    // G
			out[2] = y2 + v2;                                   // R
		};
		double off[3], col[3][3];
		apply(0, 0, 0, off);
		apply(0, 0, 1, col[0]); // unit blue
		apply(0, 1, 0, col[1]); // unit green
		apply(1, 0, 0, col[2]); // unit red
		int m[3][3], o[3];   // 10-bit fixed point: out[i] = sum m[i][j] * in[j] (in = B,G,R)
		for (int i = 0; i < 3; i++)
		{
			o[i] = (int)std::lround(off[i] * 1024) + 512;
			for (int j = 0; j < 3; j++)
				m[i][j] = (int)std::lround((col[j][i] - off[i]) * 1024);
		}
		for (int y = 0; y < h; y++)
		{
			uint8_t* p = img + (long)y * stride;
			for (int x = 0; x < w; x++, p += 4)
			{
				const int B = p[0], G = p[1], R = p[2];
				for (int i = 0; i < 3; i++)
					p[i] = (uint8_t)std::clamp((m[i][0] * B + m[i][1] * G + m[i][2] * R + o[i]) >> 10, 0, 255);
			}
		}
	}

	// sharpening: each pixel minus a share of its four neighbours (a small unsharp mask)
	if (a.sharpness > 0 && w > 2 && h > 2)
	{
		const int k = std::clamp(a.sharpness, 0, 63) * 4; // up to x1 of the edge signal (/256 per neighbour)
		const size_t row = (size_t)w * 4;
		std::vector<uint8_t> above(img, img + row), cur(row);
		for (int y = 1; y + 1 < h; y++)
		{
			uint8_t* p = img + (long)y * stride;
			const uint8_t* below = p + stride;
			std::memcpy(cur.data(), p, row);
			for (int x = 1; x + 1 < w; x++)
			{
				for (int c = 0; c < 3; c++)
				{
					const int i = x * 4 + c, centre = cur[i];
					const int edge = 4 * centre - above[i] - below[i] - cur[i - 4] - cur[i + 4];
					p[i] = (uint8_t)std::clamp(centre + ((edge * k) >> 8), 0, 255);
				}
			}
			above.swap(cur);
		}
	}
}

void DrawNoSignal(uint8_t* img, int w, int h, long stride)
{
	auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b)
	{
		if (x < 0 || y < 0 || x >= w || y >= h)
			return;
		uint8_t* p = img + (long)y * stride + x * 4;
		p[0] = b;
		p[1] = g;
		p[2] = r;
		p[3] = 0xFF;
	};

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			put(x, y, 32, 32, 36);

	// camera body + lens, centred
	const int s = std::max(1, std::min(w, h) / 12); // unit
	const int cx = w / 2, cy = h / 2;
	const int bodyL = cx - 3 * s, bodyR = cx + s, bodyT = cy - 2 * s, bodyB = cy + 2 * s;
	for (int y = bodyT; y < bodyB; y++)
		for (int x = bodyL; x < bodyR; x++)
			put(x, y, 150, 150, 155);

	// lens: triangle pointing left, attached to the right of the body
	for (int i = 0; i < 2 * s; i++)
	{
		int half = (int)((long)i * 3 / 2) + s / 2;
		for (int y = cy - half; y <= cy + half; y++)
			put(bodyR + i, y, 150, 150, 155);
	}

	// red slash
	const int len = 4 * s;
	const int thick = std::max(1, s / 3);
	for (int i = -len; i <= len; i++)
		for (int t = -thick; t <= thick; t++)
			put(cx + i + t, cy + i, 220, 60, 60);
}

} // namespace ps3eye
