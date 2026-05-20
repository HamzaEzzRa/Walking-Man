#include "MusicPlayer.h"

#include "AreaMusicData.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

#include "CustomMediaLoader.h"
#include "GameStateManager.h"
#include "ModConfiguration.h"
#include "ModManager.h"
#include "PlaybackQueue.h"

#include "MemoryUtils.h"
#include "MinHook.h"

#include "UIButton.h"

#include "Utils.h"

namespace
{
	constexpr size_t collectorsItemCountOffset = 0x10;
	constexpr size_t collectorsItemDataOffset = 0x18;
	constexpr size_t collectorsItemUnlockFactOffset = 0x68;
	constexpr size_t booleanFactUuidOffset = 0x08;
	constexpr int32_t maxCollectorsItems = 4096;

	std::unordered_map<std::string, void*> musicUnlockFacts;
	bool musicUnlockFactsCached = false;
	uintptr_t gamePassCollectorsItemSystemGlobalAddress = 0;
}

MusicPlayer::MusicPlayer()
{
}

void MusicPlayer::CancelPendingAreaMusicTransition(const char* reason)
{
	if (!pendingMusicData)
	{
		return;
	}

	constexpr const char* logPrefix = "Music Player";
	Logging::Write(logPrefix,
		"Canceling queued area music track \"%s\"%s%s",
		pendingMusicData->name ? pendingMusicData->name : "",
		reason && reason[0] ? ": " : "",
		reason && reason[0] ? reason : ""
	);
	pendingMusicData = nullptr;
	pendingMusicDisplayDescription = true;
	pendingMusicOverridePrepared = false;
	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(0);
	currentMusicSourceStartOffsetMs.store(0);
	if (ModManager* instance = ModManager::GetInstance())
	{
		instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
	}
}

void MusicPlayer::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
		case ModEventType::FunctionScanCompleted:
		{
			OnFunctionScanDone();
			break;
		}
		case ModEventType::ScanCompleted:
		{
			OnScanDone();
			break;
		}
		case ModEventType::FrameRendered:
		{
			OnRender();
			break;
		}
		case ModEventType::PreExitTriggered:
		{
			OnPreExit();
			break;
		}
		case ModEventType::UIButtonPressed:
		{
			UIButtonAction action = std::any_cast<UIButtonAction>(event.data);
			OnUIButtonAction(action);
			break;
		}
		case ModEventType::InputPressResolved:
		{
			const InputCode& inputCode = std::any_cast<InputCode>(event.data);
			OnInputPress(inputCode);
			break;
		}
		case ModEventType::BTTerritoryStateChanged:
		case ModEventType::MuleTerritoryStateChanged:
		{
			auto* territoryFlagState = std::any_cast<FlagState<EnemyTerritoryFlag>*>(event.data);
			const bool wasBlocked = HasActiveMusicBlocker();
			std::atomic<bool>& territoryBlocker = event.type == ModEventType::BTTerritoryStateChanged
				? btTerritoryBlocksMusic
				: muleTerritoryBlocksMusic;
			territoryBlocker.store(territoryFlagState->current != EnemyTerritoryFlag::SAFE);
			const bool isBlocked = HasActiveMusicBlocker();

			HandleMusicBlockerChange(
				wasBlocked,
				isBlocked,
				"enemy territory",
				"enemy territory cleared",
				"enemy territory interruption"
			);
			break;
		}
		case ModEventType::FacilityBlockStateChanged:
		{
			if (ModConfiguration::stopInFacility)
			{
				auto* facilityFlagState = std::any_cast<FlagState<AreaFlag>*>(event.data);
				const bool wasBlocked = HasActiveMusicBlocker();
				facilityTerritoryBlocksMusic.store(facilityFlagState->current == AreaFlag::INSIDE);
				const bool isBlocked = HasActiveMusicBlocker();

				HandleMusicBlockerChange(
					wasBlocked,
					isBlocked,
					"in facility",
					"facility territory cleared",
					"facility territory interruption"
				);
			}
			break;
		}
		case ModEventType::ChiralNetworkStateChanged:
		{
			if (ModConfiguration::connectToChiralNetwork)
			{
				auto* chiralNetworkFlagState = std::any_cast<FlagState<ChiralNetworkFlag>*>(event.data);
				const bool wasBlocked = HasActiveMusicBlocker();
				chiralNetworkBlocksMusic.store(chiralNetworkFlagState->current == ChiralNetworkFlag::OFF);
				const bool isBlocked = HasActiveMusicBlocker();

				HandleMusicBlockerChange(
					wasBlocked,
					isBlocked,
					"chiral network off",
					"chiral network restored",
					"chiral network interruption"
				);
			}
			break;
		}
		default: break;
	}
}

void MusicPlayer::OnFunctionScanDone()
{
	const FunctionData* functionData = ModManager::GetFunctionData("DSCollectorsItemSystemLoad");
	if (
		ModConfiguration::gameProvider == GameProvider::XBOX_GAMEPASS
		&& functionData
		&& MemoryUtils::IsReadableAddress(functionData->address, 32)
	)
	{
		uint8_t* bytes = reinterpret_cast<uint8_t*>(functionData->address);
		for (size_t i = 0; i + 7 <= 32; ++i)
		{
			if (bytes[i] == 0x48 && bytes[i + 1] == 0x8B && bytes[i + 2] == 0x3D)
			{
				int32_t displacement = 0;
				std::memcpy(&displacement, bytes + i + 3, sizeof(displacement));
				gamePassCollectorsItemSystemGlobalAddress = functionData->address + i + 7 + displacement;
				break;
			}
		}
	}

	bool hookResult = ModManager::TryHookFunction(
		"DSCollectorsItemSystemLoad",
		reinterpret_cast<void*>(&MusicPlayer::DSCollectorsItemSystemLoadHook)
	);
	Logging::Write(logPrefix,
		"DSCollectorsItemSystemLoad function hook %s",
		hookResult ? "installed successfully" : "failed"
	);
}

