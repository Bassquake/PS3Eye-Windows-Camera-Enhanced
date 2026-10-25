// Unit tests for the portable core (no camera needed). Build: see CMakeLists.txt (PS3EYE_BUILD_TESTS).
#define PS3EYE_TESTING 1
#include "../src/core/convert.h"
#include "../src/core/ps3eye_camera.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ps3eye;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// RGB image -> GRBG mosaic (row0: G R, row1: B G)
static std::vector<uint8_t> Mosaic(const std::vector<uint8_t>& rgb, int w, int h)
{
	std::vector<uint8_t> m(w * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			const uint8_t* p = &rgb[(y * w + x) * 3];
			int ch = (y & 1) == 0 ? ((x & 1) == 0 ? 1 : 0) : ((x & 1) == 0 ? 2 : 1);
			m[y * w + x] = p[ch];
		}
	return m;
}

static void TestDemosaicFlat()
{
	const int w = 64, h = 48;
	std::vector<uint8_t> rgb(w * h * 3);
	for (int i = 0; i < w * h; i++) { rgb[i * 3] = 200; rgb[i * 3 + 1] = 100; rgb[i * 3 + 2] = 50; }
	auto bayer = Mosaic(rgb, w, h);
	std::vector<uint8_t> out(w * h * 4);
	BayerToBGRX(bayer.data(), w, h, out.data(), w * 4);
	int bad = 0;
	for (int i = 0; i < w * h; i++)
		if (out[i * 4] != 50 || out[i * 4 + 1] != 100 || out[i * 4 + 2] != 200 || out[i * 4 + 3] != 255) bad++;
	CHECK(bad == 0, "flat colour demosaic: %d wrong pixels (edges included)", bad);
}

static void TestDemosaicGradient()
{
	const int w = 640, h = 480;
	std::vector<uint8_t> rgb(w * h * 3);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			uint8_t* p = &rgb[(y * w + x) * 3];
			p[0] = (uint8_t)(x * 255 / (w - 1));
			p[1] = (uint8_t)(y * 255 / (h - 1));
			p[2] = (uint8_t)(255 - x * 255 / (w - 1));
		}
	auto bayer = Mosaic(rgb, w, h);
	std::vector<uint8_t> out(w * h * 4);
	BayerToBGRX(bayer.data(), w, h, out.data(), w * 4);
	int maxErr = 0;
	for (int y = 1; y < h - 1; y++)
		for (int x = 1; x < w - 1; x++)
		{
			const uint8_t* p = &rgb[(y * w + x) * 3];
			const uint8_t* o = &out[(y * w + x) * 4];
			maxErr = std::max(maxErr, std::abs(o[2] - p[0]));
			maxErr = std::max(maxErr, std::abs(o[1] - p[1]));
			maxErr = std::max(maxErr, std::abs(o[0] - p[2]));
		}
	CHECK(maxErr <= 2, "gradient demosaic max error %d", maxErr);
}

static void TestBottomUp()
{
	const int w = 8, h = 4;
	std::vector<uint8_t> bayer(w * h);
	for (int i = 0; i < w * h; i++) bayer[i] = (uint8_t)(i / w * 60); // rows differ
	std::vector<uint8_t> top(w * h * 4), bottom(w * h * 4);
	BayerToBGRX(bayer.data(), w, h, top.data(), w * 4);
	// bottom-up: scanline 0 is the last row in memory, stride negative
	BayerToBGRX(bayer.data(), w, h, bottom.data() + (h - 1) * w * 4, -(long)w * 4);
	bool same = true;
	for (int y = 0; y < h; y++)
		same &= memcmp(&top[y * w * 4], &bottom[(h - 1 - y) * w * 4], w * 4) == 0;
	CHECK(same, "negative stride writes rows bottom-up");
}

static void TestYuv()
{
	struct { uint8_t r, g, b, y, u, v; } c[] = {
		{ 255, 255, 255, 235, 128, 128 },
		{ 0, 0, 0, 16, 128, 128 },
		{ 255, 0, 0, 82, 90, 240 },
		{ 0, 255, 0, 145, 54, 34 },
		{ 0, 0, 255, 41, 240, 110 },
	};
	for (auto& t : c)
	{
		const int w = 4, h = 2;
		std::vector<uint8_t> bgrx(w * h * 4);
		for (int i = 0; i < w * h; i++) { bgrx[i * 4] = t.b; bgrx[i * 4 + 1] = t.g; bgrx[i * 4 + 2] = t.r; bgrx[i * 4 + 3] = 255; }
		std::vector<uint8_t> nv12(w * h * 3 / 2);
		BGRXToNV12(bgrx.data(), w, h, w * 4, nv12.data(), w);
		CHECK(std::abs(nv12[0] - t.y) <= 1 && std::abs(nv12[w * h] - t.u) <= 1 && std::abs(nv12[w * h + 1] - t.v) <= 1,
			"NV12 of (%d,%d,%d) = Y%d U%d V%d, expected Y%d U%d V%d", t.r, t.g, t.b, nv12[0], nv12[w * h], nv12[w * h + 1], t.y, t.u, t.v);
		std::vector<uint8_t> yuy2(w * h * 2);
		BGRXToYUY2(bgrx.data(), w, h, w * 4, yuy2.data(), w * 2);
		CHECK(std::abs(yuy2[0] - t.y) <= 1 && std::abs(yuy2[1] - t.u) <= 1 && std::abs(yuy2[2] - t.y) <= 1 && std::abs(yuy2[3] - t.v) <= 1,
			"YUY2 of (%d,%d,%d) wrong", t.r, t.g, t.b);
	}
}

