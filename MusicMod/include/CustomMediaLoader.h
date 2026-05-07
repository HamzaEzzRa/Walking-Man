#pragma once

#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cctype>
#include <filesystem>

#include "AudioDecoder.h"
#include "ModConfiguration.h"

#include "Logger.h"
#include "Utils.h"

namespace fs = std::filesystem;

namespace CustomMediaLoader
{
	struct CustomSongInfo
	{
		std::string filename;
		std::string title = "";
		std::string artist = "Custom";
	};

	extern const uint32_t customAreaMusicOriginalMediaId;
	extern std::vector<std::unique_ptr<std::string>> customSongStringStorage;

	const char* StoreCustomSongString(const std::string& value);
	CustomSongInfo ParseCustomSongInfo(const fs::path& audioPath);

	bool LoadCustomSongsFromFolder();
	void BindCustomSongsToAreaTrack(const MusicData& baseTrack);
}
