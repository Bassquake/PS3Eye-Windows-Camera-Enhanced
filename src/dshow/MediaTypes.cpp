#include "MediaTypes.h"

#include "../core/rates.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace vcam {

namespace {

constexpr DWORD FourCC(char a, char b, char c, char d)
{
	return (DWORD)(uint8_t)a | ((DWORD)(uint8_t)b << 8) | ((DWORD)(uint8_t)c << 16) | ((DWORD)(uint8_t)d << 24);
}

// FourCC subtypes: {XXXXXXXX-0000-0010-8000-00AA00389B71}
constexpr GUID FourCCGuid(DWORD fcc)
{
	return { fcc, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
}

const GUID kYUY2 = FourCCGuid(FourCC('Y', 'U', 'Y', '2'));
const GUID kNV12 = FourCCGuid(FourCC('N', 'V', '1', '2'));
const GUID kI420 = FourCCGuid(FourCC('I', '4', '2', '0'));

#define PS3EYE_FORMAT(subtype, compression, bits) \
	{ subtype, compression, bits, 640, 480, 30 }, { subtype, compression, bits, 640, 480, 60 }, \
	{ subtype, compression, bits, 640, 480, 75 }

#define PS3EYE_FORMAT_QVGA(subtype, compression, bits) \
	{ subtype, compression, bits, 320, 240, 30 }, { subtype, compression, bits, 320, 240, 60 }, \
	{ subtype, compression, bits, 320, 240, 125 }, { subtype, compression, bits, 320, 240, 187 }

// YUY2 first: every DirectShow app handles it. Chrome, Zoom and Teams also take NV12/I420.
// Each entry's rate is just a common pick: apps can ask for any rate up to the size's maximum
// (75 fps at 640x480, 187 at 320x240), see GetStreamCaps and IAMVideoControl.
const VideoFormat kFormats[] = {
	PS3EYE_FORMAT(&kYUY2, FourCC('Y', 'U', 'Y', '2'), 16),
	PS3EYE_FORMAT(&kNV12, FourCC('N', 'V', '1', '2'), 12),
	PS3EYE_FORMAT(&kI420, FourCC('I', '4', '2', '0'), 12),
	PS3EYE_FORMAT(&MEDIASUBTYPE_RGB24, BI_RGB, 24),
	PS3EYE_FORMAT(&MEDIASUBTYPE_RGB32, BI_RGB, 32),
	PS3EYE_FORMAT_QVGA(&kYUY2, FourCC('Y', 'U', 'Y', '2'), 16),
	PS3EYE_FORMAT_QVGA(&kNV12, FourCC('N', 'V', '1', '2'), 12),
	PS3EYE_FORMAT_QVGA(&kI420, FourCC('I', '4', '2', '0'), 12),
	PS3EYE_FORMAT_QVGA(&MEDIASUBTYPE_RGB24, BI_RGB, 24),
	PS3EYE_FORMAT_QVGA(&MEDIASUBTYPE_RGB32, BI_RGB, 32),
};

#undef PS3EYE_FORMAT
#undef PS3EYE_FORMAT_QVGA

bool IsPlanar(const GUID& subtype) { return subtype == kNV12 || subtype == kI420; }
bool IsRgb(const GUID& subtype) { return subtype == MEDIASUBTYPE_RGB24 || subtype == MEDIASUBTYPE_RGB32; }

const VideoFormat* FindFormat(const GUID& subtype, uint32_t w, uint32_t h)
{
	for (const VideoFormat& f : kFormats)
	{
		if (*f.subtype == subtype && f.width == w && f.height == h)
			return &f;
	}
	return nullptr;
}

long RowBytes(const VideoFormat& f, long width)
{
	return IsPlanar(*f.subtype) ? width : ((width * f.bits / 8) + 3) & ~3L;
}

DWORD FrameBytes(const VideoFormat& f, long stride, long height)
{
	return IsPlanar(*f.subtype) ? (DWORD)(stride * height * 3 / 2) : (DWORD)(stride * height);
}

uint32_t FpsFrom(REFERENCE_TIME avg, uint32_t width)
{
	if (avg <= 0)
		return 30;
	uint32_t fps = (uint32_t)((10000000LL + avg / 2) / avg);
	return std::clamp<uint32_t>(fps, 1, MaxFps(width));
}

} // namespace

const VideoFormat* Formats(size_t* count)
{
	*count = std::size(kFormats);
	return kFormats;
}

uint32_t MaxFps(uint32_t width)
{
	return ps3eye::MaxFps(width);
}

bool DescribeFormat(const AM_MEDIA_TYPE& mt, StreamFormat& out)
{
	if (mt.majortype != MEDIATYPE_Video || mt.formattype != FORMAT_VideoInfo || !mt.pbFormat || mt.cbFormat < sizeof(VIDEOINFOHEADER))
		return false;
	const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)mt.pbFormat;
	const BITMAPINFOHEADER& bih = vih->bmiHeader;

