#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "IEventListener.h"

struct MusicData;

class AreaMusicManager : public IEventListener
{
public:
	inline static constexpr const char* TargetTrackName = "Don't Be So Serious";

	AreaMusicManager();

	void OnEvent(const ModEvent&) override;

	static void Initialize();
	static bool UsesOverride(const MusicData*);
	static bool IsTemplateTrack(const MusicData*);
	static bool Register(const MusicData*);
	static void Unset();
	static void RetireBuffer();
	static void SetNextPlaybackStartOffsetMs(long long);
	static bool DurationControlledByWwise();
	static long long GetRegisteredEffectiveDurationMs(const MusicData*);

private:
	struct AreaMusicMetadataPatch;
	struct LiveWwiseOffsets;

	// Registration and media lifecycle
	static bool ResolveWwiseMediaFunctions();
	static bool LoadAreaMusicManager(const char*);
	static void ClearAreaMusicMetadataPatch();

	// Wwise bank metadata
	static bool FindAreaMusicBankMetadata(const uint8_t*, size_t);
	static bool PatchAreaMusicHircMetadata(uint8_t*, size_t, const AreaMusicMetadataPatch*);
	static bool ReloadAreaMusicBankMetadata(bool);
	static void RememberAreaMusicBankMemory(const void*, size_t, uint32_t, const char*);
	static void RestoreAreaMusicBankMetadata();

	// Live Wwise object metadata
	static bool InstallWwiseObjectLookupHook();
	static void CaptureWwiseObjectRegistry(void*);
	static void* __cdecl WwiseObjectLookupHook(void*, uint32_t, int);
	static void* LookupWwiseObjectInTable(uint32_t, int);
	static void* LookupWwiseObject(uint32_t);
	static const LiveWwiseOffsets& GetLiveWwiseOffsets();
	static bool PatchLiveAreaMusicMetadata(const MusicData*, long long, long long);
	static void RestoreLiveAreaMusicMetadata();

	// Wwise hook
	static uint32_t __cdecl LoadBankMemoryCopyHook(const void*, unsigned long, unsigned long*);
	static uint32_t __cdecl LoadBankMemoryViewHook(const void*, unsigned long, unsigned long*);
	static uint32_t __cdecl SetStateHook(uint32_t, uint32_t);

public:
	struct LastSetStateObservation
	{
		uint32_t stateGroup = 0;
		uint32_t stateValue = 0;
		long long timestampMs = 0;
		bool valid = false;
	};

	static LastSetStateObservation GetLastSetStateObservation();

	// Open a logging window where WwiseObjectLookup calls for music HIRC items (segment/track/ranseq) are logged.
	// Used to discover the segment/track/source chain associated with each area song at runtime.
	static void OpenChainCaptureWindow(const char* tag, long long durationMs);

	// Per-song Wwise chain inside CAkMusicSwitchCntr 878146756. Captured 2026-05-11 (Steam DC) via runtime
	// WwiseObjectLookup logging + offline HIRC parsing. See memory/area_music_offset_research.md for the table.
	struct AreaMusicChain
	{
		const char* songName;
		uint32_t ranseqId;
		uint32_t segmentId;
		uint32_t trackId;
		uint32_t sourceId;
	};

	static const AreaMusicChain* LookupChainForSong(const char* songName);

	// Patches the live Wwise segment + track for `chain` so the next playback starts at `sourceStartMs`.
	// Unlike the existing Register() / PatchLiveAreaMusicMetadata path which is tied to DBSS's chain and the
	// custom-WEM-override flow, this function only adjusts timing (BeginTrim / segment duration / markers) — it
	// does NOT rewrite source IDs, so it's safe to use on native area songs without affecting the source media.
	// Caller must invoke RestoreNativeAreaMusicOffset() before another song plays, on stop, and on pre-exit.
	static bool PatchNativeAreaMusicOffset(const AreaMusicChain* chain, long long sourceStartMs);
	static void RestoreNativeAreaMusicOffset();

	// Diagnostic: log the current value at the patched offsets on `chain`'s segment + track. Used to verify whether
	// our writes are still in place at a given moment (e.g. after game's PlayMusic returns) or whether Wwise's audio
	// thread overwrote them during the state transition.
	static void DiagnosticLogChainState(const AreaMusicChain* chain, const char* tag);

private:
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
	using WwiseObjectLookupFn = void* (__cdecl*)(void*, uint32_t, int);
	using AkSetStateFn = uint32_t(__cdecl*)(uint32_t, uint32_t);

	inline static constexpr uint32_t akSuccess = 1;
	inline static constexpr uint32_t areaMusicOverrideMediaId = 14330364;
	inline static constexpr uint32_t areaMusicOverrideTrackId = 864381405;
	inline static constexpr uint32_t areaMusicOverrideSegmentId = 515599299;
	inline static constexpr uint32_t areaMusicOverrideRanSeqId = 124768037;
	inline static constexpr uint32_t areaMusicSwitchCntrId = 878146756;
	inline static constexpr uint32_t areaMusicStateGroupId = 0x1A2853AC;
	inline static constexpr uint32_t areaMusicOverrideStateValue = 0x87D41010;
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
	inline static constexpr const char* setStateExportName =
		"?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@KK@Z";

	struct AreaMusicMetadataPatch
	{
		bool enabled = false;
		long long durationMs = 0;
		long long sourceStartMs = 0;
		uint32_t mediaId = areaMusicOverrideMediaId;
		uint32_t mediaSize = 0;
		uint32_t sourcePluginId = areaMusicOverrideSourcePluginId;
	};

	struct AreaMusicManagerBuffer
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
	inline static bool wwiseObjectLookupHookInitialized = false;
	inline static WwiseObjectLookupFn originalWwiseObjectLookupFunc = nullptr;
	inline static std::atomic<uintptr_t> wwiseObjectRegistryAddress{ 0 };
	inline static std::mutex areaMusicMetadataMutex{};
	inline static AreaMusicMetadataPatch areaMusicMetadataPatch{};
	inline static std::atomic<long long> nextPlaybackStartOffsetMs{ 0 };
	inline static std::vector<AreaMusicBankMemoryView> areaMusicBankMemoryViews{};
	inline static std::unique_ptr<AreaMusicManagerBuffer> areaMusicOverrideBuffer{};
	inline static std::vector<std::unique_ptr<AreaMusicManagerBuffer>> retiredAreaMusicManagerBuffers{};
	inline static LiveAreaMusicMetadataBackup liveAreaMusicMetadataBackup{};
	inline static uint32_t areaMusicOverrideRegisteredMediaId = 0;
	inline static bool areaMusicOverrideRegistered = false;
	inline static bool areaMusicOverrideMetadataPatched = false;

	inline static AkSetStateFn setStateFunc = nullptr;
	inline static AkSetStateFn originalSetStateFunc = nullptr;
	inline static bool setStateHookInitialized = false;
	inline static std::mutex lastSetStateMutex{};
	inline static LastSetStateObservation lastSetStateObservation{};

	// Music-HIRC ID set populated from in-memory bank at first load; used to filter the chain-capture log down
	// to music-related Wwise objects (segment / track / ranseq / switch-cntr).
	inline static std::mutex musicIdSetMutex{};
	inline static std::unordered_map<uint32_t, uint8_t> musicHircIdToType{}; // id -> HIRC type byte
	inline static std::atomic<long long> chainCaptureExpiryMs{ 0 };
	inline static std::mutex chainCaptureTagMutex{};
	inline static std::string chainCaptureTag{};

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
	inline static NativeAreaMusicBackup nativeAreaMusicBackup{};
};
