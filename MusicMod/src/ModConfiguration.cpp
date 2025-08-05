#include "ModConfiguration.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "ordered_set.h"

#include "GameData.h"

#include "MemoryUtils.h"

namespace ModConfiguration
{
	const std::string modPublicName = "Walking Man";
	const std::string modInternalVersion = "1.0.4";
	const std::string modLogFilename = "walkingman.log";
	const std::string enableDevFilename = "walkingman.dev";
	bool devMode = false;

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

		{"allowScriptedSongs",
		[](const std::string& val) { allowScriptedSongs = (val == "true" || val == "1"); }},

		{"showMusicPlayerUI",
		[](const std::string& val) { showMusicPlayerUI = (val == "true" || val == "1"); }}
	};

	std::string Trim(const std::string& str)
	{
		const char* whitespace = " \t\r\n";
		size_t start = str.find_first_not_of(whitespace);
		size_t end = str.find_last_not_of(whitespace);
		return (start == std::string::npos) ? "" : str.substr(start, end - start + 1);
	}

	bool IsCommentOrEmpty(const std::string& line)
	{
		std::string trimmed = Trim(line);
		return trimmed.empty() || trimmed.rfind("//", 0) == 0;
	}

	bool LoadConfigFromFile()
	{
		Logger logger{ "Mod Configuration" };

		std::ifstream file(configFilePath);
		if (!file.is_open())
		{
			return false;
		}

		Section previousSection = Section::NONE;
		Section currentSection = Section::NONE;
		std::string line;

		while (std::getline(file, line))
		{
			if (IsCommentOrEmpty(line))
			{
				continue;
			}

			size_t commentPos = line.find("//");
			if (commentPos != std::string::npos)
			{
				line = Trim(line.substr(0, commentPos));
			}
			if (line.empty())
			{
				continue;
			}

			if (headerSectionMap.find(line) != headerSectionMap.end())
			{
				previousSection = currentSection;
				currentSection = headerSectionMap.at(line);
				continue;
			}

			switch (currentSection)
			{
				case Section::GLOBAL_SETTINGS:
				{
					size_t eqPos = line.find('=');
					if (eqPos == std::string::npos) break;

					std::string key = Trim(line.substr(0, eqPos));
					std::string val = Trim(line.substr(eqPos + 1));
					if (val != "true" && val != "false" && val != "1" && val != "0")
					{
						std::string errorMessage = "Invalid value for setting "
							+ key + " (" + val + ")"
							+ "\nPlease check your \"" + ModConfiguration::configFilePath + "\" file.";
						MemoryUtils::ShowErrorPopup(errorMessage, ModConfiguration::modPublicName);
						continue;
					}

					auto it = parameterSetters.find(key);
					if (it != parameterSetters.end())
					{
						it->second(val);
					}
					break;
				}
				case Section::ACTIVE_SONGS:
				{
					if (previousSection != Section::ACTIVE_SONGS)
					{
						activePlaylist.clear();
					}
					activePlaylist.insert(line);
					break;
				}
				case Section::INACTIVE_SONGS:
					break;
				default:
					break;
			}

			previousSection = currentSection;
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
				"44 0F B6 88 ?? ?? ?? ?? 45 84 C9 74 1D 8B 90"}
			},
			{
				"GamePreExit",
				{"GamePreExit",
				"40 53 48 83 EC ?? 80 3D ?? ?? ?? ?? ?? 8B DA 74 06 48 83 C4 ?? 5B C3"}
			},
			{
				"GamePreLoad",
				{"GamePreLoad",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 "
				"56 41 57 48 8D AC 24 10 FB FF FF 48 81 EC F0 05 00 00 48 8B 05"}
			},
			{
				"AccessMusicPool",
				{"AccessMusicPool",
				"48 89 5C 24 ?? 57 48 83 EC ?? 48 83 79 ?? ?? 75 0B 48 83 79 ?? ?? "
				"0F 84 ?? ?? ?? ?? 48 8B 01 FF 10 4C 8B 15 ?? ?? ?? ?? 48 8B F8"}
			},
			{
				"AccessLanguagePool",
				{"AccessLanguagePool",
				"48 8B 01 48 FF A0 ?? ?? ?? ?? CC CC CC "
				"CC CC CC 48 89 5C 24 ?? 57 4C 8B 51"}
			},

			// Input tracking functions
			{
				"ProcessControllerInput",
				{"ProcessControllerInput",
				"4C 8B DC ?? 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 "
				"?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 8B 41"}
			},

			// Game state functions
			{
				"InGameFlagUpdate",
				{"InGameFlagUpdate",
				"41 57 41 56 41 55 41 54 56 57 55 53 B8 ?? ?? "
				"?? ?? E8 ?? ?? ?? ?? 48 29 C4 44 0F 29 9C 24"}
			},

			// In-game UI functions
			{
				"InGameUIUpdateStaticPoolCaller",
				{"InGameUIUpdateStaticPoolCaller",
				"44 89 4C 24 20 41 54 48 83 EC 40 48 89 74 24 60 4C "
				"8B E2 4C 89 74 24 28 4D 8B F0 48 8B F1 48 85 D2"}
			},
			{
				"AccessStaticUIPool",
				{"AccessStaticUIPool",
				//"4C 8B DC 57 48 81 EC ?? ?? ?? ?? C4 C1 78 29 73 C8 "
				//"48 8D 05 ?? ?? ?? ?? C4 C1 78 29 7B B8 48 8B F9", // Hooking this caused a weird bug where facility elevators wouldn't work
				//true} // this function has AVX instructions in the prologue
				"48 89 ?? 24 20 55 48 8D 6C 24 A9 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? " // Child function that shares the same arg4
				"48 33 C4 48 89 45 ?? 48 8B 41 50 48 8B ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9"}
			},
			{
				"GetNotificationPool",
				{"GetNotificationPool",
				"48 83 EC 28 E8 ?? ?? ?? FF 48 8B 40 ?? 48 83 C4 28 C3 CC CC CC "
				"CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 56 57 41 54"}
			},
			{
				"DrawNotificationText",
				{"DrawNotificationText",
				"44 88 4C 24 20 44 89 44 24 18 48 89 54 24 10 53 55 "
				"56 41 54 41 55 41 56 41 57 48 83 EC ?? 44 8B B9"}
			},
			{
				"CreateRuntimeUITextFromString",
				{"CreateRuntimeUITextFromString",
				"40 53 48 83 EC 20 48 8B D9 48 C7 01 00 00 00 00 49 C7 C0 FF FF FF FF 66 "
				"0F 1F 84 00 00 00 00 00 49 FF C0 42 80 3C 02 00 75 F6 E8 51 08 00 00"}
			},
			{
				"TryFreeRuntimeUIText",
				{"TryFreeRuntimeUIText",
				"48 8B 09 B8 FF FF FF FF F0 0F C1 01 83 F8 01 0F 84 ?? ?? ?? ?? C3"}
			},
			{
				"AssignRuntimeUIText",
				{"AssignRuntimeUIText",
				"48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 09 48 3B 0A"}
			},
			{
				"UpdateRuntimeUIText",
				{"UpdateRuntimeUIText",
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 48 8B D9 48 "
				"8B FA 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 1F"}
			},
			{
				"InGameUIDrawElement",
				{"InGameUIDrawElement",
				"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? C5 F8 "
				"29 74 24 ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 ?? 48 63 EA"}
			},
			{
				"InGameUIUpdateElement",
				{"InGameUIUpdateElement",
				"48 89 5C 24 08 57 48 83 EC 20 48 63 FA 48 "
				"8B D9 E8 ?? ?? ?? ?? 45 33 C0 4C 8B CF"}
			},
			{
				"PostMenuExit",
				{"PostMenuExit",
				"40 57 48 83 EC 40 F6 41 64 02 48 8B F9 48 89 74 24 58 74 24 C5 "
				"F8 10 41 40 44 8B 49 60 48 8D 54 24 30 48 8B 49 08 41 B0 01"}
			},

			// Music player functions
			{
				"PlayMusic",
				{"PlayMusic",
				"48 85 D2 74 ?? 48 83 EC 58 48 8B 05 ?? ?? ?? "
				"?? 48 33 C4 48 89 44 24 40 4D 85 C0 74 ??"}
			},
			{
				"PlayingLoop",
				{"PlayingLoop",
				"4C 39 48 08 75 06 4C 39 40 10 74 0B"}
			},
			{
				"PlayUISound",
				{"PlayUISound",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 "
				"EC 20 48 8B 01 41 0F B6 F1 41 8B D8 4C 8B F2 48 8B F9"}
			},
			{
				"ShowMusicDescription",
				{"ShowMusicDescription",
				"40 53 56 57 48 81 EC 80 00 00 00 C5 F8 29 74 24 70 48 "
				"8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 8B D9 8B FA",
				true} // has AVX instructions in the prologue, avoid hooking it
			},
			{
				"ShowMusicDescriptionCore",
				{"ShowMusicDescriptionCore",
				"4C 8B DC 55 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? "
				"?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 73 F0 33 F6 4D 89 73 D0"}
			}
		};

		// sadly music doesn't keep the same offsets inside this table across versions (most likely because of new music in DC)
		// ended up going for a stable aob per audio
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

		// Missing songs:
		// - "I'll Keep Coming" - Low Roar - 12
		// - "Yellow Box" - The Neighbourhood - 40
		// - "Ghost" - Alan Walker & Au/Ra - 41
		// - "Trigger" - Khalid & Major Lazer - 42
		// - "Meanwhile... In Genova" - The S.L.P. - 43
		// - "Ludens" - Bring Me The Horizon - 44
		// - "Pop Virus" - Gen Hoshino - 50
		// - "Alone" - Biting Elbows - 101
		// - "Path" - Apocalyptica - 106
		// - "Path Vol. 2" - Apocalyptica & Sandra Nasic - 107
		// - "Death Stranding" - CHVRCHES ?? (not sure if it exists in-game)
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
			{
				"Ambient 1",
				{0, MusicType::AMBIENT, 180000, "Ambient 1", "",
				"?? ?? ?? ?? ?? 7F 00 00 3B AE 7B 3A EE 91 42 1D 8B 6B BB 93 4D C7 D7 1A"}
			},
			{
				"Ambient 2",
				{0, MusicType::AMBIENT, 180000, "Ambient 2", "",
				"?? ?? ?? ?? ?? 7F 00 00 73 8E 1B C3 D5 54 4C 3C 9C 37 19 87 70 B1 AF 7C"}
			},
			{
				"Ambient 3",
				{0, MusicType::AMBIENT, 180000, "Ambient 3", "",
				"?? ?? ?? ?? ?? 7F 00 00 8D 28 ED 0A C2 44 44 E4 9B 00 BD 80 4E A9 ED 57"}
			},
			{
				"Ambient 4",
				{0, MusicType::AMBIENT, 180000, "Ambient 4", "",
				"?? ?? ?? ?? ?? 7F 00 00 95 B0 41 AE 75 8E 49 A4 8B E1 54 15 E9 91 69 93"}
			},
			{
				"Ambient 5",
				{0, MusicType::AMBIENT, 180000, "Ambient 5", "",
				"?? ?? ?? ?? ?? 7F 00 00 A6 0B 21 84 06 51 4E 8C A1 F6 8B 01 2E 73 D1 C9"}
			},
			{
				"Ambient 6", // no more music a bit earlier than 3 minutes, but keeps going
				{0, MusicType::AMBIENT, 170000, "Ambient 6", "",
				"?? ?? ?? ?? ?? 7F 00 00 C2 9E 42 90 99 DE 44 A0 8F 13 02 EC 23 F3 46 A2"}
			},
			{
				"Ambient 7",
				{0, MusicType::AMBIENT, 180000, "Ambient 7", "",
				"?? ?? ?? ?? ?? 7F 00 00 CC D1 13 95 96 1E 40 9C BB 72 69 C7 C0 A1 B5 60"}
			},

			// Others
			{
				"Almost Nothing (Instrumental)",
				{32, MusicType::SONG, 0, "Almost Nothing (Instrumental)", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 CE 47 2C 1F 52 25 45 56 B2 31 8B 0E 20 BF 6B 08"}
			},
			{
				"Almost Nothing (No Beatbox)",
				{32, MusicType::SONG, 0, "Almost Nothing (No Beatbox)", "SILENT POETS",
				"?? ?? ?? ?? ?? 7F 00 00 EF E6 84 B0 5C 9F 4B BE 91 B7 AE F9 06 BD 12 FF"}
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
			},
		};

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
			{"Zoom In/Out", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Zoom In/Out"},
				{TextLanguage::ENGLISH_UK, u"Zoom In/Out"},
				{TextLanguage::FRENCH, u"Zoom avant/arrière"},
				{TextLanguage::ITALIAN, u"Zoom avanti/indietro"},
				{TextLanguage::GERMAN, u"Heran-/Herauszoomen"},
				{TextLanguage::SPANISH_SPAIN, u"Acercar/Alejar"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Usar zoom"},
				{TextLanguage::GREEK, u"Εστίασε/Απομάκρυνε"},
				{TextLanguage::POLISH, u"Przybliż/oddal"},
				{TextLanguage::RUSSIAN, u"Изменить масштаб"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Acercar/alejar vista"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Ampliar/Reduzir"},
				{TextLanguage::JAPANESE, u"拡大"},
				{TextLanguage::ARABIC, u"تكبير/تصغير"},
				{TextLanguage::DUTCH, u"In-/uitzoomen"},
				{TextLanguage::CZECH, u"Přiblížit/oddálit"},
				{TextLanguage::TURKISH, u"Yakınlaştır/Uzaklaştır"},
				{TextLanguage::HUNGARIAN, u"Nagyítás/Kicsinyítés"},
				{TextLanguage::KOREAN, u"확대하기"},
				{TextLanguage::CHINESE_TRADITIONAL, u"放大／縮小"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"放大/缩小"},
			}}},
			{"Stand Up", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Stand Up"},
				{TextLanguage::ENGLISH_UK, u"Stand Up"},
				{TextLanguage::FRENCH, u"Se lever"},
				{TextLanguage::ITALIAN, u"Alzati"},
				{TextLanguage::GERMAN, u"Aufstehen"},
				{TextLanguage::SPANISH_SPAIN, u"Levantarte"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Levantar"},
				{TextLanguage::GREEK, u"Σήκω"},
				{TextLanguage::POLISH, u"Wstań"},
				{TextLanguage::RUSSIAN, u"Встать"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Levantarse"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Levantar"},
				{TextLanguage::JAPANESE, u"立ち上がる"},
				{TextLanguage::ARABIC, u"وقوف"},
				{TextLanguage::DUTCH, u"Opstaan"},
				{TextLanguage::CZECH, u"Stoupnout si"},
				{TextLanguage::TURKISH, u"Kalk"},
				{TextLanguage::HUNGARIAN, u"Felállás"},
				{TextLanguage::KOREAN, u"일어서기"},
				{TextLanguage::CHINESE_TRADITIONAL, u"站立"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"站起来"},
			}}},
			{"Descend/Ascend", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Descend/Ascend"},
				{TextLanguage::ENGLISH_UK, u"Descend/Ascend"},
				{TextLanguage::FRENCH, u"Plus bas/Plus haut"},
				{TextLanguage::ITALIAN, u"Scendi/sali"},
				{TextLanguage::GERMAN, u"Nach oben/unten"},
				{TextLanguage::SPANISH_SPAIN, u"Bajar/Subir"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Descer/Subir câmara"},
				{TextLanguage::GREEK, u"Πάνω/Κάτω"},
				{TextLanguage::POLISH, u"Niżej/wyżej"},
				{TextLanguage::RUSSIAN, u"Спуск/подъем"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Descender/Ascender"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Descer/Subir"},
				{TextLanguage::JAPANESE, u"下降/上昇"},
				{TextLanguage::ARABIC, u"أسفل/أعلى"},
				{TextLanguage::DUTCH, u"Omlaag/omhoog"},
				{TextLanguage::CZECH, u"Sestoupit/vystoupit"},
				{TextLanguage::TURKISH, u"Alçal/Yüksel"},
				{TextLanguage::HUNGARIAN, u"Emelkedés/Ereszkedés"},
				{TextLanguage::KOREAN, u"하강/상승"},
				{TextLanguage::CHINESE_TRADITIONAL, u"上升／下降"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"上升/下降"},
			}}},

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
				{TextLanguage::RUSSIAN, u"Воспроизвести музыку"},
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
				{TextLanguage::GREEK, u"Σταμάτησε μουσική"},
				{TextLanguage::POLISH, u"Zatrzymaj muzykę"},
				{TextLanguage::RUSSIAN, u"Остановить музыку"},
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
				{TextLanguage::FRENCH, u"Mode de boucle (Tous)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Tutti)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Alle)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Todos)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Todos)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (όλα)"},
				{TextLanguage::POLISH, u"Tryb pętli (Wszystkie)"},
				{TextLanguage::RUSSIAN, u"Режим петли (Все)"},
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
				{TextLanguage::FRENCH, u"Mode de boucle (Seul)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Uno)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Eins)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Uno)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Um)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (μία)"},
				{TextLanguage::POLISH, u"Tryb pętli (Jeden)"},
				{TextLanguage::RUSSIAN, u"Режим петли (Один)"},
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
				{TextLanguage::FRENCH, u"Mode de boucle (Aucun)"},
				{TextLanguage::ITALIAN, u"Modalità Loop (Nessuna)"},
				{TextLanguage::GERMAN, u"Schleifenmodus (Kein)"},
				{TextLanguage::SPANISH_SPAIN, u"Modo bucle (Ninguno)"},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Modo Loop (Nenhum)"},
				{TextLanguage::GREEK, u"Λειτουργία βρόχου (Καμία)"},
				{TextLanguage::POLISH, u"Tryb pętli (Brak)"},
				{TextLanguage::RUSSIAN, u"Режим петли (Нет)"},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Modo bucle (Ninguno)"},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Modo Loop (Nenhum)"},
				{TextLanguage::JAPANESE, u"ループモード (なし)"},
				{TextLanguage::ARABIC, u"وضع التكرار (دون تكرار)"},
				{TextLanguage::DUTCH, u"Lusmodus (Geen)"},
				{TextLanguage::CZECH, u"Režim smyčky (Žádná)"},
				{TextLanguage::TURKISH, u"Döngü Modu (Hiçbiri)"},
				{TextLanguage::HUNGARIAN, u"Loop mód (Egyetlen)"},
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
				{TextLanguage::FRENCH, u"Menace à proximité: le lecteur de musique est désactivé."},
				{TextLanguage::ITALIAN, u"Minaccia vicina: lettore musicale disattivato."},
				{TextLanguage::GERMAN, u"Bedrohung in der Nähe: Musik-Player deaktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Amenaza cercana: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Ameaça próxima: leitor de música desativado."},
				{TextLanguage::GREEK, u"Κλειστή απειλή: η συσκευή αναπαραγωγής μουσικής είναι απενεργοποιημένη."},
				{TextLanguage::POLISH, u"Niebezpieczeństwo w pobliżu: wyłączony odtwarzacz muzyki."},
				{TextLanguage::RUSSIAN, u"Угроза рядом: музыкальный проигрыватель выключен."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Amenaza cercana: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Ameaça próxima: reprodutor de música desativado."},
				{TextLanguage::JAPANESE, u"脅威接近：ミュージックプレーヤーが無効化されました。"},
				{TextLanguage::ARABIC, u"خطر قريب: تم تعطيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Bedreiging dichtbij: muziekspeler gedeactiveerd."},
				{TextLanguage::CZECH, u"Ohrožení v blízkosti: hudební přehrávač deaktivován."},
				{TextLanguage::TURKISH, u"Tehdit yakın: müzik çalar devre dışı."},
				{TextLanguage::HUNGARIAN, u"Fenyegetés a közelben: a zenelejátszó kikapcsolt."},
				{TextLanguage::KOREAN, u"주변 위협: 음악 플레이어가 비활성화되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"威胁临近：音乐播放器已停用。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"威胁临近：音乐播放器已停用。"},
			}}},
			{"Threat cleared: music player can be activated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Threat cleared: music player can be activated."},
				{TextLanguage::ENGLISH_UK, u"Threat cleared: music player can be activated."},
				{TextLanguage::FRENCH, u"Menace écartée: le lecteur de musique peut être activé."},
				{TextLanguage::ITALIAN, u"Minaccia sventata: lettore musicale riattivabile."},
				{TextLanguage::GERMAN, u"Bedrohung in der Nähe: Musik-Player aktivierbar."},
				{TextLanguage::SPANISH_SPAIN, u"Amenaza despejada: se puede activar el reproductor de música."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Ameaça evitada: leitor de música pode ser ativado."},
				{TextLanguage::GREEK, u"Εκκαθάριση απειλής: η συσκευή αναπαραγωγής μουσικής μπορεί να ενεργοποιηθεί."},
				{TextLanguage::POLISH, u"Usunięto zagrożenie: można aktywować odtwarzacz muzyki."},
				{TextLanguage::RUSSIAN, u"Угроза снята: можно включить музыкальный проигрыватель."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Amenaza despejada: reproductor de música puede activarse."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Ameaça eliminada: reprodutor de música pode ser ativado."},
				{TextLanguage::JAPANESE, u"脅威クリア：ミュージックプレーヤーがアクティベート可能。"},
				{TextLanguage::ARABIC, u"نهاية الخطر: يمكن تفعيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Bedreiging geweken: muziekspeler kan worden geactiveerd."},
				{TextLanguage::CZECH, u"Hrozba odstraněna: hudební přehrávač lze aktivovat."},
				{TextLanguage::TURKISH, u"Tehdit temizlendi: müzik çalar etkinleştirilebilir."},
				{TextLanguage::HUNGARIAN, u"Fenyegetés elhárítva: a zenelejátszó aktiválható."},
				{TextLanguage::KOREAN, u"위협이 제거되었습니다: 음악 플레이어를 활성화할 수 있습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"威脅消失：音樂播放器可以啟動。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"威胁消失：音乐播放器可以启动。"},
			}}},
			{"Chiral network off: music player deactivated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Chiral network off: music player deactivated."},
				{TextLanguage::ENGLISH_UK, u"Chiral network off: music player deactivated."},
				{TextLanguage::FRENCH, u"Déconnecté du réseau chiral: le lecteur de musique est désactivé."},
				{TextLanguage::ITALIAN, u"Rete chirale spenta: lettore musicale disattivato."},
				{TextLanguage::GERMAN, u"Chiral‑Netz aus: Musik‑Player deaktiviert."},
				{TextLanguage::SPANISH_SPAIN, u"Red quiral desactivada: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Rede quiral desligada: leitor de música desativado."},
				{TextLanguage::GREEK, u"Δίκτυο απενεργοποιημένο: παίκτης μουσικής απενεργοποιήθηκε."},
				{TextLanguage::POLISH, u"Sieć wyłączona: odtwarzacz muzyki wyłączony."},
				{TextLanguage::RUSSIAN, u"Хиральная сеть отключена: музыкальный плеер деактивирован."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Red quiral desactivada: reproductor de música desactivado."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Rede quiral desativada: reprodutor de música desativado."},
				{TextLanguage::JAPANESE, u"キラルネットワークオフ：ミュージックプレーヤーが無効化されました。"},
				{TextLanguage::ARABIC, u"الشبكة الكيرالية متوقفة: تم تعطيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Chiraal netwerk uit: muziekspeler gedeactiveerd."},
				{TextLanguage::CZECH, u"Chirální síť deaktivována: hudební přehrávač deaktivován."},
				{TextLanguage::TURKISH, u"Ağ kapalı: müzik çalar devre dışı."},
				{TextLanguage::HUNGARIAN, u"Hálózat letiltva: a zenelejátszó le van tiltva."},
				{TextLanguage::KOREAN, u"네트워크 꺼짐: 음악 플레이어가 비활성화됩니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"網路關閉：音乐播放器已停用。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"网路关闭：音乐播放器已停用。"},
			}}},
			{"Chiral network on: music player can be activated.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Chiral network on: music player can be activated."},
				{TextLanguage::ENGLISH_UK, u"Chiral network on: music player can be activated."},
				{TextLanguage::FRENCH, u"Connecté au réseau chiral: le lecteur de musique peut être activé."},
				{TextLanguage::ITALIAN, u"Rete chirale attiva: lettore musicale riattivabile."},
				{TextLanguage::GERMAN, u"Chiral‑Netz an: Musik‑Player aktivierbar."},
				{TextLanguage::SPANISH_SPAIN, u"Red quiral activa: se puede activar el reproductor de música."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Rede quiral ligada: leitor de música pode ser ativado."},
				{TextLanguage::GREEK, u"Δίκτυο ενεργοποιημένο: η συσκευή αναπαραγωγής μουσικής μπορεί να ενεργοποιηθεί."},
				{TextLanguage::POLISH, u"Sieć aktywna: można włączyć odtwarzacz muzyki."},
				{TextLanguage::RUSSIAN, u"Хиральная сеть на: можно активировать музыкальный плеер."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Red quiral encendida: reproductor de música puede activarse."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Rede quiral ligada: reprodutor de música pode ser ativado."},
				{TextLanguage::JAPANESE, u"キラルネットワークオン：ミュージックプレーヤーがアクティベーション可能。"},
				{TextLanguage::ARABIC, u"الشبكة الكيرالية مفعّلة: يمكن تفعيل مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Chiraal netwerk aan: muziekspeler kan geactiveerd worden."},
				{TextLanguage::CZECH, u"Chirální síť povolena: hudební přehrávač lze aktivovat."},
				{TextLanguage::TURKISH, u"Ağ açık: müzik çalar etkinleştirilebilir."},
				{TextLanguage::HUNGARIAN, u"Hálózat engedélyezve: a zenelejátszó aktiválható."},
				{TextLanguage::KOREAN, u"네트워크 켜짐: 음악 플레이어를 활성화할 수 있습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"已啟用網路：音樂播放器可以啟動。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"已启用网路：音乐播放器可以启动。"},
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
				{TextLanguage::RUSSIAN, u"Музыкальный проигрыватель прервался."},
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
			}}},
			{"Entering facility: music player interrupted.", LocalizedText{{
				{TextLanguage::ENGLISH_US, u"Entering facility: music player interrupted."},
				{TextLanguage::ENGLISH_UK, u"Entering facility: music player interrupted."},
				{TextLanguage::FRENCH, u"Accès à l'installation: le lecteur de musique a été interrompu."},
				{TextLanguage::ITALIAN, u"Entrata in struttura: riproduzione musicale interrotta."},
				{TextLanguage::GERMAN, u"Einrichtung betreten: Musik-Player unterbrochen."},
				{TextLanguage::SPANISH_SPAIN, u"Entrando la instalación: reproductor de música interrumpido."},
				{TextLanguage::PORTUGUESE_PORTUGAL, u"Entrar na instalação: leitor de música interrompido."},
				{TextLanguage::GREEK, u"Είσοδος στους χώρους: διακοπή αναπαραγωγής μουσικής."},
				{TextLanguage::POLISH, u"Wejście do obiektu: odtwarzacz muzyki przerwany."},
				{TextLanguage::RUSSIAN, u"Вход на объект: музыкальный проигрыватель прервался."},
				{TextLanguage::SPANISH_LATIN_AMERICA, u"Entrando la instalación: reproductor de música interrumpido."},
				{TextLanguage::PORTUGUESE_LATIN_AMERICA, u"Entrando nas instalações: reprodutor de música interrompido."},
				{TextLanguage::JAPANESE, u"施設に入る：音楽プレーヤーが中断。"},
				{TextLanguage::ARABIC, u"دخول المنشأة: تم توقيف مشغل الموسيقى."},
				{TextLanguage::DUTCH, u"Binnengaan faciliteit: muziekspeler onderbroken."},
				{TextLanguage::CZECH, u"Přístup k instalaci: hudební přehrávač přerušen."},
				{TextLanguage::TURKISH, u"Tesise giriliyor: müzik çalar kesintiye uğradı."},
				{TextLanguage::HUNGARIAN, u"Hozzáférés a létesítményhez: zenelejátszó megszakítva."},
				{TextLanguage::KOREAN, u"시설 진입 중: 음악 플레이어가 중단되었습니다."},
				{TextLanguage::CHINESE_TRADITIONAL, u"進入設施： 音樂播放器中斷。"},
				{TextLanguage::CHINESE_SIMPLIFIED, u"进入设施： 音乐播放器中断。"},
			}}}
		};
	}
}