void MusicPlayer::OnScanDone()
{
	bool hookResult = ModManager::TryHookFunction(
		"PlayMusic",
		reinterpret_cast<void*>(&MusicPlayer::PlayMusicHook)
	);
	Logging::Write(logPrefix,
		"PlayMusic function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	const FunctionData* playingLoopData = ModManager::GetFunctionData("PlayingLoop");
	if (!playingLoopData || !playingLoopData->address)
	{
		Logging::Write(logPrefix, "Failed to find PlayingLoop function signature");
	}
	else
	{
		playingLoopAddress = playingLoopData->address;
	}

	hookResult = ModManager::TryHookFunction(
		"PlayUISound",
		reinterpret_cast<void*>(&MusicPlayer::PlayUISoundHook)
	);
	Logging::Write(logPrefix,
		"PlayUISound function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"ShowMusicDescriptionCore",
		reinterpret_cast<void*>(&MusicPlayer::ShowMusicDescriptionCoreHook)
	);
	Logging::Write(logPrefix,
		"ShowMusicDescriptionCore function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	auto baseAreaTrackIt = ModConfiguration::Databases::songDatabase.find(AreaMusic::OverrideTarget.songName);
	if (baseAreaTrackIt != ModConfiguration::Databases::songDatabase.end())
	{
		CustomMediaLoader::BindCustomSongsToAreaTrack(baseAreaTrackIt->second);
	}
	else
	{
		Logging::Write(logPrefix,
			"Base area track \"%s\" not found in song database; area override tracks will not be bound",
			AreaMusic::OverrideTarget.songName
		);
	}

	std::vector<const MusicData*> songMusicDataList;
	for (const std::string& name : ModConfiguration::activePlaylist)
	{
		auto it = ModConfiguration::Databases::songDatabase.find(name);
		if (it == ModConfiguration::Databases::songDatabase.end())
		{
			auto customIt = ModConfiguration::Databases::customSongDatabase.find(name);
			if (customIt == ModConfiguration::Databases::customSongDatabase.end())
			{
				Logging::Write(logPrefix, "Song \"%s\" not found in database, skipping...", name.c_str());
				continue;
			}

			auto& customMusicData = customIt->second;
			if (!customMusicData.address)
			{
				Logging::Write(logPrefix, "Custom song \"%s\" has no bound area address, skipping...", name.c_str());
				continue;
			}
			songMusicDataList.push_back(&customMusicData);
			continue;
		}

		auto& songMusicData = it->second;
		if (!songMusicData.address)
		{
			Logging::Write(logPrefix, "Song \"%s\" has no valid address, skipping...", name.c_str());
			continue;
		}
		songMusicDataList.push_back(&songMusicData);
	}
	songQueue.SetData(songMusicDataList.data(), songMusicDataList.size());
	songQueue.Reset();

	if (ModConfiguration::devMode) // Only create pool queue in dev mode
	{
		std::vector<const MusicData*> poolMusicDataList;
		auto it = ModConfiguration::Databases::songDatabase.find("Music Pool Start");
		if (it != ModConfiguration::Databases::songDatabase.end())
		{
			auto& poolMusicData = it->second;
			if (poolMusicData.address)
			{
				poolMusicDataList.push_back(&poolMusicData);
				Logging::Write(logPrefix, "Pushed pool music %s with address %p",
					poolMusicData.name, poolMusicData.address
				);

				uintptr_t currentAddress = poolMusicData.address;
				uint64_t poolMusicSignature = *reinterpret_cast<uint64_t*>(poolMusicData.address);
				size_t index = 0;

				std::vector<size_t> skipIndices{};

				while (true)
				{
					currentAddress = currentAddress + poolMusicDataSize;
					uint64_t signature = *reinterpret_cast<uint64_t*>(currentAddress);
					if (signature != poolMusicSignature)
					{
						Logging::Write(logPrefix, "Pool music signature mismatch at address %p, pool ended", (void*)currentAddress);
						break;
					}

					std::string poolMusicName = "Pool Music " + std::to_string(++index);
					for (size_t skipIndex : skipIndices)
					{
						if (index == skipIndex + 1)
						{
							continue;
						}
					}

					MusicData* newData = (MusicData*)malloc(sizeof(MusicData));
					if (!newData)
					{
						Logging::Write(logPrefix, "Failed to allocate memory for new pool music data");
						continue;
					}
					newData->descriptionID = 0;
					newData->type = MusicType::UNKNOWN;
					newData->maxLength = 0;
					newData->name = _strdup(poolMusicName.c_str());
					newData->artist = "";
					newData->signature = "";
					newData->exclusiveDC = false;
					newData->address = currentAddress;
					newData->active = true;
					newData->customAreaTrack = false;
					newData->customWemPath = nullptr;
					newData->internalWwiseAreaTrack = {};

					poolMusicDataList.push_back(newData);

					Logging::Write(logPrefix, "Pushed pool music %s with address %p",
						newData->name, newData->address
					);
				}
			}
		}
		poolQueue.SetData(poolMusicDataList.data(), poolMusicDataList.size());
		poolQueue.Reset();
	}
}

void MusicPlayer::OnRender()
{
	if (
		pendingMusicData
		&& std::chrono::steady_clock::now() >= pendingMusicStartTime
	)
	{
		const MusicData* data = pendingMusicData;
		const bool displayDescription = pendingMusicDisplayDescription;

		if (!pendingMusicOverridePrepared && AreaMusic::UsesOverride(data))
		{
			AreaMusic::RegisterRequest registration{};
			registration.data = data;

			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{
					ModEventType::AreaMusicRegisterRequested,
					nullptr,
					&registration
				});
			}

			if (!registration.handled || !registration.success)
			{
				Logging::Write(logPrefix,
					"Area music override was not prepared for queued track \"%s\"; canceling playback",
					data->name ? data->name : ""
				);
				pendingMusicData = nullptr;
				pendingMusicDisplayDescription = true;
				pendingMusicOverridePrepared = false;
				if (ModManager* instance = ModManager::GetInstance())
				{
					instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
				}
				currentMusicData = nullptr;
				currentMusicIsPlaying.store(false);
				currentMusicPlayTime.store(0);
				currentMusicMaxLength.store(0);
				currentMusicSourceStartOffsetMs.store(0);
				currentMusicPausedByBlocker.store(false);
				return;
			}

			pendingMusicOverridePrepared = true;
			pendingMusicStartTime = std::chrono::steady_clock::now();
			Logging::Write(logPrefix, "Prepared queued area music override for \"%s\"", data->name ? data->name : "");
			return;
		}

		pendingMusicData = nullptr;
		pendingMusicDisplayDescription = true;
		pendingMusicOverridePrepared = false;
		Logging::Write(logPrefix, "Starting queued area music track: %s", data->name);
		PlayMusic(data, displayDescription);
	}

	// Handle Playback
	if (currentMusicData && !currentMusicPausedByBlocker.load())
	{
		const long long maxLength = currentMusicMaxLength.load();
		bool maxLengthReached = false;
		if (
			maxLength > 0
			&& maxLength < currentMusicPlayTime.load()
		)
		{
			Logging::Write(logPrefix, "Current music playback time exceeded its max duration");
			currentMusicPlayTime.store(0);
			currentMusicSourceStartOffsetMs.store(0);
			currentMusicIsPlaying.store(false);
			maxLengthReached = true;
		}

		if (!currentMusicIsPlaying.load())
		{
			if (gameCalledInterruptor || (gameCalledSong && ModConfiguration::allowScriptedSongs))
			{
				gameCalledSong = false;
				gameCalledInterruptor = false;

				if (AreaMusic::UsesOverride(currentMusicData))
				{
					if (ModManager* instance = ModManager::GetInstance())
					{
						instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
					}
				}
				currentMusicData = nullptr;
				if (ModManager* instance = ModManager::GetInstance())
				{
					const char* interruptionMessage = "Music player interrupted.";
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerInterrupted,
						this,
						&interruptionMessage
					});
				}
			}
			else
			{
				switch (loopMode)
				{
					case LoopMode::ALL:
					{
						Logging::Write(logPrefix, "Autoplaying next song...");
						PlayNextSong();
						break;
					}
					case LoopMode::ONE:
					{
						Logging::Write(logPrefix, "Autoplaying current song...");
						PlayCurrentSong();
						break;
					}
					case LoopMode::NONE:
					{
						if (maxLengthReached)
						{
							StopMusic();
						}
						else
						{
							currentMusicData = nullptr;
						}

						if (musicAddressWatcher)
						{
							musicAddressWatcher->Uninstall();
							musicAddressWatcher.reset();
						}

						if (ModManager* instance = ModManager::GetInstance())
						{
							instance->DispatchEvent(ModEvent{
								ModEventType::MusicPlayerStopped,
								this,
								nullptr
							});
						}
						break;
					}
					default: break;
				}
			}
		}
	}

	if (!currentMusicData && musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	// Handle Song Description
	if (pendingMusicDescriptionData)
	{
		const MusicData* pendingDescription = pendingMusicDescriptionData;
		pendingMusicDescriptionData = nullptr;
		ShowMusicDescriptionNow(pendingDescription);
	}

	if (descriptionDisplayed)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - lastDisplayTime >= std::chrono::milliseconds(displayDuration))
		{
			Logging::Write(logPrefix, "Hiding song description after %d ms", displayDuration);
			descriptionDisplayed = false;
			pendingMusicDescriptionData = nullptr;

			// Call the game's function with nullptr to fade out the description,
			// same way it does when a new description is queued
			const FunctionData* showMusicDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
			if (showMusicDescriptionFuncData && showMusicDescriptionFuncData->address)
			{
				reinterpret_cast<GenericFunction_t>(showMusicDescriptionFuncData->address)(
					nullptr,
					nullptr,
					nullptr,
					nullptr
				);
			}
		}
	}
}

