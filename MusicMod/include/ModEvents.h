#pragma once

#include <any>

enum class ModEventType
{
	PreExitTriggered,
	ScanCompleted,
	FrameRendered,

	InputPressResolved,
	InputDownResolved,
	InputUpResolved,
	
	CompassStateChanged,
	UIButtonPressed,
	
	MusicPlayerStarted,
	MusicPlayerShuffled,
	MusicPlayerStopped,
	MusicPlayerInterrupted,

	ChiralNetworkStateChanged
};

struct ModEvent
{
	ModEventType type;
	void* sender;
	std::any data;
};