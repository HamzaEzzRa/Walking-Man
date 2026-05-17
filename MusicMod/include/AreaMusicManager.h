#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AreaMusicData.h"
#include "IEventListener.h"

#include "GameData.h"

class AreaMusicManager : public IEventListener
{
public:
	AreaMusicManager() = default;

	void OnEvent(const ModEvent&) override;

private:
	inline static constexpr uint32_t areaMusicOverrideSourcePluginId = 0x00040001;

	struct LiveWwiseOffsets
	{
		size_t segmentMarkerArray;
		size_t segmentMarkerCount;
		size_t segmentDuration;
		size_t trackSourceArray;
		size_t trackSourceCount;
		size_t trackTimingArray;
		size_t trackTimingCount;
		size_t trackSourceDuration;
	};

	struct AkSourceSettings
	{
		uint32_t sourceId = 0;
		uint32_t reserved0 = 0;
		void* mediaMemory = nullptr;
		uint32_t mediaSize = 0;
		uint32_t reserved1 = 0;
	};

	struct AreaMusicManagerBuffer
	{
		std::string path{};
		std::vector<uint8_t> bytes{};
		uint32_t sourcePluginId = areaMusicOverrideSourcePluginId;
		long long durationMs = 0;
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

	struct NativeAreaMusicBackup
	{
		bool valid = false;
		uint32_t segmentId = 0;
		uint32_t trackId = 0;
		uint32_t origSegmentDurationTicks = 0;
		std::vector<uint32_t> origMarkerPositionTicks;
		uint32_t origTrackSourceDurationTicks = 0;
		std::vector<uint8_t> origTrackTimingBytes;
	};

	static void HandleRegisterRequest(AreaMusic::RegisterRequest&);

	static bool ResolveWwiseMediaFunctions();
	static bool LoadAreaMusicManager(const char*);
	static void Unset();
	static void RetireBuffer();

	static bool InstallWwiseObjectLookupHook();
	static void CaptureWwiseObjectRegistry(void*);
	static void ReleaseWwiseObject(void*);
	static void* __cdecl WwiseObjectLookupHook(void*, uint32_t, int);
	static void* LookupWwiseObjectInTable(uint32_t, int);
	static void* LookupWwiseObject(uint32_t);
	static const LiveWwiseOffsets& GetLiveWwiseOffsets();
	static bool PatchLiveAreaMusicMetadata(const MusicData*, long long, long long);
	static void RestoreLiveAreaMusicMetadata();
	static bool PatchNativeAreaMusicOffset(const AreaMusic::AreaMusicChain*, long long);
	static void RestoreNativeAreaMusicOffset();

private:
	inline static constexpr const char* logPrefix = "Area Music Manager";

	using AkSetMediaFn = uint32_t(__cdecl*)(AkSourceSettings*, unsigned long);
	using AkUnsetMediaFn = uint32_t(__cdecl*)(AkSourceSettings*, unsigned long);
	using WwiseObjectLookupFn = void* (__cdecl*)(void*, uint32_t, int);

	inline static constexpr const char* setMediaExportName =
		"?SetMedia@SoundEngine@AK@@YA?AW4AKRESULT@@PEAUAkSourceSettings@@K@Z";
	inline static constexpr const char* unsetMediaExportName =
		"?UnsetMedia@SoundEngine@AK@@YA?AW4AKRESULT@@PEAUAkSourceSettings@@K@Z";

	inline static bool wwiseMediaFunctionsResolved = false;
	inline static AkSetMediaFn setMediaFunc = nullptr;
	inline static AkUnsetMediaFn unsetMediaFunc = nullptr;
	inline static bool wwiseObjectLookupHookInitialized = false;
	inline static WwiseObjectLookupFn originalWwiseObjectLookupFunc = nullptr;
	inline static std::atomic<uintptr_t> wwiseObjectRegistryAddress{ 0 };
	inline static std::mutex areaMusicMetadataMutex{};
	inline static std::unique_ptr<AreaMusicManagerBuffer> areaMusicOverrideBuffer{};
	inline static std::vector<std::unique_ptr<AreaMusicManagerBuffer>> retiredAreaMusicManagerBuffers{};
	inline static LiveAreaMusicMetadataBackup liveAreaMusicMetadataBackup{};
	inline static uint32_t areaMusicOverrideRegisteredMediaId = 0;
	inline static bool areaMusicOverrideRegistered = false;
	inline static bool areaMusicOverrideMetadataPatched = false;
	inline static NativeAreaMusicBackup nativeAreaMusicBackup{};
};
