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
	const std::string modInternalVersion = "1.0.3";
	const std::string modLogFilename = "walkingman.log";
	const std::string enableDevFilename = "walkingman.dev";
	bool devMode = false;

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
				"AccessMusicPool",
				{"AccessMusicPool",
				"48 89 5C 24 ?? 57 48 83 EC ?? 48 83 79 ?? ?? 75 0B 48 83 79 ?? ?? "
				"0F 84 ?? ?? ?? ?? 48 8B 01 FF 10 4C 8B 15 ?? ?? ?? ?? 48 8B F8"}
			},
			{
				"GamePreLoad",
				{"GamePreLoad",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 "
				"56 41 57 48 8D AC 24 10 FB FF FF 48 81 EC F0 05 00 00 48 8B 05"}
			},
			{
				"GamePreExit",
				{"GamePreExit",
				"40 53 48 83 EC ?? 80 3D ?? ?? ?? ?? ?? 8B DA 74 06 48 83 C4 ?? 5B C3"}
			},

			// Input tracking functions
			{
				"GetInputBitmask",
				{"GetInputBitmask",
				"48 8B 91 ?? ?? ?? ?? 33 C0 48 85 D2 74 31 4C 63 82 ?? ?? ?? ?? 4D 85 C0 7E 25"}
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
			/*{ // No longer needed for the time being, might be useful to control UI sounds in the future
				"PlayUISound",
				{"PlayUISound",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 "
				"EC 20 48 8B 01 41 0F B6 F1 41 8B D8 4C 8B F2 48 8B F9"}
			},*/
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

		// Music interruptors are sounds played through PlayMusic, that should stop music in-game
		// Tracking them helps detect when autoplay should stop
		std::unordered_map<std::string, MusicData> interruptorDatabase = {
			{
				"Silence",
				{0, MusicType::SFX, 0, "Silence", "",
				"?? ?? ?? ?? ?? 7F 00 00 E4 77 3B F0 67 6B 41 DB BB CC E6 75 2F 25 5B F2"}
			},
			{
				"Enemy Detection",
				{0, MusicType::SFX, 0, "Enemy Detection", "",
				"?? ?? ?? ?? ?? 7F 00 00 7C 36 82 98 27 D7 46 B9 9A 4E 66 C4 C0 6D B0 D6"}
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
				"?? ?? ?? ?? ?? 7F 00 00 56 1A C5 A0 0F 5E 45 DE 86 02 71 4E 23 70 4C B8"}
			},
			{
				"Goliath",
				{105, MusicType::SONG, 0, "Goliath", "Woodkid",
				"?? ?? ?? ?? ?? 7F 00 00 A0 E8 B6 72 C9 60 41 76 87 A7 B6 98 0B 1A E9 40"}
			},
			{
				"Control",
				{102, MusicType::SONG, 0, "Control", "Biting Elbows",
				"?? ?? ?? ?? ?? 7F 00 00 7B A6 DF 4F 6C D4 4C 7D 86 50 F5 39 32 00 54 DC"}
			},
			{
				"Other Me",
				{103, MusicType::SONG, 0, "Other Me", "Biting Elbows",
				"?? ?? ?? ?? ?? 7F 00 00 3D 21 BE 31 6B BC 42 4A 96 03 04 9B F8 0B 58 F9"}
			},
			{
				"Fragile",
				{100, MusicType::SONG, 0, "Fragile", "Midge Ure",
				"?? ?? ?? ?? ?? 7F 00 00 FE D0 78 11 11 50 43 8F BB 02 5C 1F E4 6F 81 48"}
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
	}
}