	// a renderer may pad rows (wider biWidth) and say the picture size in rcSource
	long width = std::abs(bih.biWidth), height = std::abs(bih.biHeight);
	const RECT& rc = vih->rcSource;
	if (rc.right > rc.left && rc.bottom > rc.top)
	{
		if (rc.right - rc.left > width || rc.bottom - rc.top > height)
			return false;
		width = rc.right - rc.left;
		height = rc.bottom - rc.top;
	}

	const VideoFormat* f = FindFormat(mt.subtype, (uint32_t)width, (uint32_t)height);
	if (!f)
		return false;
	if (bih.biHeight < 0 && !IsRgb(mt.subtype))
		return false; // negative height only means something for RGB

	out.subtype = mt.subtype;
	out.pixel = mt.subtype == kYUY2 ? Pixel::YUY2 : mt.subtype == kNV12 ? Pixel::NV12 : mt.subtype == kI420 ? Pixel::I420
		: mt.subtype == MEDIASUBTYPE_RGB24 ? Pixel::RGB24 : Pixel::RGB32;
	out.width = (int)width;
	out.height = (int)height;
	out.fps = FpsFrom(vih->AvgTimePerFrame, f->width);
	out.stride = RowBytes(*f, std::abs(bih.biWidth));
	out.size = FrameBytes(*f, out.stride, height);
	out.bottomUp = IsRgb(mt.subtype) && bih.biHeight > 0;
	return true;
}

HRESULT BuildMediaType(const VideoFormat& f, uint32_t fps, AM_MEDIA_TYPE& mt)
{
	memset(&mt, 0, sizeof(mt));
	VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
	if (!vih)
		return E_OUTOFMEMORY;
	memset(vih, 0, sizeof(*vih));

	fps = std::clamp<uint32_t>(fps, 1, MaxFps(f.width));
	const long stride = RowBytes(f, (long)f.width);
	const DWORD size = FrameBytes(f, stride, (long)f.height);
	vih->AvgTimePerFrame = 10000000LL / fps;
	vih->dwBitRate = size * 8 * fps;
	vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	vih->bmiHeader.biWidth = (LONG)f.width;
	vih->bmiHeader.biHeight = (LONG)f.height; // RGB: bottom-up, the DIB default
	vih->bmiHeader.biPlanes = 1;
	vih->bmiHeader.biBitCount = f.bits;
	vih->bmiHeader.biCompression = f.compression;
	vih->bmiHeader.biSizeImage = size;

	mt.majortype = MEDIATYPE_Video;
	mt.subtype = *f.subtype;
	mt.bFixedSizeSamples = TRUE;
	mt.bTemporalCompression = FALSE;
	mt.lSampleSize = size;
	mt.formattype = FORMAT_VideoInfo;
	mt.cbFormat = sizeof(VIDEOINFOHEADER);
	mt.pbFormat = (BYTE*)vih;
	return S_OK;
}

HRESULT NormalizeMediaType(const AM_MEDIA_TYPE& mt, AM_MEDIA_TYPE& out)
{
	StreamFormat sf;
	if (!DescribeFormat(mt, sf))
		return VFW_E_INVALIDMEDIATYPE;
	const VideoFormat* f = FindFormat(sf.subtype, (uint32_t)sf.width, (uint32_t)sf.height);
	return BuildMediaType(*f, sf.fps, out);
}

void FreeMediaType(AM_MEDIA_TYPE& mt)
{
	if (mt.cbFormat && mt.pbFormat)
		CoTaskMemFree(mt.pbFormat);
	mt.cbFormat = 0;
	mt.pbFormat = nullptr;
	if (mt.pUnk)
	{
		mt.pUnk->Release();
		mt.pUnk = nullptr;
	}
}

void DeleteMediaType(AM_MEDIA_TYPE* mt)
{
	if (!mt)
		return;
	FreeMediaType(*mt);
	CoTaskMemFree(mt);
}

HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src)
{
	*dst = *src;
	if (src->cbFormat && src->pbFormat)
	{
		dst->pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
		if (!dst->pbFormat)
		{
			dst->cbFormat = 0;
			dst->pUnk = nullptr;
			return E_OUTOFMEMORY;
		}
		memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
	}
	else
	{
		dst->cbFormat = 0;
		dst->pbFormat = nullptr;
	}
	if (dst->pUnk)
		dst->pUnk->AddRef();
	return S_OK;
}

AM_MEDIA_TYPE* CreateMediaType(const AM_MEDIA_TYPE* src)
{
	AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
	if (!mt)
		return nullptr;
	if (FAILED(CopyMediaType(mt, src)))
	{
		CoTaskMemFree(mt);
		return nullptr;
	}
	return mt;
}

const wchar_t* SubtypeName(const GUID& s)
{
	if (s == kYUY2) return L"YUY2";
	if (s == kNV12) return L"NV12";
	if (s == kI420) return L"I420";
	if (s == MEDIASUBTYPE_RGB24) return L"RGB24";
	if (s == MEDIASUBTYPE_RGB32) return L"RGB32";
	return L"?";
}

} // namespace vcam
