// Frame rates the PS3 Eye's sensor runs at, fastest first. No dependencies, so the DirectShow
// filter (32-bit build included) can list them without linking the camera driver. The register
// values for each rate are in ps3eye_camera.cpp; tests/test_core.cpp checks the two agree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace ps3eye {

// 83 fps at 640x480 and 205/290 fps at 320x240 exist too but give corrupt video.
constexpr uint16_t kRatesVga[] = { 75, 60, 50, 40, 30, 25, 20, 15, 10, 8, 5, 3, 2 };
constexpr uint16_t kRatesQvga[] = { 187, 150, 137, 125, 100, 90, 75, 60, 50, 40, 37, 30, 17, 15, 12, 10, 7, 5, 3, 2 };

inline const uint16_t* SupportedRates(uint32_t width, size_t* count)
{
	*count = width <= 320 ? std::size(kRatesQvga) : std::size(kRatesVga);
	return width <= 320 ? kRatesQvga : kRatesVga;
}

inline uint32_t MaxFps(uint32_t width)
{
	return width <= 320 ? kRatesQvga[0] : kRatesVga[0];
}

} // namespace ps3eye
