#include "AreaMusicManager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <Windows.h>

#include "AudioDecoder.h"
#include "DecimaArchiveReader.h"
#include "GameData.h"
#include "Logger.h"
#include "MemoryUtils.h"
#include "ModConfiguration.h"
#include "ModManager.h"

#include "Utils.h"

namespace
{
	constexpr const char* wwiseObjectLookupFunctionName = "WwiseObjectLookup";
	constexpr size_t wwiseObjectReleaseVtableOffset = 0x10;
	constexpr size_t liveSegmentMarkerStride = 0x10;
	constexpr size_t liveSegmentMarkerPositionOffset = 0x04;
	constexpr size_t liveTrackSourceStride = 0x10;
	constexpr size_t liveTrackSourceObjectOffset = 0x08;
	constexpr size_t liveTrackSourceObjectSize = 0x28;
	constexpr size_t liveTrackTimingStride = 0x1c;
	constexpr uint32_t wwiseTicksPerMillisecond = 48;
	constexpr uint32_t maxInternalWwiseMediaSize = DecimaArchiveReader::maxExtractedFileSize;

	uint32_t MillisecondsToWwiseTicks(long long milliseconds)
	{
		if (milliseconds <= 0)
		{
			return 1;
		}

		const unsigned long long ticks =
			static_cast<unsigned long long>(milliseconds) * wwiseTicksPerMillisecond;
		if (ticks > (std::numeric_limits<uint32_t>::max)())
		{
			return (std::numeric_limits<uint32_t>::max)();
		}

		return static_cast<uint32_t>(ticks);
	}

	long long ClampSourceStartOffsetMs(long long sourceStartMs, long long sourceDurationMs)
	{
		if (sourceStartMs <= 0 || sourceDurationMs <= 1)
		{
			return 0;
		}

		return (std::min)(sourceStartMs, sourceDurationMs - 1);
	}

	bool CallWwiseReleaseNoUnwind(void* object, uintptr_t releaseAddress, uint32_t* exceptionCode)
	{
		using ReleaseFn = void(__fastcall*)(void*);
		__try
		{
			reinterpret_cast<ReleaseFn>(releaseAddress)(object);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (exceptionCode)
			{
				*exceptionCode = GetExceptionCode();
			}
			return false;
		}
	}

	bool LookupWwiseObjectNoUnwind(
		uintptr_t lookupAddress,
		void* registry,
		uint32_t objectId,
		int tableIndex,
		void** object,
		uint32_t* exceptionCode
	)
	{
		using LookupFn = void* (__cdecl*)(void*, uint32_t, int);
		__try
		{
			if (object)
			{
				*object = reinterpret_cast<LookupFn>(lookupAddress)(registry, objectId, tableIndex);
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (exceptionCode)
			{
				*exceptionCode = GetExceptionCode();
			}
			if (object)
			{
				*object = nullptr;
			}
			return false;
		}
	}

	bool LooksLikeWwiseMediaBuffer(const uint8_t* bytes, size_t size)
	{
		if (!bytes || size < 12)
		{
			return false;
		}
		const bool riff =
			std::memcmp(bytes, "RIFF", 4) == 0
			|| std::memcmp(bytes, "RIFX", 4) == 0;
		return riff && std::memcmp(bytes + 8, "WAVE", 4) == 0;
	}
}

void AreaMusicManager::ReleaseWwiseObject(void* object)
{
	if (!object)
	{
		return;
	}

	const uintptr_t objectAddress = reinterpret_cast<uintptr_t>(object);
	if (!MemoryUtils::IsReadableAddress(objectAddress, sizeof(uintptr_t)))
	{
		return;
	}

	uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
	if (!MemoryUtils::IsReadableAddress(vtable + wwiseObjectReleaseVtableOffset, sizeof(uintptr_t)))
	{
		return;
	}

	using ReleaseFn = void(__fastcall*)(void*);
	auto release = reinterpret_cast<ReleaseFn>(
		*reinterpret_cast<uintptr_t*>(vtable + wwiseObjectReleaseVtableOffset)
	);
	if (
		release
		&& MemoryUtils::IsExecutableAddress(reinterpret_cast<uintptr_t>(release), 16)
	)
	{
		uint32_t exceptionCode = 0;
		if (!CallWwiseReleaseNoUnwind(object, reinterpret_cast<uintptr_t>(release), &exceptionCode))
		{
			Logging::Write(logPrefix,
				"Wwise object release raised exception 0x%08x for object %p",
				exceptionCode,
				object
			);
		}
	}
}

const AreaMusicManager::LiveWwiseOffsets& AreaMusicManager::GetLiveWwiseOffsets()
{
	static constexpr LiveWwiseOffsets dc{
		0x110,
		0x118,
		0x120,
		0xd8,
		0xe0,
		0x100,
		0x108,
		0x110
	};

	static constexpr LiveWwiseOffsets standard{
		0x100,
		0x108,
		0x110,
		0xc8,
		0xd0,
		0xf0,
		0xf8,
		0x100
	};

	return ModConfiguration::gameVersion == GameVersion::STANDARD
		? standard
		: dc;
}

void AreaMusicManager::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
		case ModEventType::FrameRendered:
		{
			InstallWwiseObjectLookupHook();
			ResolveWwiseMediaFunctions();
			break;
		}
		case ModEventType::PreExitTriggered:
		{
			RestoreNativeAreaMusicOffset();
			Unset();
			RetireBuffer();
			break;
		}
		case ModEventType::AreaMusicRegisterRequested:
		{
			auto* request = std::any_cast<AreaMusic::RegisterRequest*>(event.data);
			if (request)
			{
				HandleRegisterRequest(*request);
			}
			break;
		}
		case ModEventType::AreaMusicUnsetRequested:
		{
			Unset();
			break;
		}
		case ModEventType::AreaMusicPatchNativeOffsetRequested:
		{
			auto* request = std::any_cast<AreaMusic::PatchNativeOffsetRequest*>(event.data);
			if (request)
			{
				request->handled = true;
				request->success = PatchNativeAreaMusicOffset(request->chain, request->sourceStartMs);
			}
			break;
		}
		case ModEventType::AreaMusicRestoreNativeOffsetRequested:
		{
			RestoreNativeAreaMusicOffset();
			break;
		}
		default:
			break;
	}
}