void MusicPlayer::OnPreExit()
{
	currentMusicPausedByBlocker.store(false);
	CancelPendingAreaMusicTransition("pre-exit");
	pendingMusicDescriptionData = nullptr;
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicMaxLength.store(0);
	currentMusicSourceStartOffsetMs.store(0);
	btTerritoryBlocksMusic.store(false);
	muleTerritoryBlocksMusic.store(false);
	facilityTerritoryBlocksMusic.store(false);
	chiralNetworkBlocksMusic.store(false);
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}
}

void MusicPlayer::OnUIButtonAction(const UIButtonAction& action)
{
	switch (action)
	{
		case UIButtonAction::TOGGLE_MUSIC:
		{
			if (currentMusicIsPlaying.load())
			{
				Logging::Write(logPrefix, "Toggling music off...");
				StopMusic();
			}
			else
			{
				Logging::Write(logPrefix, "Toggling music on...");
				if (loopMode == LoopMode::ONE)
				{
					Logging::Write(logPrefix, "Loop mode is ONE, playing current song...");
					PlayCurrentSong();
				}
				else
				{
					Logging::Write(logPrefix, "Loop mode is ALL, playing next song...");
					PlayNextSong();
				}
			}
			break;
		}
		case UIButtonAction::SHUFFLE_PLAYLIST:
		{
			if (songQueue.IsShuffled())
			{
				songQueue.Reset();
				StopMusic();
			    if (ModManager* instance = ModManager::GetInstance())
				{
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerStopped,
						this,
						nullptr
					});
				}
			}
			else
			{
				songQueue.Shuffle();
			    if (ModManager* instance = ModManager::GetInstance())
				{
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerShuffled,
						this,
						nullptr
					});
				}
				PlayNextSong();
			}
			break;
		}
		case UIButtonAction::TOGGLE_LOOP_MODE:
		{
			loopMode = static_cast<LoopMode>(
				(static_cast<int>(loopMode) + 1) % static_cast<int>(LoopMode::COUNT)
			);
			Logging::Write(logPrefix, "Loop mode changed to: %d", static_cast<int>(loopMode));
			break;
		}
		default:
			break;
	}
}

