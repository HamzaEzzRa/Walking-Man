#pragma once

#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "Logger.h"

template<typename T>
class PlaybackQueue
{
public:
	PlaybackQueue() = default;
	PlaybackQueue(const T* data, size_t size)
	{
		SetData(data, size);
	}

	void Shuffle()
	{
		GeneratePlaybackOrder(true);
	}
	void Reset()
	{
		GeneratePlaybackOrder(false);
	}

	void SetData(const T* newData, size_t newSize)
	{
		if (newSize == 0 || newData == nullptr)
		{
			Logging::Write(logPrefix, "Setting empty data to playback queue.");
			data = nullptr;
			dataSize = 0;
			playbackOrder.clear();
			currentIndex = -1;
			return;
		}

		std::unique_ptr<T[]> newBuffer(new T[newSize]);
		std::copy(newData, newData + newSize, newBuffer.get());
		
		data = std::move(newBuffer);
		dataSize = newSize;
	}

	T GetCurrent()
	{
		if (IsEmpty())
		{
			Logging::Write(logPrefix, "Playback queue is empty, cannot get current item.");
			return T{};
		}
		if (currentIndex < 0)
		{
			currentIndex = 0;
		}
		return data[playbackOrder[currentIndex]];
	}
	T GetNext()
	{
		if (IsEmpty())
		{
			Logging::Write(logPrefix, "Playback queue is empty, cannot get next item.");
			return T{};
		}
		currentIndex = (currentIndex + 1) % dataSize;
		return GetCurrent();
	}
	T GetPrevious()
	{
		if (IsEmpty())
		{
			Logging::Write(logPrefix, "Playback queue is empty, cannot get previous item.");
			return T{};
		}
		currentIndex = (currentIndex + dataSize - 1) % dataSize;
		return GetCurrent();
	}

	const long long GetCurrentIndex() const
	{
		return currentIndex;
	}

	bool IsShuffled() const
	{
		return shuffled;
	}

	bool IsEmpty() const
	{
		return dataSize == 0;
	}

private:
	void GeneratePlaybackOrder(bool shuffle)
	{
		playbackOrder.resize(dataSize);
		if (dataSize > 0)
		{
			std::iota(playbackOrder.begin(), playbackOrder.end(), 0); // Fill with indices 0 to dataSize - 1
			if (shuffle)
			{
				std::mt19937 rng(std::random_device{}());
				std::shuffle(playbackOrder.begin(), playbackOrder.end(), rng);
			}
		}
		shuffled = shuffle;
		currentIndex = -1;
	}

private:
	inline static constexpr const char* logPrefix = "Music Playback Queue";

	std::unique_ptr<T[]> data;
	size_t dataSize = 0;

	bool shuffled = false;

	std::vector<size_t> playbackOrder;
	long long currentIndex = -1;
};