bool AreaMusicManager::InstallWwiseObjectLookupHook()
{
	if (wwiseObjectLookupHookInitialized)
	{
		return originalWwiseObjectLookupFunc != nullptr;
	}

	const FunctionData* lookupFuncData = ModManager::GetFunctionData(wwiseObjectLookupFunctionName);
	if (!lookupFuncData || !lookupFuncData->address)
	{
		return false;
	}

	if (!ModManager::TryHookFunction(
		wwiseObjectLookupFunctionName,
		reinterpret_cast<void*>(&AreaMusicManager::WwiseObjectLookupHook)
	))
	{
		wwiseObjectLookupHookInitialized = true;
		Logging::Write(logPrefix, "Failed to install Wwise object lookup hook");
		return false;
	}

	wwiseObjectLookupHookInitialized = true;
	originalWwiseObjectLookupFunc =
		reinterpret_cast<WwiseObjectLookupFn>(lookupFuncData->originalFunction);
	if (!originalWwiseObjectLookupFunc)
	{
		Logging::Write(logPrefix, "Wwise object lookup hook installed without an original function");
		return false;
	}
	Logging::Write(logPrefix,
		"Installed Wwise object lookup hook at %p",
		reinterpret_cast<void*>(lookupFuncData->address)
	);
	return true;
}

void AreaMusicManager::CaptureWwiseObjectRegistry(void* registry)
{
	if (!registry)
	{
		return;
	}

	const uintptr_t registryAddress = reinterpret_cast<uintptr_t>(registry);
	if (!MemoryUtils::IsWritableAddress(registryAddress, 0x80))
	{
		return;
	}

	const uintptr_t previous = wwiseObjectRegistryAddress.exchange(
		registryAddress,
		std::memory_order_acq_rel
	);
	if (previous != registryAddress)
	{
		Logging::Write(logPrefix, "Captured Wwise object registry at %p", registry);
	}
}

void* __cdecl AreaMusicManager::WwiseObjectLookupHook(void* registry, uint32_t objectId, int tableIndex)
{
	WwiseObjectLookupFn original = originalWwiseObjectLookupFunc;
	if (!original)
	{
		const FunctionData* lookupFuncData = ModManager::GetFunctionData(wwiseObjectLookupFunctionName);
		original =
			lookupFuncData
			? reinterpret_cast<WwiseObjectLookupFn>(lookupFuncData->originalFunction)
			: nullptr;
		originalWwiseObjectLookupFunc = original;
	}

	if (!original)
	{
		return nullptr;
	}

	void* object = original(registry, objectId, tableIndex);
	const uintptr_t capturedRegistryAddress = wwiseObjectRegistryAddress.load(std::memory_order_acquire);
	if (
		registry
		&& reinterpret_cast<uintptr_t>(registry) != capturedRegistryAddress
		&& (object || !capturedRegistryAddress)
	)
	{
		CaptureWwiseObjectRegistry(registry);
	}

	return object;
}

bool AreaMusicManager::PatchNativeAreaMusicOffset(const AreaMusic::AreaMusicChain* chain, long long sourceStartMs)
{
	if (!chain || sourceStartMs <= 0)
	{
		return false;
	}

	RestoreNativeAreaMusicOffset();

	void* segment = LookupWwiseObject(chain->segmentId);
	void* track = LookupWwiseObject(chain->trackId);
	if (!segment || !track)
	{
		Logging::Write(logPrefix,
			"Could not locate live Wwise objects for chain (segment=%u track=%u): segment=%p track=%p",
			chain->segmentId,
			chain->trackId,
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();

	if (
		!MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		Logging::Write(logPrefix, "Refusing native offset patch: segment or track memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);

	// Native track source duration can be transient; segment duration is the stable source length.
	const uint32_t origSegmentDurationTicks = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	if (origSegmentDurationTicks == 0)
	{
		Logging::Write(logPrefix, "Cannot patch \"%s\": live segment duration is 0", chain->songName);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	const long long sourceDurationMs =
		static_cast<long long>(origSegmentDurationTicks) / wwiseTicksPerMillisecond;
	// Early state activation can expose stale metadata; refuse values that would truncate playback.
	if (sourceDurationMs < 1000 || sourceDurationMs > (60LL * 60 * 1000))
	{
		Logging::Write(logPrefix,
			"Skipping native offset patch for \"%s\": live segment duration is implausible (%lld ms from %u ticks)",
			chain->songName,
			sourceDurationMs,
			origSegmentDurationTicks
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}
	const long long clampedStartMs = ClampSourceStartOffsetMs(sourceStartMs, sourceDurationMs);
	const long long effectiveDurationMs = (std::max)(1LL, sourceDurationMs - clampedStartMs);
	const uint32_t sourceStartTicks = clampedStartMs <= 0 ? 0 : MillisecondsToWwiseTicks(clampedStartMs);
	const uint32_t effectiveDurationTicks = MillisecondsToWwiseTicks(effectiveDurationMs);

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingCount > 256
		|| (timingArray && timingCount > 0
			&& !MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(timingArray),
				static_cast<size_t>(timingCount) * liveTrackTimingStride))
	)
	{
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint32_t origBeginTrimTicks = 0;
	if (timingArray && timingCount > 0)
	{
		origBeginTrimTicks = MemoryUtils::ReadUnaligned<uint32_t>(timingArray + 0x18);
	}

	const uint32_t origSourceDurationTicks = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration
	);

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerCount > 256
		|| (markerArray && markerCount > 0
			&& !MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(markerArray),
				static_cast<size_t>(markerCount) * liveSegmentMarkerStride))
	)
	{
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	NativeAreaMusicBackup backup;
	backup.valid = true;
	backup.segmentId = chain->segmentId;
	backup.trackId = chain->trackId;
	backup.origSegmentDurationTicks = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	backup.origTrackSourceDurationTicks = origSourceDurationTicks;
	if (markerArray && markerCount > 0)
	{
		backup.origMarkerPositionTicks.reserve(markerCount);
		for (uint32_t i = 0; i < markerCount; i++)
		{
			backup.origMarkerPositionTicks.push_back(MemoryUtils::ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			));
		}
	}
	if (timingArray && timingCount > 0)
	{
		const size_t timingBytes = static_cast<size_t>(timingCount) * liveTrackTimingStride;
		backup.origTrackTimingBytes.assign(timingArray, timingArray + timingBytes);
	}

	MemoryUtils::WriteUnaligned<uint32_t>(segmentBytes + offsets.segmentDuration, effectiveDurationTicks);
	if (markerArray && markerCount > 0)
	{
		for (uint32_t i = 0; i < markerCount; i++)
		{
			const uint32_t markerTicks = MemoryUtils::ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			);
			if (markerTicks > 0 && markerTicks != effectiveDurationTicks)
			{
				MemoryUtils::WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					effectiveDurationTicks
				);
			}
		}
	}

	MemoryUtils::WriteUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration,
		origSegmentDurationTicks
	);
	if (timingArray && timingCount > 0)
	{
		for (uint32_t i = 0; i < timingCount; i++)
		{
			uint8_t* entry = timingArray + (static_cast<size_t>(i) * liveTrackTimingStride);
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x0c, 0);  // PlayAt
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x10, effectiveDurationTicks); // ClipDuration
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x14, origSegmentDurationTicks); // SourceDuration
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x18, sourceStartTicks); // BeginTrimOffset
		}
	}

	nativeAreaMusicBackup = std::move(backup);
	Logging::Write(logPrefix,
		"Patched native offset for \"%s\" (segment=%u track=%u source=%u) sourceDur=%lld ms, "
		"sourceStart=%lld ms/%u ticks, effective=%lld ms, origBeginTrim=%u ticks",
		chain->songName,
		chain->segmentId,
		chain->trackId,
		chain->sourceId,
		sourceDurationMs,
		clampedStartMs,
		sourceStartTicks,
		effectiveDurationMs,
		origBeginTrimTicks
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	return true;
}