void MusicPlayer::PlayMusicHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Play Music Hook";
	const bool blockerPausedByActiveBlocker = currentMusicPausedByBlocker.load() && HasActiveMusicBlocker();
	//Logging::Write(logPrefix, "PlayMusic called with args: %p, %p, %p, %p", arg1, arg2, arg3, arg4);

	if (!playMusicFuncRCXAddress)
	{
		playMusicFuncRCXAddress = reinterpret_cast<uintptr_t>(arg1);
		Logging::Write(logPrefix, "1st arg address set to: %p", (void*)playMusicFuncRCXAddress);
	}

	auto silenceIt = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (silenceIt != ModConfiguration::Databases::interruptorDatabase.end())
	{
		const MusicData& silenceData = silenceIt->second;
		if (reinterpret_cast<uintptr_t>(arg3) == silenceData.address)
		{
			uintptr_t arg2Address = reinterpret_cast<uintptr_t>(arg2);
			if (playMusicFuncRDXAddress != arg2Address)
			{
				playMusicFuncRDXAddress = arg2Address;
				Logging::Write(logPrefix, "2nd arg address set to: %p", (void*)playMusicFuncRDXAddress);
			}
		}
	}

	const char* gameInterruptorName = nullptr;
	bool gameCalledSilence = false;
	gameCalledInterruptor = false;
	for (const auto& [name, interruptorData] : ModConfiguration::Databases::interruptorDatabase)
	{
		if (reinterpret_cast<uintptr_t>(arg3) == interruptorData.address)
		{
			gameInterruptorName = interruptorData.name ? interruptorData.name : name.c_str();
			gameCalledSilence = name == "Silence";
			gameCalledInterruptor = !gameCalledSilence || blockerPausedByActiveBlocker;
			break;
		}
	}

	if (gameCalledSilence && blockerPausedByActiveBlocker)
	{
		return;
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function is not found, cannot play music");
		return;
	}

	gameCalledSong = false;
	const MusicData* songToPlay = nullptr;
	if (!gameCalledInterruptor)
	{
		for (const auto& [name, musicData] : ModConfiguration::Databases::songDatabase)
		{
			if (name == "Music Pool Start")
			{
				continue;
			}

			if (reinterpret_cast<uintptr_t>(arg3) == musicData.address)
			{
				gameCalledSong = true;
				songToPlay = &musicData;
				break;
			}
		}
	}

	if (songToPlay && gameCalledSong && !ModConfiguration::allowScriptedSongs)
	{
		Logging::Write(logPrefix,
			"Configuration does not allow scripted songs, skipping song %s",
			songToPlay->name
		);
		return;
	}

	if ((gameCalledInterruptor || gameCalledSong) && (currentMusicData || pendingMusicData))
	{
		const char* interruptionReason = gameCalledInterruptor
			? (gameInterruptorName ? gameInterruptorName : "game interruptor")
			: (songToPlay && songToPlay->name ? songToPlay->name : "game music");
		if (blockerPausedByActiveBlocker)
		{
			if (!gameCalledSilence)
			{
				Logging::Write(logPrefix,
					"Blocked game music interruption \"%s\" while area music is blocker-paused",
					interruptionReason
				);
			}
			return;
		}

		currentMusicPausedByBlocker.store(false);
		CancelPendingAreaMusicTransition(interruptionReason);
		if (AreaMusic::UsesOverride(currentMusicData))
		{
			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
			}
		}
		currentMusicData = nullptr;
		currentMusicIsPlaying.store(false);
		currentMusicPlayTime.store(0);
		currentMusicMaxLength.store(0);
		currentMusicSourceStartOffsetMs.store(0);

		if (ModManager* instance = ModManager::GetInstance())
		{
			const char* interruptionMessage = "Music player interrupted.";
			instance->DispatchEvent(ModEvent{
				ModEventType::MusicPlayerInterrupted,
				nullptr,
				&interruptionMessage
			});
		}
	}

	if (
		songToPlay
		&& songToPlay->name
		&& std::strcmp(songToPlay->name, AreaMusic::OverrideTarget.songName) == 0
	)
	{
		if (ModManager* instance = ModManager::GetInstance())
		{
			instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
		}
	}

	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::PlayUISoundHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Play UI Sound";

	if ((currentMusicData || pendingMusicData) && arg1 && arg2)
	{
		std::vector<uint8_t> targetBytes;
		std::vector<bool> masks;
		for (const auto& nameInterruptorPair : ModConfiguration::Databases::interruptorUIDatabase)
		{
			bool match = true;
			const MusicData& interruptor = nameInterruptorPair.second;

			targetBytes.clear();
			masks.clear();
			MemoryUtils::ParseHexString(interruptor.signature, targetBytes, masks);
			for (size_t i = 0; i < targetBytes.size(); i++)
			{
				if (masks[i])
				{
					if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(arg2) + i) != targetBytes[i])
					{
						match = false;
						break;
					}
				}
			}

			if (match)
			{
				Logging::Write(logPrefix, "Interruptor %s matched, skipping UI sound", interruptor.name);
				return;
			}
		}
	}

	const FunctionData* playUISoundFuncData = ModManager::GetFunctionData("PlayUISound");
	if (!playUISoundFuncData || !playUISoundFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play UI sound function is not found, cannot play sound.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(playUISoundFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::ShowMusicDescriptionCoreHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Show Music Description Core Hook";

	if (arg1)
	{
		musicDescriptionManager = arg1;
	}

	const FunctionData* showMusicDescriptionCoreFuncData = ModManager::GetFunctionData("ShowMusicDescriptionCore");
	if (!showMusicDescriptionCoreFuncData || !showMusicDescriptionCoreFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original show music description core function was not hooked, cannot show description");
		return;
	}

	if (gameCalledSong && !ModConfiguration::allowScriptedSongs)
	{
		Logging::Write(logPrefix, "Configuration does not allow scripted songs, skipping song description");
		return;
	}

	reinterpret_cast<GenericFunction_t>(showMusicDescriptionCoreFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::DSCollectorsItemSystemLoadHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* functionData = ModManager::GetFunctionData("DSCollectorsItemSystemLoad");
	if (!functionData || !functionData->originalFunction)
	{
		return;
	}

	using DSCollectorsItemSystemLoad_t = void (*)(void*, void*, void*, void*);
	reinterpret_cast<DSCollectorsItemSystemLoad_t>(functionData->originalFunction)(arg1, arg2, arg3, arg4);

	void* system = arg1;
	if (ModConfiguration::gameProvider == GameProvider::XBOX_GAMEPASS)
	{
		system = nullptr;
		if (MemoryUtils::IsReadableAddress(gamePassCollectorsItemSystemGlobalAddress, sizeof(void*)))
		{
			system = *reinterpret_cast<void**>(gamePassCollectorsItemSystemGlobalAddress);
		}
	}

	CacheUnlockFacts(system);
}

void MusicPlayer::CacheUnlockFacts(void* system)
{
	musicUnlockFacts.clear();
	musicUnlockFactsCached = false;
	if (!system)
	{
		Logging::Write(logPrefix, "DSCollectors item system was not available; music unlock gating will fail open");
		return;
	}

	const uintptr_t systemAddress = reinterpret_cast<uintptr_t>(system);
	if (
		!MemoryUtils::IsReadableAddress(systemAddress + collectorsItemCountOffset, sizeof(int32_t))
		|| !MemoryUtils::IsReadableAddress(systemAddress + collectorsItemDataOffset, sizeof(uintptr_t))
	)
	{
		Logging::Write(logPrefix, "DSCollectors item system layout was not readable; music unlock gating will fail open");
		return;
	}

	const int32_t count = *reinterpret_cast<int32_t*>(systemAddress + collectorsItemCountOffset);
	const uintptr_t items = *reinterpret_cast<uintptr_t*>(systemAddress + collectorsItemDataOffset);
	if (count <= 0 || count > maxCollectorsItems || !MemoryUtils::IsReadableAddress(items, static_cast<size_t>(count) * sizeof(uintptr_t)))
	{
		Logging::Write(logPrefix, "DSCollectors item vector was not readable; music unlock gating will fail open");
		return;
	}

	for (int32_t i = 0; i < count; ++i)
	{
		const uintptr_t item = reinterpret_cast<uintptr_t*>(items)[i];
		if (!item || !MemoryUtils::IsReadableAddress(item + collectorsItemUnlockFactOffset, sizeof(uintptr_t)))
		{
			continue;
		}

		void* fact = *reinterpret_cast<void**>(item + collectorsItemUnlockFactOffset);
		if (!fact || !MemoryUtils::IsReadableAddress(reinterpret_cast<uintptr_t>(fact) + booleanFactUuidOffset, 16))
		{
			continue;
		}

		for (const auto& [name, unlockData] : ModConfiguration::Databases::musicUnlockFactDatabase)
		{
			if (unlockData.exclusiveDC && ModConfiguration::gameVersion != GameVersion::DC)
			{
				continue;
			}

			std::vector<uint8_t> bytes;
			std::vector<bool> masks;
			MemoryUtils::ParseHexString(unlockData.signature, bytes, masks);
			if (
				bytes.size() == 16
				&& std::memcmp(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fact) + booleanFactUuidOffset), bytes.data(), 16) == 0
			)
			{
				musicUnlockFacts[name] = fact;
				break;
			}
		}
	}

	musicUnlockFactsCached = true;
	Logging::Write(logPrefix,
		"Cached %zu music unlock facts from DSCollectors item system",
		musicUnlockFacts.size()
	);
}

