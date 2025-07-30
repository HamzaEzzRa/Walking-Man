#pragma once

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