void AreaMusicManager::RestoreNativeAreaMusicOffset()
{
	NativeAreaMusicBackup backup;
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		if (!nativeAreaMusicBackup.valid)
		{
			return;
		}
		backup = nativeAreaMusicBackup;
		nativeAreaMusicBackup = {};
	}

	void* segment = LookupWwiseObject(backup.segmentId);
	void* track = LookupWwiseObject(backup.trackId);
	if (!segment || !track)
	{
		Logging::Write(logPrefix,
			"Could not restore native offset (segment=%u track=%u not found)",
			backup.segmentId,
			backup.trackId
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);

	if (MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t)))
	{
		MemoryUtils::WriteUnaligned<uint32_t>(
			segmentBytes + offsets.segmentDuration,
			backup.origSegmentDurationTicks
		);
	}

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerArray
		&& markerCount == backup.origMarkerPositionTicks.size()
		&& markerCount > 0
		&& MemoryUtils::IsWritableAddress(
			reinterpret_cast<uintptr_t>(markerArray),
			static_cast<size_t>(markerCount) * liveSegmentMarkerStride
		)
	)
	{
		for (uint32_t i = 0; i < markerCount; i++)
		{
			MemoryUtils::WriteUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset,
				backup.origMarkerPositionTicks[i]
			);
		}
	}

	if (MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t)))
	{
		MemoryUtils::WriteUnaligned<uint32_t>(
			trackBytes + offsets.trackSourceDuration,
			backup.origTrackSourceDurationTicks
		);
	}
	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingArray
		&& !backup.origTrackTimingBytes.empty()
		&& static_cast<size_t>(timingCount) * liveTrackTimingStride == backup.origTrackTimingBytes.size()
		&& MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(timingArray), backup.origTrackTimingBytes.size())
	)
	{
		std::memcpy(timingArray, backup.origTrackTimingBytes.data(), backup.origTrackTimingBytes.size());
	}
	Logging::Write(logPrefix,
		"Restored native offset (segment=%u track=%u)",
		backup.segmentId,
		backup.trackId
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
}

void* AreaMusicManager::LookupWwiseObjectInTable(uint32_t objectId, int tableIndex)
{
	WwiseObjectLookupFn lookup = originalWwiseObjectLookupFunc;
	const uintptr_t registryAddress = wwiseObjectRegistryAddress.load(std::memory_order_acquire);
	if (!lookup || !registryAddress)
	{
		return nullptr;
	}

	if (!MemoryUtils::IsWritableAddress(registryAddress, 0x80))
	{
		return nullptr;
	}

	void* object = nullptr;
	uint32_t exceptionCode = 0;
	if (!LookupWwiseObjectNoUnwind(
		reinterpret_cast<uintptr_t>(lookup),
		reinterpret_cast<void*>(registryAddress),
		objectId,
		tableIndex,
		&object,
		&exceptionCode
	))
	{
		Logging::Write(logPrefix,
			"Wwise object lookup raised exception 0x%08x for object %u in table %d (registry %p)",
			exceptionCode,
			objectId,
			tableIndex,
			reinterpret_cast<void*>(registryAddress)
		);
		return nullptr;
	}
	return object;
}

void* AreaMusicManager::LookupWwiseObject(uint32_t objectId)
{
	void* object = LookupWwiseObjectInTable(objectId, 0);
	if (object)
	{
		return object;
	}
	return LookupWwiseObjectInTable(objectId, 1);
}

