#pragma once

#include <any>

enum class ModEventType
{
	NoEvent = -1,

	PreExitTriggered,
	FunctionScanCompleted,
	ScanCompleted,
	FrameRendered,

	TextLanguageChanged,

	InputPressResolved,
	InputDownResolved,
	InputUpResolved,

	CompassStateChanged,
	UIButtonPressed,

	MusicPlayerStarted,
	MusicPlayerShuffled,
	MusicPlayerStopped,
	MusicPlayerInterrupted,

	AreaMusicRegisterRequested,
	AreaMusicUnsetRequested,
	AreaMusicPatchNativeOffsetRequested,
	AreaMusicRestoreNativeOffsetRequested,

	BTTerritoryStateChanged,
	MuleTerritoryStateChanged,
	FacilityBlockStateChanged,
	ChiralNetworkStateChanged
};

struct ModEvent
{
	ModEventType type;
	void* sender;
	std::any data;
};