bool MusicPlayer::IsTrackUnlocked(const MusicData* data)
{
	if (!ModConfiguration::skipLockedSongs || !data || data->type != MusicType::SONG || data->customWemPath || !data->name)
	{
		return true;
	}

	auto unlockDataIt = ModConfiguration::Databases::musicUnlockFactDatabase.find(data->name);
	if (
		unlockDataIt == ModConfiguration::Databases::musicUnlockFactDatabase.end()
		|| (unlockDataIt->second.exclusiveDC && ModConfiguration::gameVersion != GameVersion::DC)
	)
	{
		return true;
	}

	auto factIt = musicUnlockFacts.find(data->name);
	if (!musicUnlockFactsCached || factIt == musicUnlockFacts.end() || !factIt->second)
	{
		return true;
	}

	const FunctionData* getBooleanFactData = ModManager::GetFunctionData("GetBooleanFact");
	if (!getBooleanFactData || !getBooleanFactData->address)
	{
		return true;
	}

	using GetBooleanFact_t = bool (*)(void*, void*);
	return reinterpret_cast<GetBooleanFact_t>(getBooleanFactData->address)(factIt->second, factIt->second);
}

bool MusicPlayer::ClearMusicDescription()
{
	const FunctionData* functionData = ModManager::GetFunctionData("ClearMusicDescriptionByType");
	if (!musicDescriptionManager || !functionData || !functionData->address)
	{
		return false;
	}

	using ClearMusicDescriptionByType_t = void (*)(void*, uint32_t, bool);
	ClearMusicDescriptionByType_t clearMusicDescriptionByType =
		reinterpret_cast<ClearMusicDescriptionByType_t>(functionData->address);
	clearMusicDescriptionByType(musicDescriptionManager, 3, true);
	return true;
}

bool MusicPlayer::HasActiveMusicBlocker()
{
	return btTerritoryBlocksMusic.load()
		|| muleTerritoryBlocksMusic.load()
		|| facilityTerritoryBlocksMusic.load()
		|| chiralNetworkBlocksMusic.load();
}

void MusicPlayer::HandleMusicBlockerChange(
	bool wasBlocked,
	bool isBlocked,
	const char* pauseReason,
	const char* resumeReason,
	const char* interruptionReason
)
{
	if (!wasBlocked && isBlocked && (currentMusicData || pendingMusicData))
	{
		if (!PauseCurrentMusicForBlocker(pauseReason))
		{
			CancelPendingAreaMusicTransition(interruptionReason);
			if (AreaMusic::UsesOverride(currentMusicData))
			{
				if (ModManager* instance = ModManager::GetInstance())
				{
					instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
				}
			}

			if (musicAddressWatcher)
			{
				musicAddressWatcher->Uninstall();
				musicAddressWatcher.reset();
			}

			currentMusicData = nullptr;
			currentMusicIsPlaying.store(false);
			currentMusicPausedByBlocker.store(false);
			currentMusicPlayTime.store(0);
			currentMusicMaxLength.store(0);
			currentMusicSourceStartOffsetMs.store(0);

			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{ ModEventType::MusicPlayerStopped, this, nullptr });
			}
		}
	}
	else if (wasBlocked && !isBlocked)
	{
		ResumeCurrentMusicForBlocker(resumeReason);
	}
}

void MusicPlayer::InstallMusicAddressWatcher()
{
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	if (!currentMusicData)
	{
		return;
	}

	if (!playingLoopAddress)
	{
		Logging::Write(logPrefix, "PlayingLoop function signature was not found, cannot install music watcher");
		return;
	}

	uintptr_t addressToWatch = currentMusicData->address + musicAddressAccessOffset;
	musicAddressWatcher = std::make_unique<BreakpointWatcher>(
		[] {
			if (currentMusicPausedByBlocker.load())
			{
				return;
			}

			std::chrono::milliseconds songDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - currentMusicStartTime
			);
			currentMusicPlayTime.store(songDuration.count());
		},
		[] {
			constexpr const char* logPrefix = "Music Address Watcher";
			if (currentMusicPausedByBlocker.load())
			{
				Logging::Write(logPrefix, "Ignoring access timeout while area music is paused by a blocker");
				return;
			}

			std::chrono::milliseconds songDuration =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - currentMusicStartTime
				);
			Logging::Write(logPrefix, "Music stopped playing... Duration was %lld ms", songDuration.count());
			currentMusicIsPlaying.store(false);
		},
		watcherPollingInterval,
		timeTillAutoplay
	);
	musicAddressWatcher->Install(addressToWatch, playingLoopAddress, watchedInstructionLength);
}

bool MusicPlayer::PauseCurrentMusicForBlocker(const char* reason)
{
	if (pendingMusicData && !currentMusicData)
	{
		CancelPendingAreaMusicTransition(reason);
		return false;
	}

	if (!currentMusicData)
	{
		return false;
	}

	if (currentMusicPausedByBlocker.load())
	{
		return true;
	}

	if (currentMusicIsPlaying.load())
	{
		const std::chrono::milliseconds songDuration =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - currentMusicStartTime
			);
		currentMusicPlayTime.store(songDuration.count());
	}

	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	currentMusicPausedByBlocker.store(true);
	currentMusicIsPlaying.store(false);
	if (!PlaySilenceForAreaMusicTransition())
	{
		currentMusicPausedByBlocker.store(false);
		return false;
	}

	Logging::Write(logPrefix,
		"Paused area music \"%s\"%s%s",
		currentMusicData->name ? currentMusicData->name : "",
		reason && reason[0] ? ": " : "",
		reason && reason[0] ? reason : ""
	);
	return true;
}