bool AreaMusicManager::PatchLiveAreaMusicMetadata(
	const MusicData* data,
	long long fallbackDurationMs,
	long long requestedSourceStartMs
)
{
	if (!AreaMusic::UsesOverride(data))
	{
		return false;
	}

	bool hasPreviousPatch = false;
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		hasPreviousPatch = liveAreaMusicMetadataBackup.valid || areaMusicOverrideMetadataPatched;
	}
	if (hasPreviousPatch)
	{
		RestoreLiveAreaMusicMetadata();
	}

	const long long sourceDurationMs = fallbackDurationMs;
	if (sourceDurationMs <= 0)
	{
		Logging::Write(logPrefix, "Cannot patch live Wwise metadata for \"%s\": no duration is known", data->name ? data->name : "");
		return false;
	}

	const long long sourceStartMs = ClampSourceStartOffsetMs(
		requestedSourceStartMs,
		sourceDurationMs
	);
	const long long effectiveDurationMs = (std::max)(
		1LL,
		sourceDurationMs - sourceStartMs
	);
	const uint32_t sourceDurationTicks = MillisecondsToWwiseTicks(sourceDurationMs);
	const uint32_t effectiveDurationTicks = MillisecondsToWwiseTicks(effectiveDurationMs);
	const uint32_t sourceStartTicks = sourceStartMs <= 0 ? 0 : MillisecondsToWwiseTicks(sourceStartMs);
	const bool customMediaOverride = AreaMusic::UsesCustomMediaOverride(data);
	const bool internalWwiseOverride = AreaMusic::UsesInternalWwiseOverride(data);
	const uint32_t sourceId = AreaMusic::OverrideTarget.sourceId;
	const uint32_t mediaSize = areaMusicOverrideBuffer
		? static_cast<uint32_t>((std::min)(
			areaMusicOverrideBuffer->bytes.size(),
			static_cast<size_t>((std::numeric_limits<uint32_t>::max)())
		))
		: 0;
	const uint32_t sourcePluginId = areaMusicOverrideBuffer
		? areaMusicOverrideBuffer->sourcePluginId
		: areaMusicOverrideSourcePluginId;
	const bool patchSourceMediaMetadata = areaMusicOverrideBuffer && !areaMusicOverrideBuffer->bytes.empty();

	void* segment = LookupWwiseObject(AreaMusic::OverrideTarget.segmentId);
	void* track = LookupWwiseObject(AreaMusic::OverrideTarget.trackId);
	if (!segment || !track)
	{
		Logging::Write(logPrefix,
			"Could not find live Wwise music objects for Area00 metadata patch (segment=%p, track=%p)",
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	LiveAreaMusicMetadataBackup backup{};
	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();
	if (
		!MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		Logging::Write(logPrefix, "Refusing live Wwise metadata patch: segment or track object memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerCount > 256
		|| (
			markerArray
			&& markerCount > 0
			&& !MemoryUtils::IsWritableAddress(
				reinterpret_cast<uintptr_t>(markerArray),
				static_cast<size_t>(markerCount) * liveSegmentMarkerStride
			)
		)
	)
	{
		Logging::Write(logPrefix, "Refusing live Wwise segment patch: marker count %u is unreasonable", markerCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* sourceArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackSourceArray
	);
	const uint32_t sourceCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceCount
	);
	if (
		sourceCount > 256
		|| (
			sourceArray
			&& sourceCount > 0
			&& !MemoryUtils::IsWritableAddress(
				reinterpret_cast<uintptr_t>(sourceArray),
				static_cast<size_t>(sourceCount) * liveTrackSourceStride
			)
		)
	)
	{
		Logging::Write(logPrefix, "Refusing live Wwise track patch: source entry count %u is unreasonable", sourceCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingCount > 256
		|| (
			timingArray
			&& timingCount > 0
			&& !MemoryUtils::IsWritableAddress(
				reinterpret_cast<uintptr_t>(timingArray),
				static_cast<size_t>(timingCount) * liveTrackTimingStride
			)
		)
	)
	{
		Logging::Write(logPrefix, "Refusing live Wwise track patch: timing entry count %u is unreasonable", timingCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	backup.valid = true;
	backup.segmentDurationTicks = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	if (markerArray)
	{
		backup.markerPositionTicks.reserve(markerCount);
		for (uint32_t i = 0; i < markerCount; i++)
		{
			backup.markerPositionTicks.push_back(MemoryUtils::ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			));
		}
	}

	backup.trackSourceEntryCount = sourceCount;
	if (sourceArray && sourceCount > 0)
	{
		const size_t sourceBytes = static_cast<size_t>(sourceCount) * liveTrackSourceStride;
		backup.trackSourceEntryBytes.assign(sourceArray, sourceArray + sourceBytes);
	}

	backup.trackSourceDurationTicks = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration
	);
	backup.trackTimingCount = timingCount;
	if (timingArray && timingCount > 0)
	{
		const size_t timingBytes = static_cast<size_t>(timingCount) * liveTrackTimingStride;
		backup.trackTimingBytes.assign(timingArray, timingArray + timingBytes);
	}

	uint32_t patchedSourceCount = 0;
	if (sourceArray && sourceCount > 0)
	{
		for (uint32_t i = 0; i < sourceCount; i++)
		{
			uint8_t* entry = sourceArray + (static_cast<size_t>(i) * liveTrackSourceStride);
			const uint32_t sourceKey = MemoryUtils::ReadUnaligned<uint32_t>(entry);
			if (sourceKey != AreaMusic::OverrideTarget.sourceId)
			{
				continue;
			}

			void* sourceObject = *reinterpret_cast<void**>(entry + liveTrackSourceObjectOffset);
			uint8_t* sourceObjectBytes = static_cast<uint8_t*>(sourceObject);
			if (
				sourceObjectBytes
				&& MemoryUtils::IsWritableAddress(
					reinterpret_cast<uintptr_t>(sourceObjectBytes),
					liveTrackSourceObjectSize
				)
			)
			{
				LiveAreaMusicMemoryBackup objectBackup{};
				objectBackup.data = sourceObjectBytes;
				objectBackup.bytes.assign(
					sourceObjectBytes,
					sourceObjectBytes + liveTrackSourceObjectSize
				);
				backup.sourceObjectBackups.push_back(std::move(objectBackup));
				MemoryUtils::WriteUnaligned<uint32_t>(sourceObjectBytes, sourceId);
				MemoryUtils::WriteUnaligned<uint32_t>(sourceObjectBytes + 0x04, sourceId);
				if (patchSourceMediaMetadata && mediaSize > 0)
				{
					MemoryUtils::WriteUnaligned<uint32_t>(sourceObjectBytes + 0x08, mediaSize);
				}
				if (patchSourceMediaMetadata)
				{
					MemoryUtils::WriteUnaligned<uint32_t>(sourceObjectBytes + 0x18, sourcePluginId);
				}
			}

			MemoryUtils::WriteUnaligned<uint32_t>(entry, sourceId);
			patchedSourceCount++;
		}
	}

	MemoryUtils::WriteUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration,
		effectiveDurationTicks
	);

	uint32_t patchedMarkerCount = 0;
	if (markerArray && markerCount > 0)
	{
		for (uint32_t i = 0; i < markerCount; i++)
		{
			const uint32_t markerTicks = MemoryUtils::ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			);
			if (markerTicks > 0 && markerTicks != effectiveDurationTicks)
			{
				MemoryUtils::WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					effectiveDurationTicks
				);
				patchedMarkerCount++;
			}
		}
	}

	MemoryUtils::WriteUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration,
		sourceDurationTicks
	);

	uint32_t patchedTimingCount = 0;
	if (timingArray && timingCount > 0)
	{
		for (uint32_t i = 0; i < timingCount; i++)
		{
			uint8_t* entry = timingArray + (static_cast<size_t>(i) * liveTrackTimingStride);
			const uint32_t entryId0 = MemoryUtils::ReadUnaligned<uint32_t>(entry);
			const uint32_t entryId1 = MemoryUtils::ReadUnaligned<uint32_t>(entry + sizeof(uint32_t));
			if (
				entryId0 != AreaMusic::OverrideTarget.sourceId
				&& entryId1 != AreaMusic::OverrideTarget.sourceId
			)
			{
				continue;
			}

			if (entryId0 == AreaMusic::OverrideTarget.sourceId)
			{
				MemoryUtils::WriteUnaligned<uint32_t>(entry, sourceId);
			}
			if (entryId1 == AreaMusic::OverrideTarget.sourceId)
			{
				MemoryUtils::WriteUnaligned<uint32_t>(entry + sizeof(uint32_t), sourceId);
			}
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x0c, 0);
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x10, effectiveDurationTicks);
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x14, sourceDurationTicks);
			MemoryUtils::WriteUnaligned<uint32_t>(entry + 0x18, sourceStartTicks);
			patchedTimingCount++;
		}
	}

	if (patchedSourceCount == 0 || patchedTimingCount == 0)
	{
		if (sourceArray && !backup.trackSourceEntryBytes.empty())
		{
			std::memcpy(sourceArray, backup.trackSourceEntryBytes.data(), backup.trackSourceEntryBytes.size());
		}
		for (const LiveAreaMusicMemoryBackup& objectBackup : backup.sourceObjectBackups)
		{
			if (
				objectBackup.data
				&& !objectBackup.bytes.empty()
				&& MemoryUtils::IsWritableAddress(
					reinterpret_cast<uintptr_t>(objectBackup.data),
					objectBackup.bytes.size()
				)
			)
			{
				std::memcpy(objectBackup.data, objectBackup.bytes.data(), objectBackup.bytes.size());
			}
		}
		MemoryUtils::WriteUnaligned<uint32_t>(
			segmentBytes + offsets.segmentDuration,
			backup.segmentDurationTicks
		);
		if (markerArray && markerCount == backup.markerPositionTicks.size())
		{
			for (uint32_t i = 0; i < markerCount; i++)
			{
				MemoryUtils::WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					backup.markerPositionTicks[i]
				);
			}
		}
		MemoryUtils::WriteUnaligned<uint32_t>(
			trackBytes + offsets.trackSourceDuration,
			backup.trackSourceDurationTicks
		);
		if (timingArray && !backup.trackTimingBytes.empty())
		{
			std::memcpy(timingArray, backup.trackTimingBytes.data(), backup.trackTimingBytes.size());
		}
		Logging::Write(logPrefix,
			"Could not patch target source %u in live Wwise track metadata for \"%s\" (source entries %u/%u, timing entries %u/%u)",
			AreaMusic::OverrideTarget.sourceId,
			data->name ? data->name : "",
			patchedSourceCount,
			sourceCount,
			patchedTimingCount,
			timingCount
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		areaMusicOverrideMetadataPatched = false;
		return false;
	}

	liveAreaMusicMetadataBackup = std::move(backup);
	areaMusicOverrideMetadataPatched = true;
	Logging::Write(logPrefix,
		"Patched live Area00 Wwise music metadata for \"%s\" "
		"(%s source id %u, media size 0x%08x, source plugin 0x%08x, source %lld ms/%u ticks, effective %lld ms/%u ticks, source start %lld ms/%u ticks, sources %u/%u, markers %u/%u, timing entries %u/%u)",
		data->name ? data->name : "",
		customMediaOverride ? "custom media" : (internalWwiseOverride ? "internal Wwise cloned media" : "area override"),
		sourceId,
		mediaSize,
		sourcePluginId,
		sourceDurationMs,
		sourceDurationTicks,
		effectiveDurationMs,
		effectiveDurationTicks,
		sourceStartMs,
		sourceStartTicks,
		patchedSourceCount,
		sourceCount,
		patchedMarkerCount,
		markerCount,
		patchedTimingCount,
		timingCount
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	return areaMusicOverrideMetadataPatched;
}

