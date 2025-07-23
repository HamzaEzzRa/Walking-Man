#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "BreakpointWatcher.h"

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
	void PlayMusic(const MusicData*, bool);

	void PlayCurrentSong();
	void PlayNextSong();
	void PlayPreviousSong();

	void PlayNextAmbient();
	void PlayPreviousAmbient();

	void PlayNextUnknown();
	void PlayPreviousUnknown();

	void PlayByName(const std::string&);
	void StopMusic();

	static void PlayMusicHook(void*, void*, void*, void*);
	static void PlayUISoundHook(void*, void*, void*, void*);

private:
	enum LoopMode
	{
		ALL,
		ONE,
		NONE,
		COUNT // Just for size, not a valid mode
	};

	Logger logger = Logger("Music Player");

	inline static PlaybackQueue<const MusicData*> unknownQueue{};
	inline static PlaybackQueue<const MusicData*> ambientQueue{};
	inline static PlaybackQueue<const MusicData*> songQueue{};

	inline static uintptr_t playMusicFuncRCXAddress = 0;
	inline static uintptr_t playMusicFuncRDXAddress = 0;
	inline static uintptr_t offsetMenuToInGameRDX = -64;
	inline static bool gameCalledMusic = false;
	
	inline static const MusicData* currentMusicData = nullptr;
	inline static std::chrono::time_point<std::chrono::steady_clock> currentMusicStartTime;
	inline static std::atomic<bool> currentMusicIsPlaying = false;

	inline static LoopMode loopMode = LoopMode::ALL;
	uint16_t timeTillAutoplay = 1000; // in ms, time to wait before playing next song automatically

	inline static uintptr_t playingLoopAddress = 0;
	
	// Tracks playing loop instruction that accesses currentMusicAddress (+0x08 to be exact), helps detect song end
	std::unique_ptr<BreakpointWatcher> musicAddressWatcher;
	inline static uint8_t musicAddressAccessOffset = 0x08;
	inline static uintptr_t watchedInstructionLength = 4; // in bytes
	inline static uint32_t watcherPollingInterval = 10; // ms

	bool descriptionDisplayed = false;
	uint16_t displayDuration = 6500; // in ms
	std::chrono::time_point<std::chrono::steady_clock> lastDisplayTime;
};