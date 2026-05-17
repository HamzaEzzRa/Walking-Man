#include "MemoryWatcher.h"

#include <chrono>
#include <cstring>

#include "MemoryUtils.h"

MemoryWatcher::MemoryWatcher(
	std::function<void(const void* newValue)> onChanged,
	uint32_t pollInterval
) : onChanged_(onChanged), pollIntervalMs_(pollInterval) {}

void MemoryWatcher::Install(const void* watchAddr, size_t size)
{
	Uninstall();

	watchAddr_ = watchAddr;
	watchSize_ = size;
	previousValue_.assign(size, 0);
	if (MemoryUtils::IsReadableAddress(watchAddr_, watchSize_))
	{
		std::memcpy(previousValue_.data(), watchAddr_, watchSize_);
	}

	active_ = true;
	monitorThread_ = std::thread(&MemoryWatcher::MonitorLoop, this);
}

void MemoryWatcher::Uninstall()
{
	active_ = false;
	if (monitorThread_.joinable())
	{
		monitorThread_.join();
	}
	watchAddr_ = nullptr;
}

void MemoryWatcher::MonitorLoop()
{
	std::vector<uint8_t> currentValue(watchSize_);
	while (active_)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_));
		if (!active_)
		{
			break;
		}

		if (!MemoryUtils::IsReadableAddress(watchAddr_, watchSize_))
		{
			continue;
		}

		std::memcpy(currentValue.data(), watchAddr_, watchSize_);
		if (currentValue == previousValue_)
		{
			continue;
		}

		previousValue_ = currentValue;
		if (onChanged_)
		{
			onChanged_(currentValue.data());
		}
	}
}
