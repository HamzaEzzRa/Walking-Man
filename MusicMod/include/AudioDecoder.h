#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AudioDecoder
{
	inline constexpr uint32_t WwisePcmSourcePluginId = 0x00010001;
	inline constexpr uint32_t WwiseVorbisSourcePluginId = 0x00040001;

	struct WwiseMediaBuffer
	{
		std::string path{};
		std::vector<uint8_t> bytes{};
		uint32_t sourcePluginId = WwiseVorbisSourcePluginId;
		long long durationMs = 0;
		uint32_t sampleRate = 0;
		uint32_t channels = 0;
		uint32_t bitsPerSample = 0;
		bool decodedToPcm = false;
	};

	const std::vector<std::string>& SupportedCustomAudioExtensions();
	bool IsSupportedCustomAudioPath(const std::string& path);
	bool IsWemPath(const std::string& path);
	bool ShouldDecodeToPcmWem(const std::string& path);
	uint32_t DetectWemSourcePluginId(const std::vector<uint8_t>& bytes);
	bool DecodeFileToPcmWem(const std::string& path, WwiseMediaBuffer& output);
	bool LoadWwiseMedia(const std::string& path, WwiseMediaBuffer& output);
}