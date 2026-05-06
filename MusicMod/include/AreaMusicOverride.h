#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct MusicData;

class AreaMusicOverride
{
public:
	inline static constexpr const char* TargetTrackName = "Don't Be So Serious";

	static void Initialize();
	static bool UsesOverride(const MusicData*);
	static bool IsTemplateTrack(const MusicData*);
	static bool Register(const MusicData*);
	static void Unset();
	static void RetireBuffer();
	static bool DurationControlledByWwise();
	static long long GetRegisteredEffectiveDurationMs(const MusicData*);

private:
	struct AreaMusicMetadataPatch;

	static uint32_t GetAreaMusicOverrideMediaId(const MusicData*);
	static const char* GetAreaMusicOverridePath(const MusicData*);
	static long long GetRegisteredSourceDurationMs(const MusicData*);
	static long long CalculateEffectiveDurationMs(long long, const MusicData*);
	static bool ResolveWwiseMediaFunctions();
	static bool LoadAreaMusicOverride(const char*);
	static void ClearAreaMusicMetadataPatch();
	static void PrepareAreaMusicMetadataPatch(const MusicData*, uint32_t, uint32_t, long long);
	static bool FindAreaMusicBankMetadata(const uint8_t*, size_t);
	static bool PatchAreaMusicHircMetadata(uint8_t*, size_t, const AreaMusicMetadataPatch*);
	static bool PatchAreaMusicBankMetadata(uint8_t*, size_t);
	static bool PatchLiveAreaMusicMetadata(const MusicData*, long long);
	static void RestoreLiveAreaMusicMetadata();
	static bool PatchKnownAreaMusicBankMetadata();
	static bool PatchProcessAreaMusicBankMetadata();
	static bool ReloadAreaMusicBankMetadata(bool);
	static void RememberAreaMusicBankMemory(const void*, size_t, uint32_t, const char*);
	static void BackupAreaMusicBankMetadataBytes(uint8_t*, size_t, const std::vector<uint8_t>&);
	static void BackupAreaMusicBankMetadata(uint8_t*, size_t);
	static void RestoreAreaMusicBankMetadata();
	static uint32_t __cdecl LoadBankMemoryCopyHook(const void*, unsigned long, unsigned long*);
	static uint32_t __cdecl LoadBankMemoryViewHook(const void*, unsigned long, unsigned long*);

	struct AkSourceSettings
	{
		uint32_t sourceId = 0;
		uint32_t reserved0 = 0;
		void* mediaMemory = nullptr;
		uint32_t mediaSize = 0;
		uint32_t reserved1 = 0;
	};

	using AkSetMediaFn = uint32_t(__cdecl*)(AkSourceSettings*, unsigned long);
	using AkUnsetMediaFn = uint32_t(__cdecl*)(AkSourceSettings*, unsigned long);
	using AkLoadBankMemoryFn = uint32_t(__cdecl*)(const void*, unsigned long, unsigned long*);
	using AkUnloadBankMemoryFn = uint32_t(__cdecl*)(uint32_t, const void*);

	inline static constexpr uint32_t akSuccess = 1;
	inline static constexpr uint32_t areaMusicOverrideMediaId = 14330364;
	inline static constexpr uint32_t areaMusicOverrideTrackId = 864381405;
	inline static constexpr uint32_t areaMusicOverrideSegmentId = 515599299;
	inline static constexpr uint32_t areaMusicOverrideRanSeqId = 124768037;
	inline static constexpr uint32_t areaMusicOverrideSourcePluginId = 0x00040001;
	inline static constexpr uint32_t areaMusicOverridePcmSourcePluginId = 0x00010001;
	inline static constexpr const char* setMediaExportName =
		"?SetMedia@SoundEngine@AK@@YA?AW4AKRESULT@@PEAUAkSourceSettings@@K@Z";
	inline static constexpr const char* unsetMediaExportName =
		"?UnsetMedia@SoundEngine@AK@@YA?AW4AKRESULT@@PEAUAkSourceSettings@@K@Z";
	inline static constexpr const char* loadBankMemoryCopyExportName =
		"?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXKAEAK@Z";
	inline static constexpr const char* loadBankMemoryViewExportName =
		"?LoadBankMemoryView@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXKAEAK@Z";
	inline static constexpr const char* unloadBankMemoryExportName =
		"?UnloadBank@SoundEngine@AK@@YA?AW4AKRESULT@@KPEBX@Z";

