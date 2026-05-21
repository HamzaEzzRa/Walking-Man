#include "ModConfiguration.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ordered_set.h"

#include "AudioDecoder.h"
#include "GameData.h"

#include "MemoryUtils.h"
#include "Utils.h"

namespace
{
	MusicData MakeInternalWwiseAreaTrack(
		uint16_t descriptionID,
		long long durationMs,
		const char* name,
		const char* artist,
		const char* signature,
		InternalWwiseAreaTrackData internalWwiseAreaTrack,
		bool exclusiveDC = false
	)
	{
		MusicData data{};
		data.descriptionID = descriptionID;
		data.type = MusicType::SONG;
		data.maxLength = durationMs;
		data.name = name;
		data.artist = artist;
		data.signature = signature;
		data.exclusiveDC = exclusiveDC;
		data.customAreaTrack = true;
		data.internalWwiseAreaTrack = internalWwiseAreaTrack;
		return data;
	}
}

namespace ModConfiguration
{
	const std::string modPublicName = "Walking Man";
	const std::string modInternalVersion = "1.1.0";
	const std::string modLogFilename = "walkingman.log";
	const std::string enableDevFilename = "walkingman.dev";
	bool devMode = false;

	GameProvider gameProvider = GameProvider::STEAM;
	GameVersion gameVersion = GameVersion::DC;

	const std::string configFilePath = "walkingman.ini";
	const std::unordered_map<std::string, Section> headerSectionMap =
	{
		{ "[Global Settings]", Section::GLOBAL_SETTINGS },
		{ "[Playlist]", Section::ACTIVE_SONGS },
	};

	// Mod configuration defaults
	bool showSongDescription = true;
	bool showNotificationMessage = true;

	bool connectToChiralNetwork = true;
	bool stopInFacility = true;

	bool skipLockedSongs = true;

	bool customSongsEnabled = true;
	std::string customSongsFolderPath = "";

	bool allowScriptedSongs = true;
	bool showMusicPlayerUI = true;

	// Default ordered playlist
	tsl::ordered_set<std::string> activePlaylist =
	{
		"Don't Be So Serious", "Bones", "Poznan", "Anything You Need", "Easy Way Out",
		"I'm Leaving", "Give Up", "Gosia", "Without You", "Breathe In",
		"Because We Have To", "St. Eriksplan", "Rolling Over", "Once in a Long, Long While...",
		"The Machine", "Patience", "Not Around", "Please Don't Stop (Chapter 1)",
		"Tonight, tonight, tonight", "Please Don't Stop (Chapter 2)", "Half Asleep",
		"Waiting (10 Years)", "Nobody Else", "Asylums For The Feeling", "Almost Nothing", "BB's Theme"
	};

	// Maps global setting names to lambda setter functions
	const std::unordered_map<std::string, std::function<void(const std::string&)>> parameterSetters =
	{
		{"showSongDescription",
		[](const std::string& val) { showSongDescription = (val == "true" || val == "1"); }},
		
		{"showNotificationMessage",
		[](const std::string& val) { showNotificationMessage = (val == "true" || val == "1"); }},
		
		{"connectToChiralNetwork",
		[](const std::string& val) { connectToChiralNetwork = (val == "true" || val == "1"); }},

		{"stopInFacility",
		[](const std::string& val) { stopInFacility = (val == "true" || val == "1"); }},

		{"skipLockedSongs",
		[](const std::string& val) { skipLockedSongs = (val == "true" || val == "1"); }},

		{"customSongsEnabled",
		[](const std::string& val) { customSongsEnabled = (val == "true" || val == "1"); }},

		{"customSongsFolderPath",
		[](const std::string& val) { customSongsFolderPath = val; }},

		{"allowScriptedSongs",
		[](const std::string& val) { allowScriptedSongs = (val == "true" || val == "1"); }},

		{"showMusicPlayerUI",
		[](const std::string& val) { showMusicPlayerUI = (val == "true" || val == "1"); }},
	};

	bool LoadConfigFromFile()
	{
		constexpr const char* logPrefix = "Mod Configuration";

		std::ifstream file(configFilePath);
		if (!file.is_open())
		{
			return false;
		}

		Section currentSection = Section::NONE;
		std::string line;

		while (std::getline(file, line))
		{
			if (Utils::IsCommentOrEmpty(line))
			{
				continue;
			}

			size_t commentPos = line.find("//");
			if (commentPos != std::string::npos)
			{
				line = Utils::Trim(line.substr(0, commentPos));
			}
			if (line.empty())
			{
				continue;
			}

			auto sectionIt = headerSectionMap.find(line);
			if (sectionIt != headerSectionMap.end())
			{
				currentSection = sectionIt->second;
				if (currentSection == Section::ACTIVE_SONGS)
				{
					activePlaylist.clear();
				}
				continue;
			}

			switch (currentSection)
			{
				case Section::GLOBAL_SETTINGS:
				{
					size_t eqPos = line.find('=');
					if (eqPos == std::string::npos) break;

					std::string key = Utils::Trim(line.substr(0, eqPos));
					std::string val = Utils::Trim(line.substr(eqPos + 1));
					auto it = parameterSetters.find(key);
					if (it == parameterSetters.end())
					{
						break;
					}

					if (
							key != "customSongsFolderPath"
							&& val != "true" && val != "false" && val != "1" && val != "0"
					)
					{
						std::string errorMessage = "Invalid value for setting "
							+ key + " (" + val + ")"
							+ "\nPlease check your \"" + ModConfiguration::configFilePath + "\" file.";
						MemoryUtils::ShowErrorPopup(errorMessage, ModConfiguration::modPublicName);
						continue;
					}

					it->second(val);
					break;
				}
				case Section::ACTIVE_SONGS:
				{
					activePlaylist.insert(line);
					break;
				}
				case Section::INACTIVE_SONGS:
					break;
				default:
					break;
			}
		}

		return true;
	}