void AreaMusicManager::RestoreLiveAreaMusicMetadata()
{
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		if (!liveAreaMusicMetadataBackup.valid)
		{
			areaMusicOverrideMetadataPatched = false;
			return;
		}
	}

	LiveAreaMusicMetadataBackup backup{};
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		backup = liveAreaMusicMetadataBackup;
	}

	void* segment = LookupWwiseObject(AreaMusic::OverrideTarget.segmentId);
	void* track = LookupWwiseObject(AreaMusic::OverrideTarget.trackId);
	if (!segment || !track)
	{
		Logging::Write(logPrefix,
			"Could not find live Wwise music objects while restoring Area00 metadata (segment=%p, track=%p)",
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		{
			std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
			liveAreaMusicMetadataBackup = {};
			areaMusicOverrideMetadataPatched = false;
		}
		return;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();
	if (
		!MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !MemoryUtils::IsWritableAddress(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		Logging::Write(logPrefix, "Refusing live Wwise metadata restore: segment or track object memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		{
			std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
			liveAreaMusicMetadataBackup = {};
			areaMusicOverrideMetadataPatched = false;
		}
		return;
	}

	MemoryUtils::WriteUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration,
		backup.segmentDurationTicks
	);

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = MemoryUtils::ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerArray
		&& markerCount == backup.markerPositionTicks.size()
		&& MemoryUtils::IsWritableAddress(
			reinterpret_cast<uintptr_t>(markerArray),
			static_cast<size_t>(markerCount) * liveSegmentMarkerStride
		)
	)
	{
		for (uint32_t i = 0; i < markerCount; i++)
		{
			MemoryUtils::WriteUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset,
				backup.markerPositionTicks[i]
			);
		}
	}

	uint8_t* sourceArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackSourceArray
	);
	const uint32_t sourceCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceCount
	);
	if (
		sourceArray
		&& sourceCount == backup.trackSourceEntryCount
		&& !backup.trackSourceEntryBytes.empty()
		&& MemoryUtils::IsWritableAddress(
			reinterpret_cast<uintptr_t>(sourceArray),
			backup.trackSourceEntryBytes.size()
		)
	)
	{
		std::memcpy(
			sourceArray,
			backup.trackSourceEntryBytes.data(),
			backup.trackSourceEntryBytes.size()
		);
	}

	for (const LiveAreaMusicMemoryBackup& objectBackup : backup.sourceObjectBackups)
	{
		if (
			objectBackup.data
			&& !objectBackup.bytes.empty()
			&& MemoryUtils::IsWritableAddress(
				reinterpret_cast<uintptr_t>(objectBackup.data),
				objectBackup.bytes.size()
			)
		)
		{
			std::memcpy(objectBackup.data, objectBackup.bytes.data(), objectBackup.bytes.size());
		}
	}

	MemoryUtils::WriteUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration,
		backup.trackSourceDurationTicks
	);

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = MemoryUtils::ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingArray
		&& timingCount == backup.trackTimingCount
		&& !backup.trackTimingBytes.empty()
		&& MemoryUtils::IsWritableAddress(
			reinterpret_cast<uintptr_t>(timingArray),
			backup.trackTimingBytes.size()
		)
	)
	{
		std::memcpy(
			timingArray,
			backup.trackTimingBytes.data(),
			backup.trackTimingBytes.size()
		);
	}
	Logging::Write(logPrefix, "Restored live Area00 Wwise music metadata");
	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		liveAreaMusicMetadataBackup = {};
		areaMusicOverrideMetadataPatched = false;
	}
}