bool MusicPlayer::ResumeCurrentMusicForBlocker(const char* reason)
{
	if (HasActiveMusicBlocker())
	{
		return false;
	}

	if (!currentMusicPausedByBlocker.load())
	{
		return false;
	}

	if (!currentMusicData)
	{
		currentMusicPausedByBlocker.store(false);
		return false;
	}

	return RestartCurrentMusicFromSavedPosition(reason);
}

bool MusicPlayer::RestartCurrentMusicFromSavedPosition(const char* reason)
{
	const MusicData* data = currentMusicData;
	if (!data)
	{
		return false;
	}

	const bool usesAreaOverride = AreaMusic::UsesOverride(data);
	const long long currentElapsedMs = (std::max)(0LL, currentMusicPlayTime.load());
	const long long savedResumeMs = usesAreaOverride
		? (std::max)(0LL, currentMusicSourceStartOffsetMs.load()) + currentElapsedMs
		: currentElapsedMs;
	const long long resumeMs = savedResumeMs > 0 ? savedResumeMs + resumeCompensationMs : 0;
	const AreaMusic::AreaMusicChain* nativeChain =
		(!usesAreaOverride && resumeMs > 0)
			? AreaMusic::LookupChainForSong(data->name)
			: nullptr;

	currentMusicData = nullptr;
	currentMusicPausedByBlocker.store(false);
	currentMusicIsPlaying.store(false);
	gameCalledInterruptor = false;
	gameCalledSong = false;
	PlayMusic(data, false, resumeMs);

	if (currentMusicData != data || !currentMusicIsPlaying.load())
	{
		if (nativeChain)
		{
			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{
					ModEventType::AreaMusicRestoreNativeOffsetRequested,
					nullptr,
					nullptr
				});
			}
		}
		currentMusicData = data;
		currentMusicPausedByBlocker.store(false);
		currentMusicIsPlaying.store(false);
		currentMusicSourceStartOffsetMs.store(0);
		Logging::Write(logPrefix,
			"Could not restart area music \"%s\" after blocker cleared",
			data->name ? data->name : ""
		);
		return false;
	}

	if (nativeChain && resumeMs > 0)
	{
		currentMusicPlayTime.store(resumeMs);
		currentMusicStartTime = std::chrono::steady_clock::now()
			- std::chrono::milliseconds(resumeMs);
	}

	if ((usesAreaOverride || nativeChain) && resumeMs > 0)
	{
		Logging::Write(logPrefix,
			"Resumed area music \"%s\" from %lld ms%s%s%s",
			data->name ? data->name : "",
			resumeMs,
			nativeChain ? " (native chain)" : "",
			reason && reason[0] ? ": " : "",
			reason && reason[0] ? reason : ""
		);
	}
	else
	{
		Logging::Write(logPrefix,
			"Restarted area music \"%s\" after blocker clear%s%s",
			data->name ? data->name : "",
			reason && reason[0] ? ": " : "",
			reason && reason[0] ? reason : ""
		);
	}
	return true;
}

bool MusicPlayer::PlaySilenceForAreaMusicTransition()
{
	auto silenceIt = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (silenceIt == ModConfiguration::Databases::interruptorDatabase.end() || !silenceIt->second.address)
	{
		Logging::Write(logPrefix, "Silence music data not found, cannot restart area music cleanly");
		return false;
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function was not hooked, cannot restart area music cleanly");
		return false;
	}

	void* arg1 = reinterpret_cast<void*>(playMusicFuncRCXAddress);
	void* arg2 = reinterpret_cast<void*>(playMusicFuncRDXAddress);
	if (!arg1 || !arg2)
	{
		Logging::Write(logPrefix, "PlayMusic arguments are not initialized, cannot restart area music cleanly");
		return false;
	}

	void* arg3 = reinterpret_cast<void*>(silenceIt->second.address);
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, 0);
	Logging::Write(logPrefix, "Played Silence before restarting area music template");
	return true;
}

bool MusicPlayer::QueueAreaMusicTransition(const MusicData* data, bool displayDescription)
{
	if (!data || !AreaMusic::IsTemplateTrack(data))
	{
		return false;
	}

	currentMusicPausedByBlocker.store(false);

	if (!PlaySilenceForAreaMusicTransition())
	{
		return false;
	}

	if (ModManager* instance = ModManager::GetInstance())
	{
		instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
	}

	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(0);
	currentMusicSourceStartOffsetMs.store(0);
	currentMusicPausedByBlocker.store(false);
	pendingMusicData = data;
	pendingMusicDisplayDescription = displayDescription;
	pendingMusicOverridePrepared = false;
	pendingMusicStartTime = std::chrono::steady_clock::now();
	return true;
}

bool MusicPlayer::ShowMusicDescription(const MusicData* data)
{
	if (!data || data->type != MusicType::SONG)
	{
		return false;
	}

	if (descriptionDisplayed || pendingMusicDescriptionData)
	{
		descriptionDisplayed = false;
		ClearMusicDescription();
		pendingMusicDescriptionData = data;
		Logging::Write(logPrefix, "Deferring song description until next frame: %s", data->name ? data->name : "");
		return true;
	}

	return ShowMusicDescriptionNow(data);
}

