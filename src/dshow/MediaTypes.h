// DirectShow media types offered by the PS3 Eye filter, plus the usual AM_MEDIA_TYPE helpers
// (the DirectShow base classes aren't used, so these are written out here).
#pragma once

#include <windows.h>
#include <dshow.h>
#include <cstdint>

namespace vcam {

struct VideoFormat
{
	const GUID* subtype;
	DWORD compression;   // biCompression: a FourCC, or BI_RGB
	WORD bits;           // bits per pixel
	uint32_t width;
	uint32_t height;
	uint32_t fps;
};

// All formats in the order apps see them; index 0 is the default.
const VideoFormat* Formats(size_t* count);

enum class Pixel { YUY2, NV12, I420, RGB24, RGB32 };

// What the streaming thread needs to fill a sample of a given media type.
struct StreamFormat
{
	GUID subtype;
	Pixel pixel;
	int width;
	int height;
	uint32_t fps;
	long stride;     // bytes per row (luma row for planar formats)
	DWORD size;      // bytes per frame
	bool bottomUp;   // RGB with positive biHeight
};

// Fails for anything the filter can't produce.
bool DescribeFormat(const AM_MEDIA_TYPE& mt, StreamFormat& out);

// Fills 'mt' (pbFormat is CoTaskMemAlloc'ed) for a format at the given rate.
HRESULT BuildMediaType(const VideoFormat& f, uint32_t fps, AM_MEDIA_TYPE& mt);

// Canonical media type for whatever 'mt' describes (any accepted rate kept).
HRESULT NormalizeMediaType(const AM_MEDIA_TYPE& mt, AM_MEDIA_TYPE& out);

void FreeMediaType(AM_MEDIA_TYPE& mt);
void DeleteMediaType(AM_MEDIA_TYPE* mt);
HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src);
AM_MEDIA_TYPE* CreateMediaType(const AM_MEDIA_TYPE* src);

const wchar_t* SubtypeName(const GUID& subtype);

} // namespace vcam