	namespace Databases
	{
		std::unordered_map<std::string, FunctionData> functionDatabase =
		{
			// Management functions
			{
				"RenderTask",
				{"RenderTask",
				"40 53 48 83 EC ?? 48 8B 01 45 33 D2 48 8B D9 "
				"44 0F B6 88 ?? ?? ?? ?? 45 84 C9 74 1D 8B 90",
				// gp version
				"40 53 48 83 EC 70 48 89 6C 24 68 48 8D 99 B0"}
			},
			{
				"GamePreExit",
				{"GamePreExit",
				"40 53 48 83 EC ?? 80 3D ?? ?? ?? ?? ?? 8B DA 74 06 48 83 C4 ?? 5B C3",
				// gp version
				"40 53 48 83 EC ?? 80 3D ?? ?? ?? ?? ?? 8B DA ?? ?? 48"}
			},
			{
				"GamePreLoad",
				{"GamePreLoad",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 "
				"56 41 57 48 8D AC 24 10 FB FF FF 48 81 EC F0 05 00 00 48 8B 05",
				// gp version
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 "
				"64 24 20 55 41 56 41 57 48 8D AC 24 00 FB FF FF"}
			},
			{
				"AccessMusicPool",
				{"AccessMusicPool",
				"48 89 5C 24 ?? 57 48 83 EC ?? 48 83 79 ?? ?? 75 0B 48 83 79 ?? ?? "
				"0F 84 ?? ?? ?? ?? 48 8B 01 FF 10 4C 8B 15 ?? ?? ?? ?? 48 8B F8",
				// gp version
				"48 83 EC 48 48 8B 05 15 FC D5 02"}
			},
			{
				"WwiseObjectLookup",
				{"WwiseObjectLookup",
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 45 85 C0",
				// gp version (same)
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 45 85 C0"}
			},
			{
				"AccessLanguagePool",
				{"AccessLanguagePool",
				"48 8B 01 48 FF A0 ?? ?? ?? ?? CC CC CC "
				"CC CC CC 48 89 5C 24 ?? 57 4C 8B 51",
				// gp version (same)
				"48 8B 01 48 FF A0 ?? ?? ?? ?? CC CC CC "
				"CC CC CC 48 89 5C 24 ?? 57 4C 8B 51"}
			},

			// Input tracking functions
			{
				"ProcessControllerInput",
				{"ProcessControllerInput",
				"4C 8B DC ?? 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 "
				"?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 8B 41",
				// gp version
				"41 56 48 81 EC 20 01 00 00 48 8B 05 80 24 9B 01"}
			},

			// Game state functions
			{
				"InGameFlagUpdate",
				{"InGameFlagUpdate",
				"41 57 41 56 41 55 41 54 56 57 55 53 B8 ?? ?? "
				"?? ?? E8 ?? ?? ?? ?? 48 29 C4 44 0F 29 9C 24",
				// gp version (same)
				"41 57 41 56 41 55 41 54 56 57 55 53 B8 ?? ?? "
				"00 00 E8 ?? ?? ?? ?? 48 29 C4 44 0F 29 9C 24"}
			},
			{
				"InGameAreaUpdate",
				{"InGameAreaUpdate",
				"48 8B C4 48 89 58 20 56 57 41 ?? 41 56 41 57 "
				"48 81 EC ?? ?? ?? ?? C5 78 29 48",
				// gp version
				"48 8B C4 57 48 81 EC ?? ?? ?? ?? C5 F8 29 78 C8",
				true}
			},
			{
				"GetBooleanFact",
				{"GetBooleanFact",
				"48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 48 85 D2 74 ?? C5 F8 10 05 "
				"?? ?? ?? ?? C5 F8 11 44 24 20 48 85 C9 74 ?? C5 F8 10 41 08 C5 F8 11 44 24 20 48 83 "
				"7C 24 20 00 75 08 48 83 7C 24 28 00 74 26 48 8B 0D ?? ?? ?? ?? 4C 8B C2 48 8D 54 24 "
				"20 E8 ?? ?? ?? ?? 48 8B 4C 24 30 48 33 CC E8 ?? ?? ?? ?? 48 83 C4 ?? C3 32 C0",
				// gp version
				"48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 33 "
				"C4 48 89 44 24 30 48 85 D2 75 ?? 32 C0"}
			},
			{
				"DSCollectorsItemSystemLoad",
				{"DSCollectorsItemSystemLoad",
				"48 89 5C 24 18 56 57 41 54 41 56 41 57 48 83 EC ?? 48 8B F1",
				// gp version
				"48 89 5C 24 18 55 56 57 41 54 41 55 48 8B EC"}
			},

			// In-game UI functions
			{
				"InGameUIUpdateStaticPoolCaller",
				{"InGameUIUpdateStaticPoolCaller",
				"44 89 4C 24 20 41 54 48 83 EC 40 48 89 74 24 60 4C "
				"8B E2 4C 89 74 24 28 4D 8B F0 48 8B F1 48 85 D2",
				// gp version
				"41 54 48 83 EC 60 48 89 74 24 50"}
			},
			{
				"AccessStaticUIPool",
				{"AccessStaticUIPool",
				"48 89 ?? 24 20 55 48 8D 6C 24 A9 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
				"48 33 C4 48 89 45 ?? 48 8B 41 50 48 8B ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9",
				// gp version (same)
				"48 89 ?? 24 20 55 48 8D 6C 24 A9 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
				"48 33 C4 48 89 45 ?? 48 8B 41 50 48 8B ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9"}
			},
			{
				"GetNotificationPool",
				{"GetNotificationPool",
				"48 83 EC 28 E8 ?? ?? ?? FF 48 8B 40 ?? 48 83 C4 28 C3 CC CC CC "
				"CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 56 57 41 54",
				// gp version
				"48 83 EC 28 E8 87 6C F7 FF 48 8B 40 48 48 83 C4 28 C3"}
			},
			{
				"DrawNotificationText",
				{"DrawNotificationText",
				"44 88 4C 24 20 44 89 44 24 18 48 89 54 24 10 53 55 "
				"56 41 54 41 55 41 56 41 57 48 83 EC ?? 44 8B B9",
				// gp version
				"48 8B C4 44 88 48 20 44 89 40 18 48 89 50 10 53"}
			},
			{
				"CreateRuntimeUITextFromString",
				{"CreateRuntimeUITextFromString",
				"40 53 48 83 EC 20 48 8B D9 48 C7 01 00 00 00 00 49 C7 C0 FF FF FF FF 66 "
				"0F 1F 84 00 00 00 00 00 49 FF C0 42 80 3C 02 00 75 F6 E8 51 08 00 00",
				// gp version
				"40 53 48 83 EC 20 48 8B D9 48 C7 01 00 00 00 00 49 C7 C0 FF FF FF FF 66 "
				"0F 1F 84 00 00 00 00 00 49 FF C0 42 80 3C 02 00 75 F6 E8 41 0C 00 00"}
			},
			{
				"TryFreeRuntimeUIText",
				{"TryFreeRuntimeUIText",
				"48 8B 09 B8 FF FF FF FF F0 0F C1 01 83 F8 01 0F 84 ?? ?? ?? ?? C3",
				// gp version (same)
				"48 8B 09 B8 FF FF FF FF F0 0F C1 01 83 F8 01 0F 84 ?? ?? ?? ?? C3"}
			},
			{
				"AssignRuntimeUIText",
				{"AssignRuntimeUIText",
				"48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 09 48 3B 0A",
				// gp version
				"48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 09 48 "
				"3B 0A 74 1C B8 FF FF FF FF F0 0F C1 01 83 F8 01 75 05 E8 65"}
			},
			{
				"UpdateRuntimeUIText",
				{"UpdateRuntimeUIText",
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 48 8B D9 48 "
				"8B FA 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 1F",
				// gp version
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 48 8B D9 48 "
				"8B FA 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 3C"}
			},
			{
				"InGameUIDrawElement",
				{"InGameUIDrawElement",
				"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? C5 F8 "
				"29 74 24 ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 ?? 48 63 EA",
				// gp version
				"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 "
				"48 83 EC 70 C5 F8 29 74 24 60 C5 F8 29 7C 24 50"}
			},
			{
				"InGameUIUpdateElement",
				{"InGameUIUpdateElement",
				"48 89 5C 24 08 57 48 83 EC 20 48 63 FA 48 "
				"8B D9 E8 ?? ?? ?? ?? 45 33 C0 4C 8B CF",
				// gp version (same)
				"48 89 5C 24 08 57 48 83 EC 20 48 63 FA 48 "
				"8B D9 E8 ?? ?? ?? ?? 45 33 C0 4C 8B CF"}
			},
			{
				"PostMenuExit",
				{"PostMenuExit",
				"40 57 48 83 EC 40 F6 41 64 02 48 8B F9 48 89 74 24 58 74 24 C5 "
				"F8 10 41 40 44 8B 49 60 48 8D 54 24 30 48 8B 49 08 41 B0 01",
				// gp version
				"4C 8B DC 57 48 81 EC B0 00 00 00 48 8B 05 1E 3C 1A 02"}
			},

			// Music player functions
			{
				"PlayMusic",
				{"PlayMusic",
				"48 85 D2 74 ?? 48 83 EC 58 48 8B 05 ?? ?? ?? "
				"?? 48 33 C4 48 89 44 24 40 4D 85 C0 74",
				// gp version (same)
				"48 85 D2 74 ?? 48 83 EC 58 48 8B 05 ?? ?? ?? "
				"?? 48 33 C4 48 89 44 24 40 4D 85 C0 74"}
			},
			{
				"GetSourcePlayPositions",
				{"GetSourcePlayPositions",
				"48 83 EC ?? 4D 85 C0 74 ?? 48 85 D2 75 ?? 41 39 10",
				// gp version (same)
				"48 83 EC ?? 4D 85 C0 74 ?? 48 85 D2 75 ?? 41 39 10"}
			},
			{
				"WwiseSourceCursorUpdate",
				{"WwiseSourceCursorUpdate",
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 "
				"41 54 41 55 41 56 41 57 48 83 EC ?? 48 8B F1 49",
				// gp version (same)
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 "
				"41 54 41 55 41 56 41 57 48 83 EC ?? 48 8B F1 49"}
			},
			{
				"PlayUISound",
				{"PlayUISound",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 "
				"EC 20 48 8B 01 41 0F B6 F1 41 8B D8 4C 8B F2 48 8B F9",
				// gp version
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 41 0F B6 F9"}
			},
			{
				"ShowMusicDescription",
				{"ShowMusicDescription",
				"40 53 56 57 48 81 EC 80 00 00 00 C5 F8 29 74 24 70 48 "
				"8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 8B D9 8B FA",
				// gp version
				"48 89 5C 24 20 57 48 81 EC 80 00 00 00 C5 F8 29 74 24 70 "
				"48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 8B D9 8B FA",
				true} // has AVX instructions in the prologue
			},
			{
				"ShowMusicDescriptionFromText",
				{"ShowMusicDescriptionFromText",
				"48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 "
				"81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 4C",
				// gp version
				"4C 8B DC 55 49 8D 6B ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 "
				"?? ?? ?? ?? 48 33 C4 48 89 45 ?? 49 89 5B ?? 8B D9 "}
			},
			{
				"ShowMusicDescriptionCore",
				{"ShowMusicDescriptionCore",
				"4C 8B DC 55 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? "
				"?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 73 F0 33 F6 4D 89 73 D0",
				// gp version
				"4C 8B DC 55 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 "
				"?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B 18 33 DB"}
			},
			{
				"ClearMusicDescriptionByType",
				{"ClearMusicDescriptionByType",
				"83 FA ?? 0F 87 ?? ?? ?? ?? 48 89 5C 24 18 56 48 83 EC ?? 48 63 C2 48 8B D9",
				// gp version
				"83 FA ?? 0F 87 ?? ?? ?? ?? 57 48 83 EC ?? 48 63 C2 41 0F B6 F8"}
			}
		};

		// Songs don't keep the same offsets inside this table across versions
		// (most likely because of new music added in DC)
		// Ended up going for a stable aob per audio
		/*std::string audioTableSig =
			"?? ?? ?? ?? ?? 7F 00 00 01 E6 29 73 28 09 48 F8 "
			"A3 26 69 44 AD 31 EC 71 ?? 00 00 00 00 00 00 00";*/

		// Music interruptors are sounds played through PlayMusic, that stop music in-game
		// Tracking them helps detect when autoplay should stop
		std::unordered_map<std::string, MusicData> interruptorDatabase = {
			{	// While "Silence" is an interruptor, it is also played when a song naturally ends
				// Ignore it and use other indicators, such as game state flags, to decide if playback should be interrupted
				"Silence",
				{0, MusicType::SFX, 0, "Silence", "",
				"?? ?? ?? ?? ?? 7F 00 00 E4 77 3B F0 67 6B 41 DB BB CC E6 75 2F 25 5B F2"}
			}
		};

		// Music-player-only songs use the override target at runtime.
		// Their Wwise ids/durations come from DSMusicPlayerTrackResource -> DSRaceMissionBGMResource.
		std::unordered_map<std::string, MusicData> songDatabase =
		{
			{
				"Music Pool Start",
				{0, MusicType::SONG, 0, "Music Pool Start", "",
				"?? ?? ?? ?? ?? 7F 00 00 01 E6 29 73 28 09 48 F8 A3 26 69 44 AD 31 EC 71"}
			},
			{
				"Don't Be So Serious",
				{21, MusicType::SONG, 0, "Don't Be So Serious", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 C4 BB 56 A4 5F B0 4A C5 A4 28 D8 65 FE 45 73 12"}
			},
			{
				"Bones",
				{22, MusicType::SONG, 0, "Bones", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 F4 66 B3 FE 15 0E 48 BB A6 AC 71 71 3B AF 57 AD"}
			},
			{
				"Poznan",
				{29, MusicType::SONG, 0, "Poznan", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 39 21 D5 77 DA 40 42 20 BF 2A 69 0E 39 F2 0C B8"}
			},
			{
				"Anything You Need",
				{15, MusicType::SONG, 0, "Anything You Need", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 5C 90 55 50 1B D7 4E 61 A0 91 6C 43 6A 44 84 ED"}
			},
			{
				"Easy Way Out",
				{17, MusicType::SONG, 0, "Easy Way Out", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 5A 5E D3 5D 0E 61 4E DB B1 79 2C 0A 29 20 89 15"}
			},
			{
				"I'm Leaving",
				{14, MusicType::SONG, 0, "I'm Leaving", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 98 79 47 30 CB 7F 43 89 B2 1D 0A 09 60 72 6C BB"}
			},
			{
				"Give Up",
				{1, MusicType::SONG, 0, "Give Up", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 5F 40 BB B1 59 CE 44 F7 BC FE 9A 0C 72 D3 A5 0F"}
			},
			{
				"Gosia",
				{27, MusicType::SONG, 0, "Gosia", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 79 BD 05 CA 2E 64 4C AA AD FE 21 1D B3 3E 68 67"}
			},
			{
				"Without You",
				{26, MusicType::SONG, 0, "Without You", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 4E 49 14 AD 62 2D 40 62 98 BB EB D5 2D 08 00 67"}
			},
			{
				"Breathe In",
				{11, MusicType::SONG, 0, "Breathe In", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 3B 21 F8 45 D5 7D 44 0A 82 DF 8E 51 AF 3E A0 43"}
			},
			{
				"Because We Have To",
				{4, MusicType::SONG, 0, "Because We Have To", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 BA 33 13 60 5F BE 40 CC 90 4E 38 10 45 E4 78 D3"}
			},
			{
				"St. Eriksplan",
				{23, MusicType::SONG, 0, "St. Eriksplan", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 05 9D DD E3 6F E8 42 FB 8C 94 6F F4 CA 30 1F 41"}
			},
			{
				"Rolling Over",
				{3, MusicType::SONG, 0, "Rolling Over", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 47 11 55 CA 04 45 45 8B A9 1E AD 54 3A 87 52 47"}
			},
			{
				"Once in a Long, Long While...",
				{28, MusicType::SONG, 0, "Once in a Long, Long While...", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 47 C3 A0 01 A5 D4 4F 92 84 B9 6A 27 19 B1 5B 0D"}
			},
			{
				"The Machine",
				{30, MusicType::SONG, 0, "The Machine", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 48 70 54 56 5B 66 4B BF B0 A3 B5 81 93 FE EB E7"}
			},
			{
				"Patience",
				{2, MusicType::SONG, 0, "Patience", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 39 0D 21 C8 64 76 43 15 9D 51 59 8C 3A 38 BA A0"}
			},
			{
				"Not Around",
				{24, MusicType::SONG, 0, "Not Around", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 59 39 6F 24 7E BC 42 50 BB A3 EC 0B 98 59 01 DC"}
			},
			{
				"Please Don't Stop (Chapter 1)",
				{13, MusicType::SONG, 0, "Please Don't Stop (Chapter 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 C4 9B 4D 1F 56 A2 4E 87 A7 B7 A7 18 1E DD F1 F8"}
			},
			{
				"Tonight, tonight, tonight",
				{5, MusicType::SONG, 0, "Tonight, tonight, tonight", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 55 1B 36 D4 F5 F2 4B A6 8F 2D 16 72 3B 67 92 36"}
			},
			{
				"Please Don't Stop (Chapter 2)",
				{16, MusicType::SONG, 0, "Please Don't Stop (Chapter 2)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 08 E1 2B 67 43 59 42 C7 93 DF 5A 7E 98 AF 0B DA"}
			},
			{
				"I'll Keep Coming",
				MakeInternalWwiseAreaTrack(
					12,
					352213,
					"I'll Keep Coming",
					"Low Roar",
					"?? ?? ?? ?? ?? 7F 00 00 14 D0 B6 9E C7 40 4C 0E A0 9C FB 04 D1 7C 3B B1",
					{1305110817u, 1055644723u, 650923206u, 382608620u, 439722904u, 5352569u, 0x00040001u}
				)
			},
			{
				"Half Asleep",
				{0, MusicType::SONG, 0, "Half Asleep", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 36 74 BE F1 54 90 40 B9 B5 7C 3D D5 52 8C BF FB"}
			},
			{
				"Waiting (10 Years)",
				{25, MusicType::SONG, 0, "Waiting (10 Years)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 93 B5 68 43 31 9A 43 87 B7 82 4A 97 B1 D1 81 E0"}
			},
			{
				"Nobody Else",
				{0, MusicType::SONG, 0, "Nobody Else", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 95 BE A0 E6 7E 52 44 C2 8C 3E AE 05 2C 87 83 4A"}
			},
			{
				"Asylums For The Feeling",
				{31, MusicType::SONG, 0, "Asylums For The Feeling", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 2F 3F E6 D6 4B C4 43 B0 B2 30 AF 27 AA FE 04 C0"}
			},
			{
				"Almost Nothing",
				{32, MusicType::SONG, 0, "Almost Nothing", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 A2 BA 0D EB 2A 00 4F 48 89 1C 5B D7 37 71 47 D2"}
			},
			{
				"BB's Theme",
				{35, MusicType::SONG, 0, "BB's Theme", "Ludvig Forssell",
				"?? ?? ?? ?? ?? 7F 00 00 F3 A0 F0 58 1A 9C 42 2F 8D 34 A7 E9 70 3A 45 23"}
			},
			{
				"Yellow Box",
				MakeInternalWwiseAreaTrack(
					40,
					181242,
					"Yellow Box",
					"The Neighbourhood",
					"?? ?? ?? ?? ?? 7F 00 00 FA 7F C9 EB 62 AB 43 86 BB F5 E8 A1 50 E3 18 E8",
					{2924515455u, 1012319143u, 671037301u, 487040596u, 547112959u, 2503595u, 0x00040001u}
				)
			},
			{
				"Ghost",
				MakeInternalWwiseAreaTrack(
					41,
					176797,
					"Ghost",
					"Alan Walker & Au/Ra",
					"?? ?? ?? ?? ?? 7F 00 00 2C 4A DD D9 B8 95 42 64 87 33 F5 C1 B9 0E CC F9",
					{598365015u, 1043875000u, 1047951530u, 829141283u, 898858101u, 2662798u, 0x00040001u}
				)
			},
			{
				"Trigger",
				MakeInternalWwiseAreaTrack(
					42,
					170982,
					"Trigger",
					"Khalid & Major Lazer",
					"?? ?? ?? ?? ?? 7F 00 00 0F 7D 12 60 75 31 40 A0 8F A6 05 9A AA 2A 16 5C",
					{1893238147u, 89519363u, 519029854u, 386911041u, 80774687u, 2611920u, 0x00040001u}
				)
			},
			{
				"Meanwhile... In Genova",
				MakeInternalWwiseAreaTrack(
					43,
					205158,
					"Meanwhile... In Genova",
					"The S.L.P.",
					"?? ?? ?? ?? ?? 7F 00 00 FC 1E B7 F3 84 6A 4D A3 89 84 B7 EB 07 DC 1D 48",
					{3371207578u, 1016826391u, 147702751u, 470414990u, 144885500u, 2747688u, 0x00040001u}
				)
			},
			{
				"Ludens",
				MakeInternalWwiseAreaTrack(
					44,
					276344,
					"Ludens",
					"Bring Me The Horizon",
					"?? ?? ?? ?? ?? 7F 00 00 CC 25 E3 36 66 05 42 88 84 92 14 CB FE B0 2C 8B",
					{856141473u, 237707466u, 1021953861u, 547593542u, 430772228u, 4393532u, 0x00040001u}
				)
			},
			{
				"Pop Virus",
				MakeInternalWwiseAreaTrack(
					50,
					181533,
					"Pop Virus",
					"Gen Hoshino",
					"?? ?? ?? ?? ?? 7F 00 00 23 A9 DD 3C 14 F4 49 24 BA 14 B4 B0 51 ED C7 E7",
					{391919030u, 934080991u, 670119032u, 956497239u, 798253428u, 2457847u, 0x00040001u}
				)
			},
			{
				"Car Go Fast",
				MakeInternalWwiseAreaTrack(
					60,
					325000,
					"Car Go Fast",
					"Ludvig Forssell",
					"?? ?? ?? ?? ?? 7F 00 00 D0 67 2D F6 86 06 42 12 B4 ED E0 21 AE CB 62 1E",
					{1284342117u, 53356964u, 215126431u, 267299658u, 251243249u, 10001440u, 0x00040001u},
					true
				)
			},
			{
				"Over The Threshold",
				MakeInternalWwiseAreaTrack(
					61,
					319000,
					"Over The Threshold",
					"Ludvig Forssell",
					"?? ?? ?? ?? ?? 7F 00 00 6D CC 12 4F FC 65 47 1F 94 F1 B6 99 F7 3B 84 08",
					{3913561973u, 1006104337u, 826687163u, 423182524u, 1008401873u, 6580285u, 0x00040001u},
					true
				)
			},
			{
				"Truckin'",
				MakeInternalWwiseAreaTrack(
					62,
					193000,
					"Truckin'",
					"Ludvig Forssell",
					"?? ?? ?? ?? ?? 7F 00 00 8C 1A 5C 05 28 8A 40 A8 99 C3 3E 0B 9D 25 5F 2F",
					{2233763949u, 830860311u, 282292946u, 542579712u, 198734754u, 4480029u, 0x00040001u},
					true
				)
			},
			{
				"UCA Pacific Highway 46",
				MakeInternalWwiseAreaTrack(
					63,
					247000,
					"UCA Pacific Highway 46",
					"Ludvig Forssell",
					"?? ?? ?? ?? ?? 7F 00 00 75 49 E4 E7 F3 0E 46 5B BC B8 1A 0E 25 0D DF 44",
					{3276989892u, 983148444u, 518357792u, 62764114u, 140915981u, 6009723u, 0x00040001u},
					true
				)
			},
			{
				"Highways",
				MakeInternalWwiseAreaTrack(
					64,
					164000,
					"Highways",
					"Ludvig Forssell",
					"?? ?? ?? ?? ?? 7F 00 00 A8 E1 13 25 10 74 45 51 A5 2C D0 D7 92 2D C0 C5",
					{2675753311u, 505072956u, 191416213u, 90472243u, 212867915u, 5245015u, 0x00040001u},
					true
				)
			},
			{
				"Alone",
				MakeInternalWwiseAreaTrack(
					101,
					180000,
					"Alone",
					"Biting Elbows",
					"?? ?? ?? ?? ?? 7F 00 00 D8 C7 3D 56 06 D1 4A 0B 82 7E 41 9B 0E 35 72 1A",
					{1540271023u, 78924567u, 27281959u, 953843384u, 646208726u, 2446760u, 0x00040001u},
					true
				)
			},
			{
				"Path",
				MakeInternalWwiseAreaTrack(
					106,
					186000,
					"Path",
					"Apocalyptica",
					"?? ?? ?? ?? ?? 7F 00 00 44 4B D7 F2 70 CF 41 39 B4 58 72 03 78 90 C1 92",
					{2079558722u, 408002769u, 991931525u, 345109632u, 23355226u, 2763179u, 0x00040001u},
					true
				)
			},
			{
				"Path Vol. 2",
				MakeInternalWwiseAreaTrack(
					107,
					204000,
					"Path Vol. 2",
					"Apocalyptica",
					"?? ?? ?? ?? ?? 7F 00 00 36 12 05 30 2D 17 4D 80 9E E9 A5 02 48 7E 9E 9C",
					{1669624497u, 215763671u, 711699113u, 1056922396u, 1007972598u, 3221381u, 0x00040001u},
					true
				)
			},

			// Director's cut exclusives
			{
				"Pale Yellow",
				{104, MusicType::SONG, 0, "Pale Yellow", "Woodkid",
				"?? ?? ?? ?? ?? 7F 00 00 56 1A C5 A0 0F 5E 45 DE 86 02 71 4E 23 70 4C B8", true} // true for DC exclusive
			},
			{
				"Goliath",
				{105, MusicType::SONG, 0, "Goliath", "Woodkid",
				"?? ?? ?? ?? ?? 7F 00 00 A0 E8 B6 72 C9 60 41 76 87 A7 B6 98 0B 1A E9 40", true}
			},
			{
				"Control",
				{102, MusicType::SONG, 0, "Control", "Biting Elbows",
				"?? ?? ?? ?? ?? 7F 00 00 7B A6 DF 4F 6C D4 4C 7D 86 50 F5 39 32 00 54 DC", true}
			},
			{
				"Other Me",
				{103, MusicType::SONG, 0, "Other Me", "Biting Elbows",
				"?? ?? ?? ?? ?? 7F 00 00 3D 21 BE 31 6B BC 42 4A 96 03 04 9B F8 0B 58 F9", true}
			},
			{
				"Fragile",
				{100, MusicType::SONG, 0, "Fragile", "Midge Ure",
				"?? ?? ?? ?? ?? 7F 00 00 FE D0 78 11 11 50 43 8F BB 02 5C 1F E4 6F 81 48", true}
			},

			// Ambient music, max duration is set to 3 minutes (180000 ms)
			//{
			//	"Ambient 1",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 1", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 3B AE 7B 3A EE 91 42 1D 8B 6B BB 93 4D C7 D7 1A"}
			//},
			//{
			//	"Ambient 2",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 2", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 73 8E 1B C3 D5 54 4C 3C 9C 37 19 87 70 B1 AF 7C"}
			//},
			//{
			//	"Ambient 3",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 3", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 8D 28 ED 0A C2 44 44 E4 9B 00 BD 80 4E A9 ED 57"}
			//},
			//{
			//	"Ambient 4",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 4", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 95 B0 41 AE 75 8E 49 A4 8B E1 54 15 E9 91 69 93"}
			//},
			//{
			//	"Ambient 5",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 5", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 A6 0B 21 84 06 51 4E 8C A1 F6 8B 01 2E 73 D1 C9"}
			//},
			//{
			//	"Ambient 6", // sound stops a bit earlier than 3 minutes
			//	{0, MusicType::AMBIENT, 170000, "Ambient 6", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 C2 9E 42 90 99 DE 44 A0 8F 13 02 EC 23 F3 46 A2"}
			//},
			//{
			//	"Ambient 7",
			//	{0, MusicType::AMBIENT, 180000, "Ambient 7", "",
			//	"?? ?? ?? ?? ?? 7F 00 00 CC D1 13 95 96 1E 40 9C BB 72 69 C7 C0 A1 B5 60"}
			//},

			// Others
			{
				"Almost Nothing (No Beatbox)",
				{32, MusicType::SONG, 0, "Almost Nothing (No Beatbox)", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 EF E6 84 B0 5C 9F 4B BE 91 B7 AE F9 06 BD 12 FF"}
			},
			/*{
				"Almost Nothing (Instrumental)",
				{32, MusicType::SONG, 0, "Almost Nothing (Instrumental)", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 CE 47 2C 1F 52 25 45 56 B2 31 8B 0E 20 BF 6B 08"}
			},
			{
				"Patience (duplicate 1)",
				{2, MusicType::SONG, 0, "Patience (duplicate 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 41 F4 FC DC 55 E0 4F 80 AB 62 8D 90 1C 10 A4 DC"}
			},
			{
				"Rolling Over (duplicate 1)",
				{3, MusicType::SONG, 0, "Rolling Over (duplicate 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 61 7D F8 24 AB 7F 49 09 9D 08 43 78 A8 E8 A2 62"}
			},
			{
				"Bones (duplicate 1)",
				{22, MusicType::SONG, 0, "Bones (duplicate 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 A2 FC AC AF D8 D6 4E DA A7 AD D7 C0 49 E4 DA 08"}
			},
			{
				"Without You (duplicate 1)",
				{26, MusicType::SONG, 0, "Without You (duplicate 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 BE 6E E1 F0 85 9D 47 E2 84 54 7C DF DA C6 4C D7"}
			},
			{
				"Bones (end 1)",
				{22, MusicType::SONG, 0, "Bones (end 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 D6 DE B6 F0 6D 11 43 66 97 1E C1 51 E0 CE D6 E5"}
			},
			{
				"Bones (end 2)",
				{22, MusicType::SONG, 0, "Bones (end 2)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 EC D7 93 C5 55 74 4A A2 88 37 07 66 55 87 08 CC"}
			},
			{
				"Because We Have To (duplicate 1)",
				{4, MusicType::SONG, 0, "Because We Have To (duplicate 1)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 F3 03 BC FF 29 17 4C 8F 81 ED F1 D0 76 F1 C8 A7"}
			},
			{
				"Bones (duplicate 2)",
				{22, MusicType::SONG, 0, "Bones (duplicate 2)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 6F 4A B1 53 CE 04 45 9D 81 C6 3E BA C8 CA 33 C8"}
			},
			{
				"Bones (duplicate 3)",
				{22, MusicType::SONG, 0, "Bones (duplicate 3)", "Low Roar",
				"?? ?? ?? ?? ?? 7F 00 00 F9 11 18 09 E1 ED 40 BF 89 71 17 49 FC 70 D8 15"}
			},*/
		};

		std::unordered_map<std::string, MusicUnlockFactData> musicUnlockFactDatabase =
		{
			{"Ghost", {"AA 64 2A 13 75 37 4E EC 96 11 F0 D4 C7 CE 85 7A", false}},
			{"Pop Virus", {"03 97 B1 D8 C5 7F 41 C6 92 D8 39 70 21 BB 28 8B", false}},
			{"Bones", {"4B BA 20 86 CD DB 4F AB A5 AD A9 BB 6D 5D 8A E1", false}},
			{"Goliath", {"8E 2A 8D E6 30 E4 45 9A A8 C4 97 D6 79 8C 56 2C", true}},
			{"Alone", {"E6 3E 18 F2 B3 18 42 96 96 9A C1 49 22 84 15 3A", true}},
			{"Yellow Box", {"22 48 09 F5 B4 16 4D E6 BB D1 60 6A D0 48 FE D8", false}},
			{"Asylums For The Feeling", {"C3 F0 20 00 F0 DC 4B F6 A1 AD 9D 2F D1 38 6B D0", false}},
			{"Without You", {"58 44 7C 78 20 2C 41 91 90 77 80 97 CD 8D 2B 52", false}},
			{"Ludens", {"3C 49 A1 FC 6D 03 4E AB BC AC 21 4E E2 33 16 B1", false}},
			{"I'm Leaving", {"43 34 AD DD 6F 73 47 D1 A6 A1 88 88 3A 33 D1 5C", false}},
			{"Patience", {"2A A7 1E AE E5 EA 44 07 95 C7 07 4A 38 AE E4 78", false}},
			{"Please Don't Stop (Chapter 2)", {"A9 8D 85 16 BA F1 49 DB A7 27 60 BB 70 D6 D3 77", false}},
			{"Breathe In", {"12 E5 9D F8 33 9F 42 7C B5 F1 15 D0 63 A8 3C A8", false}},
			{"The Machine", {"BD CD CD 65 14 58 4B B1 B4 A6 39 5E 92 C3 24 A9", false}},
			{"Because We Have To", {"0E 74 8B 24 AD C2 49 EF B1 DB F8 C9 3B B3 B3 85", false}},
			{"Gosia", {"89 EE 71 32 33 B5 41 24 93 4A D4 A7 B9 BC 11 5B", false}},
			{"Don't Be So Serious", {"51 2A 2F B0 4A B8 42 B2 AC CC 27 A4 81 DF 2D E7", false}},
			{"St. Eriksplan", {"B4 E0 F5 9E 0F 7B 4B 7F 88 DD E3 DD FF EB 7F BC", false}},
			{"Once in a Long, Long While...", {"C9 DA 96 1F 3C 21 49 24 B4 9A 45 0A 20 0C 29 AA", false}},
			{"Waiting (10 Years)", {"D9 F6 7A 9C D8 AC 4A 0A AB 04 39 D8 5D 4D C3 F6", false}},
			{"Not Around", {"3A 64 99 79 1B 6D 4E 41 BB 31 34 2D 9D F3 9C D4", false}},
			{"Give Up", {"CC 7C 0A CA 58 F6 48 41 93 9C 6D 41 6E 20 64 E0", false}},
			{"Control", {"D6 57 3F CA 3A 13 4C 9F 81 D9 06 77 CC 54 84 44", true}},
			{"Please Don't Stop (Chapter 1)", {"65 F0 E7 DD CD 5D 49 1E 9C AA A3 4F 36 26 54 CD", false}},
			{"Trigger", {"A3 FE B4 41 FF 55 41 06 84 D3 2B 4D 5C 88 75 A7", false}},
			{"BB's Theme", {"60 44 50 AA DD DF 44 44 8F 7F FE 3E F1 9B 79 1C", false}},
			{"Path", {"26 01 39 E9 1F 67 47 2E B1 8B FF 6B FE 89 94 11", true}},
			{"Poznan", {"90 10 BD 04 FE ED 4A 60 A8 4A 01 EB 3F E5 B2 38", false}},
			{"Path Vol. 2", {"FC 4D 2F 10 19 A0 43 1D 9F C2 DC 57 55 36 7D 16", true}},
			{"Tonight, tonight, tonight", {"1C 5A BA 7E 56 20 46 E8 95 61 62 1D 8E 53 38 3C", false}},
			{"Rolling Over", {"66 8A 0F CD 48 3C 4D 61 A5 20 7C 3D 50 75 74 BD", false}},
			{"Pale Yellow", {"39 52 16 29 44 2B 4B B6 8D 5F 48 DC 53 97 0E 81", true}},
			{"Almost Nothing", {"85 DF 6C BC 5A 5B 43 F1 BB D4 80 E5 77 78 D5 DF", false}},
			{"Fragile", {"85 AB 59 93 A9 35 4C E9 99 75 5E 1A 2F F9 83 E5", true}},
			{"Anything You Need", {"92 D8 CD 0D 7F 78 42 BF 99 79 39 47 76 B1 F8 D4", false}},
			{"Easy Way Out", {"74 56 70 68 77 02 44 F1 8C 50 21 F4 B7 D9 B3 27", false}},
			{"Other Me", {"83 FD E6 CB 23 EF 46 97 8E AF 99 22 E8 4B 17 F0", true}},
			{"I'll Keep Coming", {"09 82 18 14 43 DA 4F C7 9C EF FA CC 07 81 8F 3F", false}},
			{"Meanwhile... In Genova", {"E9 8E E4 63 76 8B 49 C3 84 33 D1 2C F1 64 FC 7D", false}},
		};

		std::unordered_map<std::string, MusicData> customSongDatabase = {};

		// Basically similar to interruptorDatabase, but these are UI sounds that indicate a music interruption
		// We don't scan for these, instead signature is matched again PlayUISound function arg2 bytes
		std::unordered_map<std::string, MusicData> interruptorUIDatabase = {
			{
				"Entering facility",
				{0, MusicType::UNKNOWN, 0, "Entering facility", "",
				"00 00 00 00 01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00 "
				"FF FF FF FF 05 00 00 00 FF FF FF FF 06 00 00 00 FF FF FF FF"}
			},
			{
				"Entering facility 2",
				{0, MusicType::UNKNOWN, 0, "Entering facility", "",
				"02 00 00 00 03 00 00 00 04 00 00 00 FF FF FF FF "
				"05 00 00 00 FF FF FF FF 06 00 00 00 FF FF FF FF"}
			},
			{
				"Entering facility 3",
				{0, MusicType::UNKNOWN, 0, "Entering facility", "",
				"03 00 00 00 04 00 00 00 FF FF FF FF 05 00 "
				"00 00 FF FF FF FF 06 00 00 00 FF FF FF FF"}
			}
		};

		std::unordered_map<std::string, LocalizedText> localizedTextDatabase =
		{
			{"Play Music", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Play Music"},
				{TextLanguage::ENGLISH_UK, u"Play Music"},
				{TextLanguage::FRENCH, u"Jouer la musique"},
				{TextLanguage::ITALIAN, u"Riproduci musica"},
				{TextLanguage::GERMAN, u"Musik spielen"},
				{TextLanguage::SPANISH_SPAIN, u"Reproducir música"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Reproduzir música"},
				{TextLanguage::GREEK, u"Αναπαραγωγή μουσικής"},
				{TextLanguage::POLISH, u"Odtwórz muzykę"},
				{TextLanguage::RUSSIAN, u"Включить плеер"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Reproducir música"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Reproduzir música"},
				{TextLanguage::JAPANESE, u"音楽を再生"},
				{TextLanguage::ARABIC, u"تشغيل الموسيقى"},
				{TextLanguage::DUTCH, u"Muziek afspelen"},
				{TextLanguage::CZECH, u"Přehrát hudbu"},
				{TextLanguage::TURKISH, u"Müzik Çal"},
				{TextLanguage::HUNGARIAN, u"Zene lejátszása"},
				{TextLanguage::KOREAN, u"음악 재생"},
				{TextLanguage::CHINESE_TRADITIONAL, u"播放音樂"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"播放音乐"},
			}}},
			{"Stop Music", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Stop Music"},
				{TextLanguage::ENGLISH_UK, u"Stop Music"},
				{TextLanguage::FRENCH, u"Arrêter la musique"},
				{TextLanguage::ITALIAN, u"Ferma musica"},
				{TextLanguage::GERMAN, u"Musik stoppen"},
				{TextLanguage::SPANISH_SPAIN, u"Detener música"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Parar música"},
				{TextLanguage::GREEK, u"Διακοπή μουσικής"},
				{TextLanguage::POLISH, u"Zatrzymaj muzykę"},
				{TextLanguage::RUSSIAN, u"Остановить плеер"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Detener música"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Parar música"},
				{TextLanguage::JAPANESE, u"音楽を停止"},
				{TextLanguage::ARABIC, u"إيقاف الموسيقى"},
				{TextLanguage::DUTCH, u"Muziek stoppen"},
				{TextLanguage::CZECH, u"Zastavit hudbu"},
				{TextLanguage::TURKISH, u"Müziği Durdur"},
				{TextLanguage::HUNGARIAN, u"Zene leállítása"},
				{TextLanguage::KOREAN, u"음악 중지"},
				{TextLanguage::CHINESE_TRADITIONAL, u"停止音樂"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"停止音乐"},
			}}},
			{"Loop Mode (All)", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Loop Mode (All)"},
				{TextLanguage::ENGLISH_UK, u"Loop Mode (All)"},
				{TextLanguage::FRENCH, u"Mode boucle (Tout)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Tutti)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Alle)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Todos)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Todos)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (όλα)"},
				{TextLanguage::POLISH, u"Tryb pętli (Wszystkie)"},
				{TextLanguage::RUSSIAN, u"Режим повтора (Плейлист)"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Modo bucle (Todos)"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Modo Loop (Todos)"},
				{TextLanguage::JAPANESE, u"ループモード (すべて)"},
				{TextLanguage::ARABIC, u"وضع التكرار (كل المقاطع)"},
				{TextLanguage::DUTCH, u"Lusmodus (Alle)"},
				{TextLanguage::CZECH, u"Režim smyčky (Všechny)"},
				{TextLanguage::TURKISH, u"Döngü Modu (Tümü)"},
				{TextLanguage::HUNGARIAN, u"Loop mód (Összes)"},
				{TextLanguage::KOREAN, u"루프 모드 (모두)"},
				{TextLanguage::CHINESE_TRADITIONAL, u"循環模式 (全部)"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"循环模式 (全部)"},
			}}},
			{"Loop Mode (One)", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Loop Mode (One)"},
				{TextLanguage::ENGLISH_UK, u"Loop Mode (One)"},
				{TextLanguage::FRENCH, u"Mode boucle (Seul)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Uno)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Eins)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Uno)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Um)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (μία)"},
				{TextLanguage::POLISH, u"Tryb pętli (Jeden)"},
				{TextLanguage::RUSSIAN, u"Режим повтора (Трек)"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Modo bucle (Uno)"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Modo Loop (Um)"},
				{TextLanguage::JAPANESE, u"ループモード (1曲)"},
				{TextLanguage::ARABIC, u"وضع التكرار (نفس المقطع)"},
				{TextLanguage::DUTCH, u"Lusmodus (Eén)"},
				{TextLanguage::CZECH, u"Režim smyčky (Jedna)"},
				{TextLanguage::TURKISH, u"Döngü Modu (Bir)"},
				{TextLanguage::HUNGARIAN, u"Loop mód (Egy)"},
				{TextLanguage::KOREAN, u"루프 모드 (하나)"},
				{TextLanguage::CHINESE_TRADITIONAL, u"循環模式 (1)"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"循环模式 (1)"},
			}}},
			{"Loop Mode (None)", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Loop Mode (None)"},
				{TextLanguage::ENGLISH_UK, u"Loop Mode (None)"},
				{TextLanguage::FRENCH, u"Mode boucle (Aucun)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Nessuna)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Kein)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Ninguno)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Nenhum)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (Καμία)"},
				{TextLanguage::POLISH, u"Tryb pętli (Brak)"},
				{TextLanguage::RUSSIAN, u"Режим повтора (Выкл)"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Modo bucle (Ninguno)"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Modo Loop (Nenhum)"},
				{TextLanguage::JAPANESE, u"ループモード (なし)"},
				{TextLanguage::ARABIC, u"وضع التكرار (دون تكرار)"},
				{TextLanguage::DUTCH, u"Lusmodus (Geen)"},
				{TextLanguage::CZECH, u"Režim smyčky (Žádná)"},
				{TextLanguage::TURKISH, u"Döngü Modu (Hiçbiri)"},
				{TextLanguage::HUNGARIAN, u"Loop mód (Nincs)"},
				{TextLanguage::KOREAN, u"루프 모드 (없음)"},
				{TextLanguage::CHINESE_TRADITIONAL, u"循環模式 (無)"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"循环模式 (无)"},
			}}},
			{"Shuffle Playlist", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Shuffle Playlist"},
				{TextLanguage::ENGLISH_UK, u"Shuffle Playlist"},
				{TextLanguage::FRENCH, u"Mélanger la playlist"},
				{TextLanguage::ITALIAN, u"Playlist casuale"},
				{TextLanguage::GERMAN, u"Zufällige wiedergabeliste"},
				{TextLanguage::SPANISH_SPAIN, u"Lista aleatoria"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Embaralhar playlist"},
				{TextLanguage::GREEK, u"Τυχαία λίστα αναπαραγωγής"},
				{TextLanguage::POLISH, u"Wymieszaj playlistę"},
				{TextLanguage::RUSSIAN, u"Перемешать плейлист"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Lista aleatoria"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Embaralhar lista de reprodução"},
				{TextLanguage::JAPANESE, u"プレイリストをシャッフル"},
				{TextLanguage::ARABIC, u"قائمة تشغيل عشوائية"},
				{TextLanguage::DUTCH, u"Shuffle afspeellijst"},
				{TextLanguage::CZECH, u"Náhodný seznam skladeb"},
				{TextLanguage::TURKISH, u"Çalma Listesini Karıştır"},
				{TextLanguage::HUNGARIAN, u"Lejátszási lista keverése"},
				{TextLanguage::KOREAN, u"재생 목록 셔플"},
				{TextLanguage::CHINESE_TRADITIONAL, u"隨機播放清單"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"随机播放清单"},
			}}},
			{"Reset Playlist", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Reset Playlist"},
				{TextLanguage::ENGLISH_UK, u"Reset Playlist"},
				{TextLanguage::FRENCH, u"Réinitialiser la playlist"},
				{TextLanguage::ITALIAN, u"Ripristina playlist"},
				{TextLanguage::GERMAN, u"Wiedergabeliste zurücksetzen"},
				{TextLanguage::SPANISH_SPAIN, u"Restablecer lista"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Redefinir playlist"},
				{TextLanguage::GREEK, u"Επαναφορά λίστας αναπαραγωγής"},
				{TextLanguage::POLISH, u"Resetuj playlistę"},
				{TextLanguage::RUSSIAN, u"Сбросить плейлист"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Restablecer lista"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Redefinir lista de reprodução"},
				{TextLanguage::JAPANESE, u"プレイリストをリセット"},
				{TextLanguage::ARABIC, u"إعادة تعيين قائمة التشغيل"},
				{TextLanguage::DUTCH, u"Reset afspeellijst"},
				{TextLanguage::CZECH, u"Obnovení seznamu skladeb"},
				{TextLanguage::TURKISH, u"Çalma Listesini Sıfırla"},
				{TextLanguage::HUNGARIAN, u"Lejátszási lista visszaállítása"},
				{TextLanguage::KOREAN, u"재생 목록 재설정"},
				{TextLanguage::CHINESE_TRADITIONAL, u"重設播放清單"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"重设播放清单"},
			}}},

			{"Threat nearby: music player deactivated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Threat nearby: music player deactivated."},
				{TextLanguage::ENGLISH_UK, u"Threat nearby: music player deactivated."},
				{TextLanguage::FRENCH, u"Menace à proximité: lecteur de musique désactivé."},
				{TextLanguage::ITALIAN, u"Minaccia vicina: lettore musicale disattivato."},
				{TextLanguage::GERMAN, u"Bedrohung in der Nähe: Musik-Player deaktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Amenaza cercana: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Ameaça próxima: leitor de música desativado."},
				{TextLanguage::GREEK, u"Κοντινή απειλή: η αναπαραγωγή μουσικής απενεργοποιήθηκε."},
				{TextLanguage::POLISH, u"Niebezpieczeństwo w pobliżu: wyłączony odtwarzacz muzyki."},
				{TextLanguage::RUSSIAN, u"Угроза рядом: музыкальный плеер недоступен."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Amenaza cercana: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Ameaça próxima: reprodutor de música desativado."},
				{TextLanguage::JAPANESE, u"脅威接近：ミュージックプレーヤーが無効化されました。"},
				{TextLanguage::ARABIC, u"خطر قريب: تم تعطيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Bedreiging dichtbij: muziekspeler gedeactiveerd."},
				{TextLanguage::CZECH, u"Ohrožení v blízkosti: hudební přehrávač deaktivován."},
				{TextLanguage::TURKISH, u"Tehdit yakın: müzik çalar devre dışı."},
				{TextLanguage::HUNGARIAN, u"Fenyegetés a közelben: a zenelejátszó kikapcsolt."},
				{TextLanguage::KOREAN, u"주변 위협: 음악 플레이어가 비활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"威脅臨近：音樂播放器已停用。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"威胁临近：音乐播放器已停用。"},
			}}},
			{"Threat cleared: music player activated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Threat cleared: music player activated."},
				{TextLanguage::ENGLISH_UK, u"Threat cleared: music player activated."},
				{TextLanguage::FRENCH, u"Menace écartée: lecteur de musique activé."},
				{TextLanguage::ITALIAN, u"Minaccia sventata: lettore musicale riattivato."},
				{TextLanguage::GERMAN, u"Bedrohung beseitigt: Musik-Player aktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Amenaza despejada: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Ameaça evitada: leitor de música ativado."},
				{TextLanguage::GREEK, u"Εκκαθάριση απειλής: η συσκευή αναπαραγωγής μουσικής ενεργοποιήθηκε."},
				{TextLanguage::POLISH, u"Usunięto zagrożenie: odtwarzacz muzyki aktywowany."},
				{TextLanguage::RUSSIAN, u"Угрозы нет: музыкальный плеер активирован."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Amenaza despejada: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Ameaça eliminada: reprodutor de música ativado."},
				{TextLanguage::JAPANESE, u"脅威クリア：ミュージックプレーヤーが有効化されました。"},
				{TextLanguage::ARABIC, u"نهاية الخطر: تم تفعيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Bedreiging geweken: muziekspeler geactiveerd."},
				{TextLanguage::CZECH, u"Hrozba odstraněna: hudební přehrávač aktivován."},
				{TextLanguage::TURKISH, u"Tehdit temizlendi: müzik çalar etkinleştirildi."},
				{TextLanguage::HUNGARIAN, u"Fenyegetés elhárítva: a zenelejátszó aktiválva."},
				{TextLanguage::KOREAN, u"위협이 제거되었습니다: 음악 플레이어가 활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"威脅消失：音樂播放器已啟動。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"威胁消失：音乐播放器已启动。"},
			}}},
			{"Chiral network off: music player deactivated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Chiral network off: music player deactivated."},
				{TextLanguage::ENGLISH_UK, u"Chiral network off: music player deactivated."},
				{TextLanguage::FRENCH, u"Déconnecté du réseau chiral: lecteur de musique désactivé."},
				{TextLanguage::ITALIAN, u"Rete chirale spenta: lettore musicale disattivato."},
				{TextLanguage::GERMAN, u"Chiral‑Netz aus: Musik‑Player deaktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Red quiral desactivada: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Rede quiral desligada: leitor de música desativado."},
				{TextLanguage::GREEK, u"Δίκτυο απενεργοποιημένο: παίκτης μουσικής απενεργοποιήθηκε."},
				{TextLanguage::POLISH, u"Sieć wyłączona: odtwarzacz muzyki wyłączony."},
				{TextLanguage::RUSSIAN, u"Хиральная сеть отключена: музыкальный плеер недоступен."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Red quiral desactivada: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Rede quiral desativada: reprodutor de música desativado."},
				{TextLanguage::JAPANESE, u"キラルネットワークオフ：ミュージックプレーヤーが無効化されました。"},
				{TextLanguage::ARABIC, u"الشبكة الكيرالية متوقفة: تم تعطيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Chiraal netwerk uit: muziekspeler gedeactiveerd."},
				{TextLanguage::CZECH, u"Chirální síť deaktivována: hudební přehrávač deaktivován."},
				{TextLanguage::TURKISH, u"Ağ kapalı: müzik çalar devre dışı."},
				{TextLanguage::HUNGARIAN, u"Hálózat letiltva: a zenelejátszó le van tiltva."},
				{TextLanguage::KOREAN, u"네트워크 꺼짐: 음악 플레이어가 비활성화됩니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"網路關閉：音樂播放器已停用。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"网络关闭：音乐播放器已停用。"},
			}}},
			{"Chiral network on: music player activated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Chiral network on: music player activated."},
				{TextLanguage::ENGLISH_UK, u"Chiral network on: music player activated."},
				{TextLanguage::FRENCH, u"Connecté au réseau chiral: lecteur de musique activé."},
				{TextLanguage::ITALIAN, u"Rete chirale attiva: lettore musicale attivato."},
				{TextLanguage::GERMAN, u"Chiral‑Netz an: Musik‑Player aktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Red quiral activa: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Rede quiral ligada: leitor de música ativado."},
				{TextLanguage::GREEK, u"Δίκτυο ενεργοποιημένο: η συσκευή αναπαραγωγής μουσικής ενεργοποιήθηκε."},
				{TextLanguage::POLISH, u"Sieć aktywna: odtwarzacz muzyki aktywowany."},
				{TextLanguage::RUSSIAN, u"Хиральная сеть подключена: музыкальный плеер активирован."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Red quiral encendida: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Rede quiral ligada: reprodutor de música ativado."},
				{TextLanguage::JAPANESE, u"キラルネットワークオン：ミュージックプレーヤーが有効化されました。"},
				{TextLanguage::ARABIC, u"الشبكة الكيرالية مفعّلة: تم تفعيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Chiraal netwerk aan: muziekspeler geactiveerd."},
				{TextLanguage::CZECH, u"Chirální síť povolena: hudební přehrávač aktivován."},
				{TextLanguage::TURKISH, u"Ağ açık: müzik çalar etkinleştirildi."},
				{TextLanguage::HUNGARIAN, u"Hálózat engedélyezve: a zenelejátszó aktiválva."},
				{TextLanguage::KOREAN, u"네트워크 켜짐: 음악 플레이어가 활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"已啟用網路：音樂播放器已啟動。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"网络已启用：音乐播放器已启动。"},
			}}},
			{"Entering facility: music player deactivated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Entering facility: music player deactivated."},
				{TextLanguage::ENGLISH_UK, u"Entering facility: music player deactivated."},
				{TextLanguage::FRENCH, u"Accès à l'installation: lecteur de musique désactivé."},
				{TextLanguage::ITALIAN, u"Entrata in struttura: riproduzione musicale disattivata."},
				{TextLanguage::GERMAN, u"Einrichtung betreten: Musik-Player deaktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Entrando en la instalación: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Entrar na instalação: leitor de música desativado."},
				{TextLanguage::GREEK, u"Είσοδος στους χώρους: αναπαραγωγή μουσικής απενεργοποιημένη."},
				{TextLanguage::POLISH, u"Wejście do obiektu: odtwarzacz muzyki dezaktywowany."},
				{TextLanguage::RUSSIAN, u"Вход на объект: музыкальный плеер деактивирован."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Entrando en la instalación: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Entrando nas instalações: reprodutor de música desativado."},
				{TextLanguage::JAPANESE, u"施設に入る：音楽プレーヤーが無効化されました。"},
				{TextLanguage::ARABIC, u"دخول المنشأة: تم تعطيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Binnengaan faciliteit: muziekspeler uitgeschakeld."},
				{TextLanguage::CZECH, u"Přístup k zařízení: hudební přehrávač deaktivován."},
				{TextLanguage::TURKISH, u"Tesise giriliyor: müzik çalar devre dışı bırakıldı."},
				{TextLanguage::HUNGARIAN, u"Hozzáférés a létesítményhez: zenelejátszó deaktiválva."},
				{TextLanguage::KOREAN, u"시설 진입 중: 음악 플레이어가 비활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"進入設施：音樂播放器已停用。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"进入设施：音乐播放器已停用。"},
			}}},
			{"Exiting facility: music player activated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Exiting facility: music player activated."},
				{TextLanguage::ENGLISH_UK, u"Exiting facility: music player activated."},
				{TextLanguage::FRENCH, u"Sortie de l'installation: lecteur de musique activé."},
				{TextLanguage::ITALIAN, u"Uscita dalla struttura: lettore musicale attivato."},
				{TextLanguage::GERMAN, u"Einrichtung verlassen: Musik-Player aktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Saliendo de la instalación: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Sair da instalação: leitor de música ativado."},
				{TextLanguage::GREEK, u"Έξοδος από τους χώρους: αναπαραγωγή μουσικής ενεργοποιημένη."},
				{TextLanguage::POLISH, u"Wyjście z obiektu: odtwarzacz muzyki aktywowany."},
				{TextLanguage::RUSSIAN, u"Выход из объекта: музыкальный плеер активирован."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Saliendo de la instalación: reproductor de música activado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Sair das instalações: reprodutor de música ativado."},
				{TextLanguage::JAPANESE, u"施設から出る：音楽プレーヤーが有効化されました。"},
				{TextLanguage::ARABIC, u"خروج من المنشأة: تم تفعيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Faciliteit verlaten: muziekspeler geactiveerd."},
				{TextLanguage::CZECH, u"Opouští zařízení: hudební přehrávač aktivován."},
				{TextLanguage::TURKISH, u"Tesisten çıkılıyor: müzik çalar etkinleştirildi."},
				{TextLanguage::HUNGARIAN, u"Kilépés a létesítményből: zenelejátszó aktiválva."},
				{TextLanguage::KOREAN, u"시설 퇴장 중: 음악 플레이어가 활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"退出設施：音樂播放器已啟動。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"退出设施：音乐播放器已启动。"},
			}}},

			{"Music player interrupted.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Music player interrupted."},
				{TextLanguage::ENGLISH_UK, u"Music player interrupted."},
				{TextLanguage::FRENCH, u"Le lecteur de musique a été interrompu."},
				{TextLanguage::ITALIAN, u"Riproduzione musicale interrotta."},
				{TextLanguage::GERMAN, u"Musik-Player unterbrochen."},
				{TextLanguage::SPANISH_SPAIN, u"Reproductor de música interrumpido."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Leitor de música interrompido."},
				{TextLanguage::GREEK, u"Το πρόγραμμα αναπαραγωγής μουσικής διακόπηκε."},
				{TextLanguage::POLISH, u"Odtwarzacz muzyki przerwany."},
				{TextLanguage::RUSSIAN, u"Музыкальный плеер прерван."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Reproductor de música interrumpido."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Reprodutor de música interrompido."},
				{TextLanguage::JAPANESE, u"音楽プレーヤーが中断した。"},
				{TextLanguage::ARABIC, u"تم توقيف مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Muziekspeler onderbroken."},
				{TextLanguage::CZECH, u"Hudební přehrávač přerušen."},
				{TextLanguage::TURKISH, u"Müzik çalar kesintiye uğradı."},
				{TextLanguage::HUNGARIAN, u"Zenelejátszó megszakítva."},
				{TextLanguage::KOREAN, u"음악 플레이어가 중단되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"音樂播放器中斷。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"音乐播放器中断。"},
			}}}
		};
	}
}