bool MusicPlayer::ShowMusicDescriptionNow(const MusicData* data)
{
	if (!data || data->type != MusicType::SONG)
	{
		return false;
	}

	const FunctionData* showMusicDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
	if (!showMusicDescriptionFuncData || !showMusicDescriptionFuncData->address)
	{
		Logging::Write(logPrefix, "Show music description function not found, cannot show description");
		return false;
	}

	GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
		showMusicDescriptionFuncData->address
	);

	if (data->descriptionID)
	{
		void* descriptionId = reinterpret_cast<void*>(data->descriptionID);
		Logging::Write(logPrefix, "Showing song description for ID: %p", descriptionId);
		showMusicDescriptionFunc(nullptr, descriptionId, nullptr, nullptr);

		lastDisplayTime = std::chrono::steady_clock::now();
		descriptionDisplayed = true;
		return true;
	}

	if (!data->name || !data->name[0])
	{
		return false;
	}

	const FunctionData* showMusicDescriptionFromTextFuncData =
		ModManager::GetFunctionData("ShowMusicDescriptionFromText");
	if (!showMusicDescriptionFromTextFuncData || !showMusicDescriptionFromTextFuncData->address)
	{
		Logging::Write(logPrefix, "Show music description from text function not found, cannot show custom description");
		return false;
	}

	std::wstring titleText = Utils::Utf8ToWstring("\"" + std::string(data->name) + "\"");
	if (titleText.empty())
	{
		Logging::Write(logPrefix, "Could not convert song title to UTF-16, cannot show custom description");
		return false;
	}

	std::wstring artistText;
	const wchar_t* artistTextList[1] = {};
	int32_t artistTextCount = 0;
	if (data->artist && data->artist[0])
	{
		artistText = Utils::Utf8ToWstring(data->artist);
		if (!artistText.empty())
		{
			artistTextList[0] = artistText.c_str();
			artistTextCount = 1;
		}
	}

	using ShowMusicDescriptionFromText_t = void (*)(
		uint32_t,
		const wchar_t*,
		const wchar_t**,
		int32_t,
		const wchar_t**,
		int32_t,
		float
	);

	ShowMusicDescriptionFromText_t showMusicDescriptionFromTextFunc =
		reinterpret_cast<ShowMusicDescriptionFromText_t>(showMusicDescriptionFromTextFuncData->address);
	showMusicDescriptionFromTextFunc(
		0,
		titleText.c_str(),
		artistTextCount > 0 ? artistTextList : nullptr,
		artistTextCount,
		nullptr,
		0,
		0.0f
	);

	Logging::Write(logPrefix,
		"Showing custom song description: \"%s\" - \"%s\"",
		data->name,
		data->artist ? data->artist : ""
	);
	lastDisplayTime = std::chrono::steady_clock::now();
	descriptionDisplayed = true;
	return true;
}

void MusicPlayer::PlayMusic(const MusicData* data, bool displayDescription, long long resumeOffsetMs)
{
	if (!data || !data->address)
	{
		Logging::Write(logPrefix, "Invalid music data provided, cannot play music");
		return;
	}

	if (!IsTrackUnlocked(data))
	{
		Logging::Write(logPrefix, "Song \"%s\" is still locked in-game, skipping", data->name ? data->name : "");
		return;
	}

	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	if (ModManager* instance = ModManager::GetInstance())
	{
		instance->DispatchEvent(ModEvent{
			ModEventType::AreaMusicRestoreNativeOffsetRequested,
			nullptr,
			nullptr
		});
	}

	const bool usesAreaOverride = AreaMusic::UsesOverride(data);
	const AreaMusic::AreaMusicChain* nativeChain =
		(data->name && !usesAreaOverride && resumeOffsetMs > 0)
			? AreaMusic::LookupChainForSong(data->name)
			: nullptr;

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function was not hooked, cannot play music");
		return;
	}

	// Always reset flags to be safe
	gameCalledSong = false;
	gameCalledInterruptor = false;

	if (
		currentMusicData
		&& AreaMusic::IsTemplateTrack(currentMusicData)
		&& AreaMusic::IsTemplateTrack(data)
	)
	{
		Logging::Write(logPrefix, "Queueing area music restart for \"%s\"", data->name);
		if (QueueAreaMusicTransition(data, displayDescription))
		{
			return;
		}
		Logging::Write(logPrefix, "Area music restart transition failed; trying immediate playback");
	}

	if (
		ModConfiguration::showSongDescription && displayDescription
	)
	{
		ShowMusicDescription(data);
	}

	void* arg1 = reinterpret_cast<void*>(playMusicFuncRCXAddress);
	if (!arg1)
	{
		Logging::Write(logPrefix, "Failed to find play music first arg");
		return;
	}
	Logging::Write(logPrefix, "First argument read successfully: %p", arg1);

	void* arg2 = reinterpret_cast<void*>(playMusicFuncRDXAddress);
	if (!arg2)
	{
		Logging::Write(logPrefix, "Failed to find play music second arg");
		return;
	}
	Logging::Write(logPrefix, "Second argument read successfully: %p", arg2);

	void* arg3 = reinterpret_cast<void*>(data->address);
	Logging::Write(logPrefix, "Third argument read successfully: %p", arg3);

	long long playbackMaxLength = data->maxLength;
	long long playbackSourceStartMs = 0;
	if (usesAreaOverride)
	{
		AreaMusic::RegisterRequest registration{};
		registration.data = data;
		registration.sourceStartMs = resumeOffsetMs;

		if (ModManager* instance = ModManager::GetInstance())
		{
			instance->DispatchEvent(ModEvent{
				ModEventType::AreaMusicRegisterRequested,
				nullptr,
				&registration
			});
		}

		if (!registration.handled || !registration.success)
		{
			Logging::Write(logPrefix,
				"Area music override was not registered for \"%s\"; canceling playback",
				data->name ? data->name : ""
			);
			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
			}
			PlaySilenceForAreaMusicTransition();
			currentMusicData = nullptr;
			currentMusicIsPlaying.store(false);
			currentMusicPlayTime.store(0);
			currentMusicMaxLength.store(0);
			currentMusicSourceStartOffsetMs.store(0);
			currentMusicPausedByBlocker.store(false);
			return;
		}
		playbackMaxLength = registration.metadataPatched
			? 0
			: (registration.effectiveDurationMs > 0 ? registration.effectiveDurationMs : data->maxLength);
		playbackSourceStartMs = registration.effectiveSourceStartMs;
		Logging::Write(logPrefix,
			registration.metadataPatched
			? "Using patched Wwise media metadata for area override track \"%s\"; runtime duration guard is %lld ms"
			: "Wwise media metadata was not patched for area override track \"%s\"; runtime duration guard is %lld ms",
			data->name ? data->name : "",
			playbackMaxLength
		);
	}
	else
	{
		if (ModManager* instance = ModManager::GetInstance())
		{
			instance->DispatchEvent(ModEvent{ ModEventType::AreaMusicUnsetRequested, nullptr, nullptr });
		}
	}

	if (nativeChain)
	{
		AreaMusic::PatchNativeOffsetRequest request{};
		request.chain = nativeChain;
		request.sourceStartMs = resumeOffsetMs;

		if (ModManager* instance = ModManager::GetInstance())
		{
			instance->DispatchEvent(ModEvent{
				ModEventType::AreaMusicPatchNativeOffsetRequested,
				nullptr,
				&request
			});
		}
	}

	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(playbackMaxLength);
	currentMusicSourceStartOffsetMs.store(playbackSourceStartMs);
	currentMusicPausedByBlocker.store(false);
	currentMusicStartTime = std::chrono::steady_clock::now();
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, 0);
	Logging::Write(logPrefix,
		"Playing BGM: \"%s\" (runtime duration guard %lld ms)",
		data->name,
		playbackMaxLength
	);

	if (data->type == MusicType::SONG || data->type == MusicType::AMBIENT)
	{
		currentMusicData = data;
		currentMusicIsPlaying.store(true);
		InstallMusicAddressWatcher();
	}
}

