#pragma once

enum MusicType
{
	UNKNOWN,
	SFX,
	AMBIENT,
	SONG
};

struct MusicData
{
	uint16_t descriptionID; // passed to the draw description function
	MusicType type;
	long long length; // estimated length in milliseconds, 0 if unknown. Not useful, playing loop tracks music ending
	const char* name; // name and artist are just for logging
	const char* artist;
	const char* signature;
	uintptr_t address = 0;
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