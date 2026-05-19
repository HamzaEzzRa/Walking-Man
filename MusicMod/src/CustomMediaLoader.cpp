#include "CustomMediaLoader.h"

#include "AreaMusicData.h"

namespace CustomMediaLoader
{
	std::vector<std::unique_ptr<std::string>> customSongStringStorage;

	const char* StoreCustomSongString(const std::string& value)
	{
		customSongStringStorage.push_back(std::make_unique<std::string>(value));
		return customSongStringStorage.back()->c_str();
	}

	CustomSongInfo ParseCustomSongInfo(const fs::path& audioPath)
	{
		CustomSongInfo songInfo{};
		std::string stem;
		if (!Utils::TryPathToUtf8String(audioPath.stem(), stem))
		{
			return songInfo;
		}

		stem = Utils::Trim(stem);
		songInfo.filename = stem;

		const std::string separator = " - ";
		const size_t separatorPos = stem.find(separator);
		if (separatorPos != std::string::npos)
		{
			const std::string artist = Utils::Trim(stem.substr(0, separatorPos));
			const std::string title = Utils::Trim(stem.substr(separatorPos + separator.size()));
			if (!artist.empty() && !title.empty())
			{
				songInfo.artist = artist;
				songInfo.title = title;
				return songInfo;
			}
		}

		songInfo.title = stem;
		return songInfo;
	}

	bool LoadCustomSongsFromFolder()
	{
		customSongStringStorage.clear();
		ModConfiguration::Databases::customSongDatabase.clear();

		if (!ModConfiguration::customSongsEnabled)
		{
			return true;
		}

		if (ModConfiguration::customSongsFolderPath.empty())
		{
			return true;
		}

		std::error_code ec;
		const fs::path customSongsFolder = fs::u8path(ModConfiguration::customSongsFolderPath);
		if (!fs::exists(customSongsFolder, ec) || !fs::is_directory(customSongsFolder, ec))
		{
			Logging::Write(logPrefix,
				"Custom songs folder does not exist or is not a directory: %s",
				ModConfiguration::customSongsFolderPath.c_str()
			);
			return false;
		}

		std::vector<fs::directory_entry> audioFiles;
		for (fs::directory_iterator it(customSongsFolder, ec), end; it != end && !ec; it.increment(ec))
		{
			const fs::directory_entry& entry = *it;
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc))
			{
				continue;
			}

