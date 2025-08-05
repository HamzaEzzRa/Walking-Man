#pragma once

#include <string>

enum class MusicType
{
	UNKNOWN,
	SFX,
	AMBIENT,
	SONG
};

enum class GameVersion
{
	DC,
	STANDARD
};

enum class TextLanguage: uint8_t
{
	UNKNOWN = 0,
	ENGLISH_US = 1,
	FRENCH = 2,
	ITALIAN = 5,
	GERMAN = 4,
	SPANISH_SPAIN = 3,
	PORTUGUESE_PORTUGAL = 7,
	GREEK = 23,
	POLISH = 11,
	RUSSIAN = 10,
	SPANISH_LATIN_AMERICA = 17,
	PORTUGUESE_LATIN_AMERICA = 18,
	JAPANESE = 16,
	ENGLISH_UK = 22,
	ARABIC = 20,
	DUTCH = 6,
	CZECH = 24,
	TURKISH = 19,
	HUNGARIAN = 25,
	KOREAN = 9,
	CHINESE_TRADITIONAL = 8,
	CHINESE_SIMPLIFIED = 21
};

struct MusicData
{
	uint16_t descriptionID; // passed to the draw description function
	MusicType type;
	long long maxLength; // max duration in ms, keeps going until the music stops if 0, else stops after this time
	const char* name; // name and artist are just for logging
	const char* artist;
	const char* signature;
	bool exclusiveDC = false; // If true, this music is exclusive to the Director's Cut version of the game
	uintptr_t address = 0;
	bool active = false;
};

struct FunctionData
{
	const char* name;
	const char* signature;
	bool usesAVX = false; // If function uses AVX instructions, use disassembly-based MemoryUtils::PlaceHook, not MinHook
	uintptr_t address = 0;
	void* originalFunction = nullptr;
};

struct LocalizedText
{
	std::unordered_map<TextLanguage, std::u16string> textMap;

	const std::u16string& GetText(TextLanguage language) const
	{
		auto it = textMap.find(language);
		if (it != textMap.end())
		{
			return it->second;
		}
		return 0;
	}
};

template<typename T>
struct ScanTarget;

template<>
struct ScanTarget<MusicData>
{
	static const char* GetSignature(const MusicData& data) { return data.signature; }
	static void SetAddress(MusicData& data, uintptr_t address) { data.address = address; }
};

template<>
struct ScanTarget<FunctionData>
{
	static const char* GetSignature(const FunctionData& func) { return func.signature; }
	static void SetAddress(FunctionData& func, uintptr_t address) { func.address = address; }
};