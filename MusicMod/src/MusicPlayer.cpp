#include "MusicPlayer.h"

#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <Windows.h>

#include "GameStateManager.h"
#include "ModConfiguration.h"
#include "ModManager.h"
#include "PlaybackQueue.h"

#include "MemoryUtils.h"
#include "MinHook.h"

#include "PatternScanner.h"

#include "UIButton.h"

#include "Utils.h"

MusicPlayer::MusicPlayer()
{
}

void MusicPlayer::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
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
		if (territoryFlagState->current != EnemyTerritoryFlag::SAFE && currentMusicData)
		{
			StopMusic();

			ModManager* instance = ModManager::GetInstance();
			if (instance)
			{
				instance->DispatchEvent(
					ModEvent{ ModEventType::MusicPlayerStopped, this, nullptr }
				);
			}
		}
		break;
	}
	case ModEventType::ChiralNetworkStateChanged:
	{
		if (ModConfiguration::connectToChiralNetwork)
		{
			auto* chiralNetworkFlagState = std::any_cast<FlagState<ChiralNetworkFlag>*>(event.data);
			if (chiralNetworkFlagState->current == ChiralNetworkFlag::OFF && currentMusicData)
			{
				StopMusic();

				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					instance->DispatchEvent(
						ModEvent{ ModEventType::MusicPlayerStopped, this, nullptr }
					);
				}
			}
		}
		break;
	}
	default:
		break;
	}
}