bool AreaMusicManager::ResolveWwiseMediaFunctions()
{
	if (wwiseMediaFunctionsResolved)
	{
		return setMediaFunc != nullptr;
	}

	wwiseMediaFunctionsResolved = true;
	HMODULE mainModule = GetModuleHandleA(nullptr);
	if (!mainModule)
	{
		Logging::Write(logPrefix, "Failed to get main module handle for Wwise media exports");
		return false;
	}

	setMediaFunc = reinterpret_cast<AkSetMediaFn>(
		GetProcAddress(mainModule, setMediaExportName)
	);
	unsetMediaFunc = reinterpret_cast<AkUnsetMediaFn>(
		GetProcAddress(mainModule, unsetMediaExportName)
	);

	if (!setMediaFunc)
	{
		Logging::Write(logPrefix, "Failed to resolve AK::SoundEngine::SetMedia export");
		return false;
	}
	if (!unsetMediaFunc)
	{
		Logging::Write(logPrefix, "AK::SoundEngine::UnsetMedia export was not resolved; cleanup will be skipped");
	}
	Logging::Write(logPrefix,
		"Resolved Wwise SetMedia at %p and UnsetMedia at %p",
		reinterpret_cast<void*>(setMediaFunc),
		reinterpret_cast<void*>(unsetMediaFunc)
	);
	return true;
}

bool AreaMusicManager::LoadAreaMusicManager(const char* overridePath)
{
	if (!overridePath || !overridePath[0])
	{
		return false;
	}

	const std::string path = overridePath;
	const std::string filename = Utils::FilenameFromPath(path);
	if (
		areaMusicOverrideBuffer
		&& !areaMusicOverrideBuffer->bytes.empty()
		&& areaMusicOverrideBuffer->path == path
		)
	{
		return true;
	}

	if (areaMusicOverrideRegistered)
	{
		Unset();
	}
	RetireBuffer();

	auto retiredIt = std::find_if(
		retiredAreaMusicManagerBuffers.begin(),
		retiredAreaMusicManagerBuffers.end(),
		[&path](const std::unique_ptr<AreaMusicManagerBuffer>& buffer)
		{
			return buffer
				&& !buffer->bytes.empty()
				&& buffer->path == path;
		}
	);
	if (retiredIt != retiredAreaMusicManagerBuffers.end())
	{
		areaMusicOverrideBuffer = std::move(*retiredIt);
		retiredAreaMusicManagerBuffers.erase(retiredIt);
		Logging::Write(logPrefix,
			"Reusing retired area music audio buffer for \"%s\" (%zu bytes, source plugin 0x%08x, duration %lld ms)",
			filename.c_str(),
			areaMusicOverrideBuffer->bytes.size(),
			areaMusicOverrideBuffer->sourcePluginId,
			areaMusicOverrideBuffer->durationMs
		);
		return true;
	}

	AudioDecoder::WwiseMediaBuffer media{};
	if (!AudioDecoder::LoadWwiseMedia(path, media))
	{
		Logging::Write(logPrefix, "Failed to load custom audio override as Wwise media: \"%s\"", filename.c_str());
		return false;
	}

	areaMusicOverrideBuffer = std::make_unique<AreaMusicManagerBuffer>();
	areaMusicOverrideBuffer->path = path;
	areaMusicOverrideBuffer->bytes = std::move(media.bytes);
	areaMusicOverrideBuffer->sourcePluginId = media.sourcePluginId;
	areaMusicOverrideBuffer->durationMs = media.durationMs;
	Logging::Write(logPrefix,
		"Loaded area music audio override from \"%s\" (%zu bytes, source plugin 0x%08x, duration %lld ms)",
		filename.c_str(),
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		areaMusicOverrideBuffer->durationMs
	);
	return true;
}

