#pragma once

#include <any>

enum class ModEventType
{
	PreExitTriggered,
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

	BTTerritoryStateChanged,
	MuleTerritoryStateChanged,
	ChiralNetworkStateChanged
};

struct ModEvent
{
	ModEventType type;
	void* sender;
	std::any data;
};