void MusicPlayer::OnScanDone()
{
	bool hookResult = ModManager::TryHookFunction(
		"PlayMusic",
		reinterpret_cast<void*>(&MusicPlayer::PlayMusicHook)
	);
	logger.Log(
		"PlayMusic function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	const FunctionData* playingLoopData = ModManager::GetFunctionData("PlayingLoop");
	if (!playingLoopData || !playingLoopData->address)
	{
		logger.Log("Failed to find PlayingLoop function signature");
	}
	else
	{
		playingLoopAddress = playingLoopData->address;
	}

	hookResult = ModManager::TryHookFunction(
		"PlayUISound",
		reinterpret_cast<void*>(&MusicPlayer::PlayUISoundHook)
	);
	logger.Log(
		"PlayUISound function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"ShowMusicDescriptionCore",
		reinterpret_cast<void*>(&MusicPlayer::ShowMusicDescriptionCoreHook)
	);
	logger.Log(
		"ShowMusicDescriptionCore function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	std::vector<const MusicData*> songMusicDataList;
	for (const std::string& name : ModConfiguration::activePlaylist)
	{
		auto it = ModConfiguration::Databases::songDatabase.find(name);
		if (it == ModConfiguration::Databases::songDatabase.end())
		{
			logger.Log("Song \"%s\" not found in database, skipping...", name.c_str());
			continue;
		}

		auto& songMusicData = it->second;
		if (!songMusicData.address)
		{
			logger.Log("Song \"%s\" has no valid address, skipping...", name.c_str());
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
				logger.Log("Pushed pool music %s with address %p",
					poolMusicData.name, poolMusicData.address
				);

				uintptr_t currentAddress = poolMusicData.address;
				// Take the first 8 bytes and compare with each new address start to check if we're still within the pool
				uint64_t poolMusicSignature = *reinterpret_cast<uint64_t*>(poolMusicData.address);
				size_t index = 0;

				std::vector<size_t> skipIndices{};

				while (true)
				{
					currentAddress = currentAddress + poolMusicDataSize;
					uint64_t signature = *reinterpret_cast<uint64_t*>(currentAddress);
					if (signature != poolMusicSignature)
					{
						logger.Log("Pool music signature mismatch at address %p, pool ended", (void*)currentAddress);
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
						logger.Log("Failed to allocate memory for new pool music data");
						continue;
					}
					newData->descriptionID = 0;
					newData->type = MusicType::UNKNOWN;
					newData->maxLength = 0;
					newData->name = _strdup(poolMusicName.c_str());
					newData->artist = "";
					newData->signature = "";
					newData->address = currentAddress;
					newData->active = true;

					poolMusicDataList.push_back(newData);

					logger.Log("Pushed pool music %s with address %p",
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
	// Handle Playback
	if (currentMusicData)
	{
		if (currentMusicData->maxLength > 0 && currentMusicData->maxLength < currentMusicPlayTime.load())
		{
			logger.Log("Current music playback time exceeded its max duration");
			currentMusicPlayTime.store(0);
			currentMusicIsPlaying.store(false);
		}

		if (!currentMusicIsPlaying.load())
		{
			if (gameCalledInterruptor || (gameCalledSong && ModConfiguration::allowScriptedSongs))
			{
				gameCalledSong = false;
				gameCalledInterruptor = false;

				currentMusicData = nullptr;
				ModManager* instance = ModManager::GetInstance();
				if (instance)
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
						logger.Log("Autoplaying next song...");
						PlayNextSong();
						break;
					}
					case LoopMode::ONE:
					{
						logger.Log("Autoplaying current song...");
						PlayCurrentSong();
						break;
					}
					case LoopMode::NONE:
					{
						if (musicAddressWatcher)
						{
							musicAddressWatcher->Uninstall();
							musicAddressWatcher.reset();
						}

						ModManager* instance = ModManager::GetInstance();
						if (instance)
						{
							instance->DispatchEvent(ModEvent{
								ModEventType::MusicPlayerStopped,
								this,
								nullptr
								});
						}
						break;
					}
					default:
						break;
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
	if (descriptionDisplayed)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - lastDisplayTime >= std::chrono::milliseconds(displayDuration))
		{
			logger.Log("Hiding song description after %d ms", displayDuration);
			const FunctionData* functionData = ModManager::GetFunctionData("ShowMusicDescription");
			if (!functionData || !functionData->address)
			{
				logger.Log("Show description function not found, cannot hide description");
			}
			else
			{
				descriptionDisplayed = false;
				GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
					functionData->address
				);
				showMusicDescriptionFunc(nullptr, nullptr, nullptr, nullptr);
			}
		}
	}
}

void MusicPlayer::OnPreExit()
{
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
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
				logger.Log("Toggling music off...");
				StopMusic();
			}
			else
			{
				logger.Log("Toggling music on...");
				if (loopMode == LoopMode::ONE)
				{
					logger.Log("Loop mode is ONE, playing current song...");
					PlayCurrentSong(); 
				}
				else
				{
					logger.Log("Loop mode is ALL, playing next song...");
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
				ModManager* instance = ModManager::GetInstance();
				if (instance)
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
				ModManager* instance = ModManager::GetInstance();
				if (instance)
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
			logger.Log("Loop mode changed to: %d", static_cast<int>(loopMode));
			break;
		}
		default:
			break;
	}
}

void MusicPlayer::PlayMusicHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Play Music Hook");
	if (!playMusicFuncRCXAddress)
	{
		playMusicFuncRCXAddress = reinterpret_cast<uintptr_t>(arg1);
		logger.Log("1st arg address set to: %p", (void*)playMusicFuncRCXAddress);
	}

	if (!playMusicFuncRDXAddress)
	{
		uintptr_t arg2Address = reinterpret_cast<uintptr_t>(arg2);
		if ((arg2Address & 0xFF) == 0x80)
		{
			playMusicFuncRDXAddress = arg2Address;
			logger.Log("2nd arg address set to: %p", (void*)playMusicFuncRDXAddress);
		}
	}

	gameCalledInterruptor = false;
	for (const auto& [name, interruptorData] : ModConfiguration::Databases::interruptorDatabase)
	{
		if (name != "Silence" && reinterpret_cast<uintptr_t>(arg3) == interruptorData.address)
		{
			gameCalledInterruptor = true;
			break;
		}
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		logger.Log("Original play music function is not found, cannot play music");
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
				continue; // Skip pool music start, it's in the database for dev purposes
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
		logger.Log(
			"Configuration does not allow scripted songs, skipping song %s",
			songToPlay->name
		);
		return;
	}

	/*if (!gameCalledSong && !gameCalledInterruptor)
	{
		logger.Log("Game called PlayMusic function with arg3: %p", arg3);
	}*/
	
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::PlayUISoundHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Play UI Sound");

	if (currentMusicData && arg1 && arg2)
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
						//logger.Log("Interruptor %s not matched at offset %zu", interruptor.name, i);
						match = false;
						break; // not matched, skip to next interruptor
					}
				}
			}

			if (match)
			{
				logger.Log("Interruptor %s matched, stopping autoplay", interruptor.name);
				currentMusicData = nullptr;
				currentMusicIsPlaying.store(false);

				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					std::string interruptionReasonStr = std::string(interruptor.name) + ": music player interrupted.";
					const char* interruptionReason = interruptionReasonStr.c_str();
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerInterrupted,
						nullptr,
						&interruptionReason
					});
				}

				break;
			}
		}
	}

	const FunctionData* playUISoundFuncData = ModManager::GetFunctionData("PlayUISound");
	if (!playUISoundFuncData || !playUISoundFuncData->originalFunction)
	{
		logger.Log("Original play UI sound function is not found, cannot play sound.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(playUISoundFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::ShowMusicDescriptionCoreHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Show Music Description Core Hook");

	const FunctionData* showMusicDescriptionCoreFuncData = ModManager::GetFunctionData("ShowMusicDescriptionCore");
	if (!showMusicDescriptionCoreFuncData || !showMusicDescriptionCoreFuncData->originalFunction)
	{
		logger.Log("Original show music description core function was not hooked, cannot show description");
		return;
	}

	if (gameCalledSong && !ModConfiguration::allowScriptedSongs)
	{
		logger.Log("Configuration does not allow scripted songs, skipping song description");
		return;
	}
	
	reinterpret_cast<GenericFunction_t>(showMusicDescriptionCoreFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::PlayMusic(const MusicData* data, bool displayDescription=true)
{
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	if (!data || !data->address)
	{
		logger.Log("Invalid music data provided, cannot play music");
		return;
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		logger.Log("Original play music function was not hooked, cannot play music");
		return;
	}

	// Always reset flags to be safe
	gameCalledSong = false;
	gameCalledInterruptor = false;

	if (
		ModConfiguration::showSongDescription && displayDescription
		&& data->type == MusicType::SONG && data->descriptionID
	)
	{
		const FunctionData* showMusicDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
		if (!showMusicDescriptionFuncData || !showMusicDescriptionFuncData->address)
		{
			logger.Log("Show music description function not found, cannot show description");
		}
		else
		{
			//if (descriptionDisplayed)
			//{
			//	descriptionDisplayed = false;
			//	drawDescriptionFunc(nullptr, nullptr, nullptr, nullptr); // clear
			//}

			void* descriptionId = reinterpret_cast<void*>(data->descriptionID);
			logger.Log("Showing song description for ID: %p", descriptionId);
			GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
				showMusicDescriptionFuncData->address
			);
			showMusicDescriptionFunc(nullptr, descriptionId, nullptr, nullptr);

			lastDisplayTime = std::chrono::steady_clock::now();
			descriptionDisplayed = true;
		}
	}

	void* arg1 = reinterpret_cast<void*>(playMusicFuncRCXAddress);
	if (!arg1)
	{
		logger.Log("Failed to find play music first arg");
		return;
	}
	logger.Log("First argument read successfully: %p", arg1);

	void* arg2 = reinterpret_cast<void*>(playMusicFuncRDXAddress);
	if (!arg2)
	{
		logger.Log("Failed to find play music second arg");
		return;
	}
	logger.Log("Second argument read successfully: %p", arg2);

	void* arg3 = reinterpret_cast<void*>(data->address);
	logger.Log("Third argument read successfully: %p", arg3);

	currentMusicPlayTime.store(0);
	currentMusicStartTime = std::chrono::steady_clock::now();
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, 0);
	logger.Log("Playing BGM: \"%s\"", data->name);

	if (data->type == MusicType::SONG || data->type == MusicType::AMBIENT)
	{
		currentMusicData = data;
		currentMusicIsPlaying.store(true);
		uintptr_t addressToWatch = currentMusicData->address + musicAddressAccessOffset;

		musicAddressWatcher = std::make_unique<BreakpointWatcher>(
			[] {
				/*Logger logger("Music Address Watcher");
				logger.Log("Music still playing...");*/
				std::chrono::milliseconds songDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - currentMusicStartTime
				);
				currentMusicPlayTime.store(songDuration.count());
			},
			[] {
				Logger logger("Music Address Watcher");
				std::chrono::milliseconds songDuration =
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - currentMusicStartTime
					);
				logger.Log("Music stopped playing... Duration was %lld ms", songDuration.count());
				currentMusicIsPlaying.store(false);
			},
			watcherPollingInterval,
			timeTillAutoplay
		);
		musicAddressWatcher->Install(addressToWatch, playingLoopAddress, watchedInstructionLength);
	}
}

