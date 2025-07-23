#pragma once

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
			logger.Log("Setting empty data to playback queue.");
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

	const T& GetCurrent()
	{
		if (dataSize == 0)
		{
			logger.Log("Playback queue is empty, cannot get current item.");
			return nullptr;
		}
		if (currentIndex < 0)
		{
			currentIndex = 0;
		}
		return data[playbackOrder[currentIndex]];
	}
	const T& GetNext()
	{
		if (dataSize == 0)
		{
			logger.Log("Playback queue is empty, cannot get next item.");
			return nullptr;
		}
		currentIndex = (currentIndex + 1) % dataSize;
		return GetCurrent();
	}
	const T& GetPrevious()
	{
		if (dataSize == 0)
		{
			logger.Log("Playback queue is empty, cannot get previous item.");
			return nullptr;
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
	Logger logger = Logger("Music Playback Queue");

	std::unique_ptr<T[]> data;
	size_t dataSize = 0;

	bool shuffled = false;

	std::vector<size_t> playbackOrder;
	long long currentIndex = -1;
};