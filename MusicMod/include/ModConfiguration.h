#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "ordered_set.h"

#include "GameData.h"

namespace ModConfiguration
{
    enum class Section
    {
        NONE,
        GLOBAL_SETTINGS,
        ACTIVE_SONGS,
        INACTIVE_SONGS
    };

    extern const std::string modPublicName;
	extern const std::string modInternalVersion;
    extern const std::string modLogFilename;
    extern const std::string enableTerminalFilename;

    extern const std::string configFilePath;
    extern const std::unordered_map<std::string, Section> headerSectionMap;

    extern bool showSongDescription;
    extern bool showNotificationMessage;
    extern bool connectToChiralNetwork;

    extern tsl::ordered_set<std::string> activePlaylist;

    extern const std::unordered_map<std::string, std::function<void(const std::string&)>> parameterSetters;

    std::string Trim(const std::string&);
    bool IsCommentOrEmpty(const std::string&);
    bool LoadConfigFromFile();

    namespace Databases
    {
		extern std::unordered_map<std::string, FunctionData> functionDatabase;

        extern std::unordered_map<std::string, MusicData> sfxDatabase;
        extern std::unordered_map<std::string, MusicData> unknownDatabase;
        extern std::unordered_map<std::string, MusicData> ambientDatabase;
        extern std::unordered_map<std::string, MusicData> songDatabase;
        extern std::unordered_map<std::string, MusicData> musicInterruptors;
    }
}