			if (AudioDecoder::IsSupportedCustomAudioPath(entry.path()))
			{
				audioFiles.push_back(entry);
			}
		}

		if (ec)
		{
			Logging::Write(logPrefix,
				"Failed while scanning custom songs folder %s: %s",
				ModConfiguration::customSongsFolderPath.c_str(),
				ec.message().c_str()
			);
			return false;
		}
		if (audioFiles.empty())
		{
			Logging::Write(logPrefix,
				"No supported audio files found in custom songs folder: %s",
				ModConfiguration::customSongsFolderPath.c_str()
			);
			return true;
		}

		std::sort(
			audioFiles.begin(),
			audioFiles.end(),
			[](const fs::directory_entry& lhs, const fs::directory_entry& rhs)
			{
				return Utils::PathToLogString(lhs.path().filename())
					< Utils::PathToLogString(rhs.path().filename());
			}
		);

		for (const fs::directory_entry& audioFile : audioFiles)
		{
			try
			{
				const fs::path audioPath = audioFile.path();
				CustomSongInfo songInfo = ParseCustomSongInfo(audioPath);
				if (songInfo.filename.empty())
				{
					Logging::Write(logPrefix,
						"Skipping custom audio with malformed filename: %s",
						Utils::PathToLogString(audioPath).c_str()
					);
					continue;
				}

				if (ModConfiguration::Databases::songDatabase.find(songInfo.filename)
					!= ModConfiguration::Databases::songDatabase.end())
				{
					Logging::Write(logPrefix,
						"Skipping custom song \"%s\" because it conflicts with a built-in area song",
						songInfo.filename.c_str()
					);
					continue;
				}

				if (ModConfiguration::Databases::customSongDatabase.find(songInfo.filename)
					!= ModConfiguration::Databases::customSongDatabase.end())
				{
					Logging::Write(logPrefix, "Skipping duplicate custom song: %s", songInfo.filename.c_str());
					continue;
				}

				fs::path absoluteAudioPath = fs::absolute(audioPath, ec);
				if (ec)
				{
					ec.clear();
					absoluteAudioPath = audioPath;
				}

				std::string absoluteAudioPathString;
				if (!Utils::TryPathToUtf8String(absoluteAudioPath, absoluteAudioPathString))
				{
					Logging::Write(logPrefix,
						"Skipping custom audio with unprintable path: %s",
						Utils::PathToLogString(audioPath).c_str()
					);
					continue;
				}

				MusicData data{};
				data.descriptionID = 0;
				data.type = MusicType::SONG;
				data.maxLength = 0;
				data.name = StoreCustomSongString(songInfo.title);
				data.artist = StoreCustomSongString(songInfo.artist);
				data.signature = "";
				data.customAreaTrack = true;
				data.customWemPath = StoreCustomSongString(absoluteAudioPathString);

				ModConfiguration::Databases::customSongDatabase.emplace(songInfo.filename, data);
				ModConfiguration::activePlaylist.insert(songInfo.filename);

				//Logging::Write(logPrefix, "Loaded custom track \"%s\"", songInfo.filename.c_str());
			}
			catch (const std::exception& e)
			{
				Logging::Write(logPrefix,
					"Skipping custom audio after scan exception for %s: %s",
					Utils::PathToLogString(audioFile.path()).c_str(),
					e.what()
				);
			}
			catch (...)
			{
				Logging::Write(logPrefix,
					"Skipping custom audio after unknown scan exception for %s",
					Utils::PathToLogString(audioFile.path()).c_str()
				);
			}
		}

		Logging::Write(logPrefix, "Loaded %zu custom tracks", ModConfiguration::Databases::customSongDatabase.size());
		return true;
	}

	void BindCustomSongsToAreaTrack(const MusicData& baseTrack)
	{
		if (!baseTrack.address)
		{
			Logging::Write(logPrefix,
				"Cannot bind area override songs because base area track \"%s\" has no scanned address",
				baseTrack.name
			);
			return;
		}

		for (auto& [name, data] : ModConfiguration::Databases::customSongDatabase)
		{
			data.address = baseTrack.address;
			data.signature = baseTrack.signature;
			data.exclusiveDC = false;
			data.active = true;
			/*Logging::Write(logPrefix,
				"Bound custom song \"%s\" to area track \"%s\" at %p",
				name.c_str(),
				baseTrack.name,
				reinterpret_cast<void*>(data.address)
			);*/
		}

		for (auto& [name, data] : ModConfiguration::Databases::songDatabase)
		{
			if (!AreaMusic::UsesInternalWwiseOverride(&data))
			{
				continue;
			}

			if (!data.address)
			{
				Logging::Write(logPrefix,
					"Cannot bind internal Wwise area override \"%s\" because its native music-player track was not found",
					name.c_str()
				);
				continue;
			}

			const uintptr_t nativeMusicPlayerAddress = data.address;
			data.address = baseTrack.address;
			data.signature = baseTrack.signature;
			Logging::Write(logPrefix,
				"Bound internal Wwise area override \"%s\" to area track \"%s\" at %p "
				"(native music-player track %p, graph key %u, source %u)",
				name.c_str(),
				baseTrack.name,
				reinterpret_cast<void*>(data.address),
				reinterpret_cast<void*>(nativeMusicPlayerAddress),
				data.internalWwiseAreaTrack.graphKey,
				data.internalWwiseAreaTrack.sourceId
			);
		}
	}
}
