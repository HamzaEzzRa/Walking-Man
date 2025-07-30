#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "IEventListener.h"
#include "InputCode.h"
#include "InputTracker.h"
#include "FunctionHook.h"
#include "UIButton.h"

#include "ordered_map.h"

#include "Logger.h"

class UIManager : public IEventListener, public FunctionHook
{
public:
	struct RuntimeUIText
	{
		uint8_t refCount;	// 0x00 -> smart pointer reference count. Once it reaches 0, the memory is freed
		uint8_t padding[3];	// 0x01 -> padding, usually 0x00 0x00 0x00
		int32_t unused;		// 0x04 -> ??? (usually 0xFFFFFFFF)
		int32_t alignment;	// 0x08 -> might be a text alignment identifier (0x0C for compass UI, left-aligned?)
		int32_t length;		// 0x0C -> number of UTF-16 wchar_t entries (including null)
		wchar_t data[];		// 0x10 -> UTF-16 string + null terminator
	};
	inline static std::mutex updateRuntimeUITextMutex;

	// Icons with no text in compass mode. Should be okay to play around with
	// Index commands order of appearance in UI (lowest idx shows at bottom of UI)
	// HOWEVER, I found another way to force the order in the static pool
	inline static std::unordered_map<const char*, size_t> iconSlotIndexMap = {
		/*{ "A", 0 }, { "SPACE", 0 },
		{ "X", 3 }, { "F", 3 },
		{ "LB", 6 }, { "G", 6 },
		{ "RT", 7 }, { "LMB", 7 },
		{ "LS", 8 }, { "SHIFT", 8 },
		{ "Y", 9 }, { "E", 9 },
		{ "RS", 11 }, { "T", 11 },
		{ "LT", 13 }, { "LMB", 13 },
		{ "RB", 14 }, { "Q", 14 },
		{ "SELECT", 16 }, { "5", 16 },
		{ "START", 17 }, { "TAB", 17 },
		{ "DPAD_UP", 19 }, { "4", 19 },*/
		// Already used in compass
		//{ "DPAD_RIGHT", 20 }, { "1", 20 },
		//{ "DPAD_LEFT", 21 }, { "3", 21 },
		//{ "DPAD_DOWN", 22 }, { "2", 22 }

		{ "A", 18 }, { "SPACE", 18 },
		{ "DPAD_UP", 19 }, { "4", 19 },
		{ "RT", 45 }, { "R", 45 }
	};

	inline static tsl::ordered_map<UIButtonAction, UIButton> musicPlayerActionButtonMap =
	{
		{
			UIButtonAction::TOGGLE_MUSIC,
			{
				"A", UIButtonAction::TOGGLE_MUSIC, // Action is redundant but nice to have if button passed as event data
				{ {0, "Play Music"}, {1, "Stop Music"} },
				{ {GAMEPAD_A, InputSource::GAMEPAD}, {VK_SPACE, InputSource::KBM} }
			}
		},
		{
			UIButtonAction::TOGGLE_LOOP_MODE,
			{
				"DPAD_UP", UIButtonAction::TOGGLE_LOOP_MODE,
				{ {0, "Loop Mode (All)"}, {1, "Loop Mode (One)"}, {2, "Loop Mode (None)"} },
				{ {GAMEPAD_DPAD_UP, InputSource::GAMEPAD}, {'4', InputSource::KBM}, {VK_NUMPAD4, InputSource::KBM} }
			}
		},
		{
			UIButtonAction::SHUFFLE_PLAYLIST,
			{
				"RT", UIButtonAction::SHUFFLE_PLAYLIST,
				{ {0, "Shuffle Playlist"}, {1, "Reset Playlist"} },
				{ {GAMEPAD_RT, InputSource::GAMEPAD}, {'R', InputSource::KBM}}
			}
		}
	};

