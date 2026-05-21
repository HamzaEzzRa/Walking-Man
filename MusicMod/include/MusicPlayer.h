#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "IEventListener.h"
#include "FunctionHook.h"

#include "GameData.h"
#include "PlaybackQueue.h"
#include "UIButton.h"

#include "Logger.h"

class MusicPlayer : public IEventListener, public FunctionHook
{
public:
	MusicPlayer();
	void OnEvent(const ModEvent&) override;

	void OnScanDone();
	void OnRender();
	void OnPreExit();

	void OnUIButtonAction(const UIButtonAction&);
	void OnInputPress(const InputCode&);

private:
	void OnFunctionScanDone();

	void PlayMusic(const MusicData*, bool = true, long long = 0);

	void PlayCurrentSong();
	void PlayNextSong();
	void PlayPreviousSong();

	void PlayByName(const std::string&);
	void StopMusic();
	static void CancelPendingAreaMusicTransition(const char*);
	void HandleMusicBlockerChange(bool, bool, const char*, const char*, const char*);
	bool PauseCurrentMusicForBlocker(const char*);
	bool ResumeCurrentMusicForBlocker(const char*);
	bool RestartCurrentMusicFromSavedPosition(const char*);
	static bool HasActiveMusicBlocker();
	bool QueueAreaMusicTransition(const MusicData*, bool);
	bool PlaySilenceForAreaMusicTransition();
	bool UpdateCurrentMusicCursor();
	static bool ReadMusicCursor(uint32_t, uint32_t, uint32_t, long long&);
	static bool ResolveCursorSource(const MusicData*, uint32_t&, uint32_t&);
	static void ResetCurrentMusicCursor();
	static bool ClearMusicDescription();
	bool ShowMusicDescription(const MusicData*);
	bool ShowMusicDescriptionNow(const MusicData*);
	static bool IsTrackUnlocked(const MusicData*);
	static void CacheUnlockFacts(void*);

	void PlayNextInPool();
	void PlayPreviousInPool();

	static void PlayMusicHook(void*, void*, void*, void*);
	static void WwiseSourceCursorUpdateHook(void*, uint32_t, void*, void*);
	static void PlayUISoundHook(void*, void*, void*, void*);
	static void ShowMusicDescriptionCoreHook(void*, void*, void*, void*);
	static void DSCollectorsItemSystemLoadHook(void*, void*, void*, void*);

	enum LoopMode
	{
		ALL,
		ONE,
		NONE,
		COUNT // Just for size, not a valid mode
	};

	struct WwiseSourcePosition
	{
		uint32_t trackId;
		uint32_t sourceId;
		uint32_t positionMs;
	};

	inline static constexpr const char* logPrefix = "Music Player";

	inline static PlaybackQueue<const MusicData*> songQueue{};

	// Pool queue is for dev purposes, helps speed up finding game music data
	inline static PlaybackQueue<const MusicData*> poolQueue{};
	inline static size_t poolMusicDataSize = 48; // Size of each entry in the pool, used to calculate potential next address

	inline static uintptr_t playMusicFuncRCXAddress = 0;
	inline static uintptr_t playMusicFuncRDXAddress = 0;

	inline static std::unordered_map<std::string, void*> musicUnlockFacts{};
	inline static bool musicUnlockFactsCached = false;
	inline static uintptr_t gamePassCollectorsItemSystemGlobalAddress = 0;

	inline static bool gameCalledSong = false;
	inline static bool gameCalledInterruptor = false;
	inline static std::atomic<long long> currentMusicPlayTime = 0; // in ms, Wwise source cursor for current playback
	inline static std::atomic<long long> currentMusicMaxLength = 0; // in ms, runtime guard for current playback

	inline static const MusicData* currentMusicData = nullptr;
	inline static std::atomic<bool> currentMusicIsPlaying = false;
	inline static std::atomic<bool> currentMusicPausedByBlocker = false;
	inline static std::atomic<uint32_t> currentMusicPlayingId = 0;
	inline static std::atomic<uint32_t> currentMusicTrackId = 0;
	inline static std::atomic<uint32_t> currentMusicSourceId = 0;
	inline static std::atomic<bool> currentMusicCursorSeen = false;
	inline static std::atomic<long long> currentMusicEndDetectedMs = 0;
	inline static std::mutex currentMusicCursorMutex{};
	inline static const MusicData* pendingMusicData = nullptr;
	inline static bool pendingMusicDisplayDescription = true;
	inline static bool pendingMusicOverridePrepared = false;
	inline static std::chrono::time_point<std::chrono::steady_clock> pendingMusicStartTime;

	inline static std::atomic<bool> btTerritoryBlocksMusic = false;
	inline static std::atomic<bool> muleTerritoryBlocksMusic = false;
	inline static std::atomic<bool> facilityTerritoryBlocksMusic = false;
	inline static std::atomic<bool> chiralNetworkBlocksMusic = false;

	inline static LoopMode loopMode = LoopMode::ALL;
	uint16_t timeTillAutoplay = 1000; // in ms, time to wait before playing next song automatically

	inline static void* musicDescriptionManager = nullptr;

	bool descriptionDisplayed = false;
	const MusicData* pendingMusicDescriptionData = nullptr;
	uint16_t displayDuration = 6500; // in ms
	std::chrono::time_point<std::chrono::steady_clock> lastDisplayTime;
};
