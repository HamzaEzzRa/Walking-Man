#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AudioDecoder
{
	struct WwiseMediaBuffer
	{
		std::string path{};
		std::vector<uint8_t> bytes{};
		uint32_t sourcePluginId = 0x00040001;
		long long durationMs = 0;
		uint32_t sampleRate = 0;
		uint32_t channels = 0;
		uint32_t bitsPerSample = 0;
		bool decodedToPcm = false;
	};

	bool IsSupportedCustomAudioPath(const std::string& path);
	bool IsSupportedCustomAudioPath(const std::filesystem::path& path);

	bool LoadWwiseMedia(const std::string& path, WwiseMediaBuffer& output);
}