	enum CompassState
	{
		CLOSED,
		OPEN,
		INCOMPATIBLE, // When sitting down, etc. Music player UI should not display
	};
	struct CompassStateData
	{
		// Consecutive calls to draw icon func with RDX following these values indicate state
		CompassState state;
		uint8_t expectedIconValue; // icon values seem to change across versions, we skip them in checks for now
		const char* expectedUITextContent; // we check only text content, icon values are not reliable
	};
	inline static std::unordered_map<CompassState, std::vector<CompassStateData>> compassStateReferenceMap =
	{
		{OPEN, {
			CompassStateData{OPEN, 0x0, "Zoom In/Out"}
		}},
		{INCOMPATIBLE, {
			CompassStateData{INCOMPATIBLE, 0x0, "Stand Up"},
			CompassStateData{INCOMPATIBLE, 0x0, "Descend/Ascend"}
		}}
	};

	enum MusicPlayerUIBlocker: uint8_t
	{
		NONE = 0,
		BT_BLOCK = 1 << 0, // Blocked by BT detection
		MULE_BLOCK = 1 << 1, // Blocked by MULE detection
		CHIRAL_BLOCK = 1 << 2, // Blocked by Chiral Network turned off
	};
	uint8_t musicPlayerUIBlockers = 0; // Bitmask of events preventing music player UI from showing

	UIManager();
	void OnEvent(const ModEvent&) override;

	void OnScanDone();
	void OnRender();
	void OnInputPress(const InputCode&);

	void UpdateMusicPlayerUIBlockers(MusicPlayerUIBlocker, bool);

private:
	static void InGameUIUpdateStaticPoolCallerHook(void*, void*, void*, void*);
	static void AccessStaticUIPoolHook(void*, void*, void*, void*);

	static void UpdateRuntimeUITextHook(void*, void*, void*, void*);

	static void InGameUIDrawElementHook(void*, void*, void*, void*);
	static void InGameUIUpdateElementHook(void*, void*, void*, void*);
	static void PostMenuExitHook(void*, void*, void*, void*);

	static void ShowNotificationText(const char*);

	static bool CheckForCompassState(CompassState, uint8_t, const std::wstring_view&);
	static void ResetModCompassState();

private:
	Logger logger{ "UI Manager" };

	// AccessStaticUIPool 4th argument is an address close to the ui static pool start (pool = arg + 0xC0)
	// This is the closest we can get to the static pool from a function. Offsets from other pools change across versions
	inline static size_t offsetToStaticUIPool = 0xC0;
	inline static uintptr_t staticUIPoolAddress = 0;

	inline static uintptr_t staticSlotTextIdOffset = 0x20; // Offset of the text ID in the static pool
	inline static uintptr_t staticSlotFlagOffset = 0x25; // Offset of the active flag in the static pool
	inline static uintptr_t staticSlotGrayedFlagOffset = 0x2A; // Offset to flag indicating whether the icon is grayed out
	inline static uintptr_t staticSlotOrderOffset = 0x57; // Offset of the order in the static pool (3 bytes available, 56-58)
	inline static uintptr_t staticSlotRuntimeOrderOffset = 0x44; // Final order at the runtime pool
	
	inline static size_t staticSlotSize = 0x60; // Size of each static slot in the pool

	inline static size_t notificationTextOffset = 0x28;
	inline static size_t notificationLockOffset = 0xFD0; // Need to acquire this lock before drawing notification text

	inline static uintptr_t updateRuntimeUITextRCXOffset = 0xD0;
	inline static uintptr_t runtimeUITextPoolOffset = 0;

	// Runtime slot size changes across versions (0x1D8 for standard, 0x228 for director's cut)
	// Calculate dynamically from inGameUIDrawElementFunc 1st arg instead
	inline static uintptr_t runtimeSlotSize = 0;
	inline static size_t maxRuntimeSlots = 8;

	inline static uintptr_t currentRuntimeUIPoolStart = 0;
	inline static uintptr_t runtimeUITextOffsetToIconId = -20; // Offset to the icon ID from the runtime UI text
	inline static uintptr_t runtimeUITextOffsetToActiveFlag = -24; // Offset to the active flag from the runtime UI text
	
	inline static uint8_t expectedCompassIconValue = 0xF6; // Expected icon value to confirm that compass UI is open
	inline static const char* expectedCompassUITextMatch = "Zoom In/Out";

	inline static CompassState currentCompassState = CLOSED;
};