	struct AreaMusicMetadataPatch
	{
		bool enabled = false;
		long long durationMs = 0;
		uint32_t mediaId = areaMusicOverrideMediaId;
		uint32_t mediaSize = 0;
		uint32_t sourcePluginId = areaMusicOverrideSourcePluginId;
	};

	struct AreaMusicOverrideBuffer
	{
		std::string path{};
		std::vector<uint8_t> bytes{};
		uint32_t sourcePluginId = areaMusicOverrideSourcePluginId;
		long long durationMs = 0;
	};

	struct AreaMusicBankMemoryView
	{
		uint8_t* data = nullptr;
		uint8_t* originalData = nullptr;
		size_t size = 0;
		uint32_t bankId = 0;
		bool loadedPatched = false;
		std::vector<uint8_t> originalBytes{};
		std::shared_ptr<std::vector<uint8_t>> ownedBytes{};
	};

	struct AreaMusicBankMetadataBackup
	{
		uint8_t* data = nullptr;
		size_t size = 0;
		std::vector<uint8_t> bytes{};
	};

	struct LiveAreaMusicMemoryBackup
	{
		uint8_t* data = nullptr;
		std::vector<uint8_t> bytes{};
	};

	struct LiveAreaMusicMetadataBackup
	{
		bool valid = false;
		uint32_t segmentDurationTicks = 0;
		std::vector<uint32_t> markerPositionTicks{};
		std::vector<uint8_t> trackSourceEntryBytes{};
		uint32_t trackSourceEntryCount = 0;
		std::vector<LiveAreaMusicMemoryBackup> sourceObjectBackups{};
		uint32_t trackSourceDurationTicks = 0;
		std::vector<uint8_t> trackTimingBytes{};
		uint32_t trackTimingCount = 0;
	};

	inline static bool wwiseMediaFunctionsResolved = false;
	inline static AkSetMediaFn setMediaFunc = nullptr;
	inline static AkUnsetMediaFn unsetMediaFunc = nullptr;
	inline static bool areaMusicMetadataHooksInitialized = false;
	inline static AkLoadBankMemoryFn loadBankMemoryCopyFunc = nullptr;
	inline static AkLoadBankMemoryFn loadBankMemoryViewFunc = nullptr;
	inline static AkUnloadBankMemoryFn unloadBankMemoryFunc = nullptr;
	inline static AkLoadBankMemoryFn originalLoadBankMemoryCopyFunc = nullptr;
	inline static AkLoadBankMemoryFn originalLoadBankMemoryViewFunc = nullptr;
	inline static std::mutex areaMusicMetadataMutex{};
	inline static AreaMusicMetadataPatch areaMusicMetadataPatch{};
	inline static std::vector<std::unique_ptr<std::vector<uint8_t>>> patchedBankViews{};
	inline static std::vector<AreaMusicBankMemoryView> areaMusicBankMemoryViews{};
	inline static std::vector<AreaMusicBankMetadataBackup> areaMusicBankMetadataBackups{};
	inline static std::unique_ptr<AreaMusicOverrideBuffer> areaMusicOverrideBuffer{};
	inline static std::vector<std::unique_ptr<AreaMusicOverrideBuffer>> retiredAreaMusicOverrideBuffers{};
	inline static LiveAreaMusicMetadataBackup liveAreaMusicMetadataBackup{};
	inline static uint32_t areaMusicOverrideRegisteredMediaId = 0;
	inline static bool areaMusicOverrideRegistered = false;
	inline static bool areaMusicOverrideMetadataPatched = false;
};