void MusicPlayer::PlayCurrentSong()
{
	// if no song is currently playing, display description if mod setting allows it
	// don't display description again if looping same song
	bool displayDescription = songQueue.GetCurrentIndex() < 0;

	const MusicData* currentSong = songQueue.GetCurrent();
	if (!currentSong)
	{
		logger.Log("No current song available in the queue");
		return;
	}

	PlayMusic(currentSong, displayDescription);
}

void MusicPlayer::PlayNextSong()
{
	const MusicData* nextSong = songQueue.GetNext();
	if (!nextSong)
	{
		logger.Log("No next songs available in the queue");
		return;
	}
	logger.Log("Attempting to play next song: %s", nextSong->name);
	PlayMusic(nextSong);
}

void MusicPlayer::PlayPreviousSong()
{
	const MusicData* previousSong = songQueue.GetPrevious();
	if (!previousSong)
	{
		logger.Log("No previous song available in the queue");
		return;
	}
	logger.Log("Attempting to play previous song: %s", previousSong->name);
	PlayMusic(previousSong);
}

void MusicPlayer::PlayByName(const std::string& name)
{
	if (name.empty())
	{
		logger.Log("Cannot find music with an empty name");
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
}

void MusicPlayer::StopMusic()
{
	if (!currentMusicData)
	{
		return;
	}

	auto it = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (it == ModConfiguration::Databases::interruptorDatabase.end())
	{
		logger.Log("Silence music data not found, cannot stop music");
		return;
	}

	const MusicData* silenceData = &(it->second);
	PlayMusic(silenceData, false); // Play silence to stop current music
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
}

void MusicPlayer::PlayNextInPool()
{
	const MusicData* nextPoolMusic = poolQueue.GetNext();
	if (!nextPoolMusic)
	{
		logger.Log("No next music available in the pool");
		return;
	}

	logger.Log("Attempting to play next music from pool: %s", nextPoolMusic->name);
	MemoryUtils::PrintBytesAtAddress(nextPoolMusic->address, 24);

	PlayMusic(nextPoolMusic);
}

void MusicPlayer::PlayPreviousInPool()
{
	const MusicData* previousPoolMusic = poolQueue.GetPrevious();
	if (!previousPoolMusic)
	{
		logger.Log("No previous music available in the pool");
		return;
	}
	
	logger.Log("Attempting to play previous music from pool: %s", previousPoolMusic->name);
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
					logger.Log("Show music description function not found, cannot show description");
				}
				else
				{
					void* descriptionId = reinterpret_cast<void*>(currentDescriptionId++);
					logger.Log("Showing song description for ID: %d", descriptionId);
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
			ModManager* instance = ModManager::GetInstance();
			if (instance)
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