bool AreaMusicManager::LoadInternalWwiseMediaFromGameArchive(
	const MusicData* data,
	AreaMusicManagerBuffer& output
)
{
	if (!AreaMusic::UsesInternalWwiseOverride(data))
	{
		return false;
	}

	const InternalWwiseAreaTrackData& internalAreaTrack = data->internalWwiseAreaTrack;
	if (
		internalAreaTrack.sourceId == 0
		|| internalAreaTrack.streamMediaSize < 16
		|| internalAreaTrack.streamMediaSize > maxInternalWwiseMediaSize
	)
	{
		return false;
	}

	const std::string streamPath = DecimaArchiveReader::BuildStreamedWemPath(internalAreaTrack.sourceId);
	std::vector<uint8_t> mediaBytes;
	const DecimaArchiveReader::ReadFileResult readResult =
		DecimaArchiveReader::ReadInitialArchiveFile(
			streamPath,
			internalAreaTrack.streamMediaSize,
			mediaBytes
		);
	if (!readResult.success)
	{
		Logging::Write(logPrefix,
			"Failed to extract Decima stream \"%s\" for internal Wwise media \"%s\": %s",
			streamPath.c_str(),
			data->name ? data->name : "",
			readResult.error.c_str()
		);
		output = {};
		return false;
	}

	if (!LooksLikeWwiseMediaBuffer(mediaBytes.data(), mediaBytes.size()))
	{
		Logging::Write(logPrefix,
			"Extracted Decima stream \"%s\" for \"%s\" is not valid Wwise media (%zu/%zu bytes)",
			streamPath.c_str(),
			data->name ? data->name : "",
			readResult.copiedBytes,
			mediaBytes.size()
		);
		output = {};
		return false;
	}

	output = {};
	output.path = "internal-wwise:" + std::to_string(internalAreaTrack.sourceId);
	output.bytes = std::move(mediaBytes);
	output.sourcePluginId = internalAreaTrack.sourcePluginId
		? internalAreaTrack.sourcePluginId
		: areaMusicOverrideSourcePluginId;
	output.durationMs = data->maxLength;

	Logging::Write(logPrefix,
		"Extracted internal Wwise media for \"%s\" from Decima stream \"%s\" "
		"(%zu bytes, source plugin 0x%08x, duration %lld ms)",
		data->name ? data->name : "",
		streamPath.c_str(),
		output.bytes.size(),
		output.sourcePluginId,
		output.durationMs
	);
	return true;
}

bool AreaMusicManager::LoadInternalWwiseMedia(const MusicData* data)
{
	if (!AreaMusic::UsesInternalWwiseOverride(data))
	{
		return false;
	}

	const InternalWwiseAreaTrackData& internalAreaTrack = data->internalWwiseAreaTrack;
	const std::string path = "internal-wwise:" + std::to_string(internalAreaTrack.sourceId);
	if (
		areaMusicOverrideBuffer
		&& !areaMusicOverrideBuffer->bytes.empty()
		&& areaMusicOverrideBuffer->path == path
	)
	{
		return true;
	}

	if (areaMusicOverrideRegistered)
	{
		Unset();
	}
	RetireBuffer();

	auto retiredIt = std::find_if(
		retiredAreaMusicManagerBuffers.begin(),
		retiredAreaMusicManagerBuffers.end(),
		[&path](const std::unique_ptr<AreaMusicManagerBuffer>& buffer)
		{
			return buffer
				&& !buffer->bytes.empty()
				&& buffer->path == path;
		}
	);
	if (retiredIt != retiredAreaMusicManagerBuffers.end())
	{
		areaMusicOverrideBuffer = std::move(*retiredIt);
		retiredAreaMusicManagerBuffers.erase(retiredIt);
		Logging::Write(logPrefix,
			"Reusing cloned internal Wwise area override for \"%s\" (%zu bytes, source plugin 0x%08x, duration %lld ms)",
			data->name ? data->name : "",
			areaMusicOverrideBuffer->bytes.size(),
			areaMusicOverrideBuffer->sourcePluginId,
			areaMusicOverrideBuffer->durationMs
		);
		return true;
	}

	AreaMusicManagerBuffer extractedBuffer{};
	if (!LoadInternalWwiseMediaFromGameArchive(data, extractedBuffer))
	{
		Logging::Write(logPrefix,
			"Cannot register Area00 override buffer for internal Wwise media \"%s\" "
			"(graph key %u, internal source %u, expected stream size %u bytes)",
			data->name ? data->name : "",
			internalAreaTrack.graphKey,
			internalAreaTrack.sourceId,
			internalAreaTrack.streamMediaSize
		);
		return false;
	}

	areaMusicOverrideBuffer = std::make_unique<AreaMusicManagerBuffer>(std::move(extractedBuffer));
	Logging::Write(logPrefix,
		"Loaded internal Wwise media for \"%s\" from source %u into Area00 target override buffer "
		"(%zu bytes, target source %u, source plugin 0x%08x, duration %lld ms)",
		data->name ? data->name : "",
		internalAreaTrack.sourceId,
		areaMusicOverrideBuffer->bytes.size(),
		AreaMusic::OverrideTarget.sourceId,
		areaMusicOverrideBuffer->sourcePluginId,
		areaMusicOverrideBuffer->durationMs
	);
	return true;
}