void MusicPlayer::PlayCurrentSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No current song available in the queue");
		return;
	}

	// if no song is currently playing, display description if mod setting allows it
	// don't display description again if looping same song
	bool displayDescription = songQueue.GetCurrentIndex() < 0;

	const MusicData* currentSong = songQueue.GetCurrent();
	for (size_t i = 0; i < songQueue.Size() && currentSong && !IsTrackUnlocked(currentSong); ++i)
	{
		Logging::Write(logPrefix, "Skipping locked song: %s", currentSong->name ? currentSong->name : "");
		currentSong = songQueue.GetNext();
	}

	if (!currentSong || !IsTrackUnlocked(currentSong))
	{
		Logging::Write(logPrefix, "No unlocked current song available in the queue");
		return;
	}

	PlayMusic(currentSong, displayDescription);
}

void MusicPlayer::PlayNextSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No next songs available in the queue");
		return;
	}

	const MusicData* nextSong = nullptr;
	for (size_t i = 0; i < songQueue.Size(); ++i)
	{
		nextSong = songQueue.GetNext();
		if (nextSong && IsTrackUnlocked(nextSong))
		{
			break;
		}
		if (nextSong)
		{
			Logging::Write(logPrefix, "Skipping locked song: %s", nextSong->name ? nextSong->name : "");
		}
	}

	if (!nextSong || !IsTrackUnlocked(nextSong))
	{
		Logging::Write(logPrefix, "No unlocked next songs available in the queue");
		return;
	}
	Logging::Write(logPrefix, "Attempting to play next song: %s", nextSong->name);
	PlayMusic(nextSong);
}

void MusicPlayer::PlayPreviousSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No previous song available in the queue");
		return;
	}

	const MusicData* previousSong = nullptr;
	for (size_t i = 0; i < songQueue.Size(); ++i)
	{
		previousSong = songQueue.GetPrevious();
		if (previousSong && IsTrackUnlocked(previousSong))
		{
			break;
		}
		if (previousSong)
		{
			Logging::Write(logPrefix, "Skipping locked song: %s", previousSong->name ? previousSong->name : "");
		}
	}

	if (!previousSong || !IsTrackUnlocked(previousSong))
	{
		Logging::Write(logPrefix, "No unlocked previous song available in the queue");
		return;
	}
	Logging::Write(logPrefix, "Attempting to play previous song: %s", previousSong->name);
	PlayMusic(previousSong);
}

void MusicPlayer::PlayByName(const std::string& name)
{
	if (name.empty())
	{
		Logging::Write(logPrefix, "Cannot find music with an empty name");
		return;
	}

	auto interruptorIt = ModConfiguration::Databases::interruptorDatabase.find(name);
	if (interruptorIt != ModConfiguration::Databases::interruptorDatabase.end())
	{
		const MusicData* interruptorMusicData = &(interruptorIt->second);
		PlayMusic(interruptorMusicData);
		return;
	}

	auto songIt = ModConfiguration::Databases::songDatabase.find(name);
	if (songIt != ModConfiguration::Databases::songDatabase.end())
	{
		const MusicData* songMusicData = &(songIt->second);
		PlayMusic(songMusicData);
		return;
	}

	auto customSongIt = ModConfiguration::Databases::customSongDatabase.find(name);
	if (customSongIt != ModConfiguration::Databases::customSongDatabase.end())
	{
		const MusicData* customSongMusicData = &(customSongIt->second);
		PlayMusic(customSongMusicData);
		return;
	}
}

void MusicPlayer::StopMusic()
{
	currentMusicPausedByBlocker.store(false);
	pendingMusicDescriptionData = nullptr;

	CancelPendingAreaMusicTransition("stop requested");

	if (!currentMusicData)
	{
		return;
	}

	auto it = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (it == ModConfiguration::Databases::interruptorDatabase.end())
	{
		Logging::Write(logPrefix, "Silence music data not found, cannot stop music");
		return;
	}

	const MusicData* silenceData = &(it->second);
	PlayMusic(silenceData, false); // Play silence to stop current music
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicPausedByBlocker.store(false);
	currentMusicMaxLength.store(0);
	currentMusicSourceStartOffsetMs.store(0);
}

void MusicPlayer::PlayNextInPool()
{
	if (poolQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No next music available in the pool");
		return;
	}

	const MusicData* nextPoolMusic = poolQueue.GetNext();
	if (!nextPoolMusic)
	{
		Logging::Write(logPrefix, "No next music available in the pool");
		return;
	}

	Logging::Write(logPrefix, "Attempting to play next music from pool: %s", nextPoolMusic->name);
	MemoryUtils::PrintBytesAtAddress(nextPoolMusic->address, 24);

	PlayMusic(nextPoolMusic);
}

void MusicPlayer::PlayPreviousInPool()
{
	if (poolQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No previous music available in the pool");
		return;
	}

	const MusicData* previousPoolMusic = poolQueue.GetPrevious();
	if (!previousPoolMusic)
	{
		Logging::Write(logPrefix, "No previous music available in the pool");
		return;
	}

	Logging::Write(logPrefix, "Attempting to play previous music from pool: %s", previousPoolMusic->name);
	MemoryUtils::PrintBytesAtAddress(previousPoolMusic->address, 24);

	PlayMusic(previousPoolMusic);
}

void MusicPlayer::OnInputPress(const InputCode& inputCode)
{
	if (inputCode.source == InputSource::KBM)
	{
		if (ModConfiguration::devMode)
		{
			if (inputCode.code == VK_F1)
			{
				PlayNextInPool();
			}
			if (inputCode.code == VK_F2)
			{
				PlayPreviousInPool();
			}
			if (inputCode.code == VK_F3)
			{
				static uint16_t currentDescriptionId = 0;

				const FunctionData* showSongDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
				if (!showSongDescriptionFuncData || !showSongDescriptionFuncData->address)
				{
					Logging::Write(logPrefix, "Show music description function not found, cannot show description");
				}
				else
				{
					void* descriptionId = reinterpret_cast<void*>(currentDescriptionId++);
					Logging::Write(logPrefix, "Showing song description for ID: %d", descriptionId);
					GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
						showSongDescriptionFuncData->address
					);
					showMusicDescriptionFunc(nullptr, descriptionId, nullptr, nullptr);
				}
			}
		}

		if (inputCode.code == VK_F9)
		{
			StopMusic();
			if (ModManager* instance = ModManager::GetInstance())
			{
				instance->DispatchEvent(ModEvent{
					ModEventType::MusicPlayerStopped,
					this,
					nullptr
				});
			}
		}
	}
}