// Build the USB byte stream the camera sends for one frame: 2048-byte payloads with a 12-byte header.
static std::vector<uint8_t> Payloads(const std::vector<uint8_t>& frame, uint32_t pts, int fid, bool truncate = false)
{
	std::vector<uint8_t> out;
	size_t pos = 0, end = truncate ? frame.size() / 2 : frame.size();
	while (pos < end)
	{
		size_t n = std::min<size_t>(2036, end - pos);
		bool last = pos + n == frame.size();
		uint8_t hdr[12] = { 12, (uint8_t)((1 << 2) | (fid ? 1 : 0) | (last ? (1 << 1) : 0)), (uint8_t)pts, (uint8_t)(pts >> 8), (uint8_t)(pts >> 16), (uint8_t)(pts >> 24) };
		out.insert(out.end(), hdr, hdr + 12);
		out.insert(out.end(), frame.begin() + pos, frame.begin() + pos + n);
		pos += n;
	}
	return out;
}

// feed like USB would: 64 KB transfers, a short payload ends a transfer
static void Feed(Camera& cam, const std::vector<uint8_t>& stream)
{
	size_t pos = 0;
	while (pos < stream.size())
	{
		size_t n = std::min<size_t>(65536, stream.size() - pos);
		cam.TestFeed(stream.data() + pos, (int)n);
		pos += n;
	}
}

static void TestFrameAssembly()
{
	const int w = 320, h = 240;
	Camera cam;
	cam.TestBegin(w, h);
	std::vector<uint8_t> f1(w * h), f2(w * h), f3(w * h), got;
	for (int i = 0; i < w * h; i++) { f1[i] = (uint8_t)i; f2[i] = (uint8_t)(i * 7); f3[i] = (uint8_t)(255 - i); }

	CHECK(!cam.WaitFrame(got, std::chrono::milliseconds(1)), "no frame before data");

	Feed(cam, Payloads(f1, 1000, 0));
	CHECK(cam.WaitFrame(got, std::chrono::milliseconds(1)) && got == f1, "frame 1 assembled");
	CHECK(!cam.WaitFrame(got, std::chrono::milliseconds(1)), "same frame not delivered twice");

	// truncated frame must be dropped, next full frame delivered
	Feed(cam, Payloads(f2, 2000, 1, true));
	Feed(cam, Payloads(f3, 3000, 0));
	CHECK(cam.WaitFrame(got, std::chrono::milliseconds(1)) && got == f3, "truncated frame dropped, frame 3 ok");

	// garbage then a good frame
	std::vector<uint8_t> junk(5000, 0xAB);
	Feed(cam, junk);
	Feed(cam, Payloads(f2, 4000, 1));
	CHECK(cam.WaitFrame(got, std::chrono::milliseconds(1)) && got == f2, "recovers after garbage");

	// slow consumer: two frames arrive, only the newest is returned
	Feed(cam, Payloads(f1, 5000, 0));
	Feed(cam, Payloads(f3, 6000, 1));
	CHECK(cam.WaitFrame(got, std::chrono::milliseconds(1)) && got == f3, "latest frame wins");
	cam.TestEnd();
}

static void TestRates()
{
	CHECK(Camera::SupportedFps(640, 30) == 30, "VGA 30");
	CHECK(Camera::SupportedFps(640, 60) == 60, "VGA 60");
	CHECK(Camera::SupportedFps(640, 100) == 75, "VGA capped at 75");
	CHECK(Camera::SupportedFps(320, 187) == 187, "QVGA 187");
	CHECK(Camera::SupportedFps(320, 300) == 187, "QVGA capped at 187");
	CHECK(Camera::SupportedFps(640, 1) == 2, "minimum rate");
}

static void WritePpm(const char* path, const std::vector<uint8_t>& bgrx, int w, int h)
{
	FILE* f = std::fopen(path, "wb");
	if (!f) return;
	std::fprintf(f, "P6 %d %d 255\n", w, h);
	for (int i = 0; i < w * h; i++) { uint8_t p[3] = { bgrx[i * 4 + 2], bgrx[i * 4 + 1], bgrx[i * 4] }; std::fwrite(p, 1, 3, f); }
	std::fclose(f);
}

int main(int argc, char** argv)
{
	TestDemosaicFlat();
	TestDemosaicGradient();
	TestBottomUp();
	TestYuv();
	TestFrameAssembly();
	TestRates();

	std::vector<uint8_t> img(640 * 480 * 4);
	DrawNoSignal(img.data(), 640, 480, 640 * 4);
	if (argc > 1)
		WritePpm(argv[1], img, 640, 480);

	std::printf("PS3 Eye cameras on the bus: %d\n", Camera::Count());
	Camera cam;
	Status st = cam.Start(640, 480, 30, Settings{});
	std::printf("Start: %s\n", StatusText(st));

	if (failures)
		std::printf("%d check(s) FAILED\n", failures);
	else
		std::printf("All checks passed\n");
	return failures ? 1 : 0;
}