void AreaMusicManager::HandleRegisterRequest(AreaMusic::RegisterRequest& request)
{
	request.handled = true;
	request.success = false;
	request.metadataPatched = false;
	request.effectiveDurationMs = request.data ? request.data->maxLength : 0;
	request.effectiveSourceStartMs = 0;

	const MusicData* data = request.data;
	if (!AreaMusic::UsesOverride(data))
	{
		request.success = true;
		return;
	}

	const bool internalWwiseOverride = AreaMusic::UsesInternalWwiseOverride(data);
	const bool customMediaOverride = AreaMusic::UsesCustomMediaOverride(data);
	if (internalWwiseOverride)
	{
		const InternalWwiseAreaTrackData& internalAreaTrack = data->internalWwiseAreaTrack;
		void* ranSeqObject = LookupWwiseObject(internalAreaTrack.ranSeqId);
		void* segmentObject = LookupWwiseObject(internalAreaTrack.segmentId);
		void* trackObject = LookupWwiseObject(internalAreaTrack.trackId);
		Logging::Write(logPrefix,
			"Preparing internal Wwise area override for \"%s\" "
			"(graph key %u, ranseq %u=%p, segment %u=%p, track %u=%p, internal source %u, "
			"stream size %u, target source %u, source plugin 0x%08x)",
			data->name ? data->name : "",
			internalAreaTrack.graphKey,
			internalAreaTrack.ranSeqId,
			ranSeqObject,
			internalAreaTrack.segmentId,
			segmentObject,
			internalAreaTrack.trackId,
			trackObject,
			internalAreaTrack.sourceId,
			internalAreaTrack.streamMediaSize,
			AreaMusic::OverrideTarget.sourceId,
			internalAreaTrack.sourcePluginId
		);
		ReleaseWwiseObject(ranSeqObject);
		ReleaseWwiseObject(segmentObject);
		ReleaseWwiseObject(trackObject);

		if (!ResolveWwiseMediaFunctions() || !LoadInternalWwiseMedia(data))
		{
			return;
		}
	}
	else if (customMediaOverride)
	{
		const char* overridePath = data ? data->customWemPath : nullptr;
		if (!ResolveWwiseMediaFunctions() || !LoadAreaMusicManager(overridePath))
		{
			return;
		}
	}
	else
	{
		return;
	}

	const uint32_t sourceId = AreaMusic::OverrideTarget.sourceId;
	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return;
	}

	const long long sourceDurationMs = areaMusicOverrideBuffer->durationMs > 0
		? areaMusicOverrideBuffer->durationMs
		: 0;
	const long long sourceStartMs = ClampSourceStartOffsetMs(
		request.sourceStartMs,
		sourceDurationMs
	);

	if (sourceDurationMs <= 0)
	{
		RestoreLiveAreaMusicMetadata();
		Logging::Write(logPrefix,
			"Area00 live metadata patch failed for \"%s\": source duration is unknown",
			data ? data->name : ""
		);
		return;
	}

	areaMusicOverrideMetadataPatched = PatchLiveAreaMusicMetadata(
		data,
		sourceDurationMs,
		sourceStartMs
	);
	if (!areaMusicOverrideMetadataPatched)
	{
		Logging::Write(logPrefix,
			"Area00 live metadata patch failed for \"%s\" (media %zu bytes, source plugin 0x%08x, duration %lld ms, source start %lld ms)",
			data ? data->name : "",
			areaMusicOverrideBuffer->bytes.size(),
			areaMusicOverrideBuffer->sourcePluginId,
			sourceDurationMs,
			sourceStartMs
		);
		return;
	}

	request.metadataPatched = true;
	request.effectiveSourceStartMs = sourceStartMs;
	request.effectiveDurationMs = (std::max)(1LL, sourceDurationMs - sourceStartMs);
	Logging::Write(logPrefix,
		"Area00 live metadata patch applied for \"%s\" "
		"(media %zu bytes, source plugin 0x%08x, duration %lld ms, source start %lld ms)",
		data ? data->name : "",
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		sourceDurationMs,
		sourceStartMs
	);

	if (areaMusicOverrideRegistered)
	{
		request.success = true;
		return;
	}

	AkSourceSettings media{};
	media.sourceId = sourceId;
	media.mediaMemory = areaMusicOverrideBuffer->bytes.data();
	media.mediaSize = static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size());

	uint32_t result = setMediaFunc(&media, 1);
	if (!result)
	{
		Logging::Write(logPrefix,
			"AK::SoundEngine::SetMedia failed for source %u with result %u",
			sourceId,
			result
		);
		if (areaMusicOverrideMetadataPatched)
		{
			RestoreLiveAreaMusicMetadata();
		}
		areaMusicOverrideMetadataPatched = false;
		request.metadataPatched = false;
		return;
	}

	areaMusicOverrideRegistered = true;
	areaMusicOverrideRegisteredMediaId = sourceId;
	const std::string overrideFilename = Utils::FilenameFromPath(areaMusicOverrideBuffer->path);
	Logging::Write(logPrefix,
		"Registered area music audio override for Wwise source %u from \"%s\" (source plugin 0x%08x)",
		sourceId,
		overrideFilename.c_str(),
		areaMusicOverrideBuffer->sourcePluginId
	);
	request.success = true;
	request.metadataPatched = areaMusicOverrideMetadataPatched;
}

void AreaMusicManager::Unset()
{
	if (!areaMusicOverrideRegistered)
	{
		RestoreLiveAreaMusicMetadata();
		return;
	}

	if (!ResolveWwiseMediaFunctions() || !unsetMediaFunc)
	{
		areaMusicOverrideRegistered = false;
		areaMusicOverrideRegisteredMediaId = 0;
		RestoreLiveAreaMusicMetadata();
		return;
	}

	AkSourceSettings media{};
	media.sourceId = areaMusicOverrideRegisteredMediaId
		? areaMusicOverrideRegisteredMediaId
		: AreaMusic::OverrideTarget.sourceId;
	if (areaMusicOverrideBuffer && !areaMusicOverrideBuffer->bytes.empty())
	{
		media.mediaMemory = areaMusicOverrideBuffer->bytes.data();
		media.mediaSize = static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size());
	}

	uint32_t result = unsetMediaFunc(&media, 1);
	if (!result)
	{
		Logging::Write(logPrefix,
			"AK::SoundEngine::UnsetMedia failed for source %u with result %u",
			media.sourceId,
			result
		);
	}
	else
	{
		Logging::Write(logPrefix, "Unregistered area music audio override for Wwise source %u", media.sourceId);
	}
	areaMusicOverrideRegistered = false;
	areaMusicOverrideRegisteredMediaId = 0;
	RestoreLiveAreaMusicMetadata();
}

void AreaMusicManager::RetireBuffer()
{
	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return;
	}
	const std::string filename = Utils::FilenameFromPath(areaMusicOverrideBuffer->path);
	Logging::Write(logPrefix,
		"Keeping retired area music audio buffer alive for \"%s\" (%zu bytes)",
		filename.c_str(),
		areaMusicOverrideBuffer->bytes.size()
	);
	retiredAreaMusicManagerBuffers.push_back(std::move(areaMusicOverrideBuffer));
}
