#include "MemoryWatcher.h"

#include <cstring>

MemoryWatcher::MemoryWatcher(
    std::function<void(const void* newValue)> onChanged,
    uint32_t pollInterval
) : onChanged_(onChanged), pollIntervalMs_(pollInterval) {}

void MemoryWatcher::Install(const void* watchAddr, size_t size)
{
    Uninstall(); // ensure previous thread is cleaned

    watchAddr_ = watchAddr;
    watchSize_ = size;
    previousValue_.resize(size);
    std::memcpy(previousValue_.data(), watchAddr_, size);

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
}

void MemoryWatcher::MonitorLoop() {
    std::vector<uint8_t> currentValue(watchSize_);
    while (active_)
    {
        if (IsReadableAddress(watchAddr_, watchSize_))
        {
            std::memcpy(currentValue.data(), watchAddr_, watchSize_);
            if (currentValue != previousValue_)
            {
                previousValue_ = currentValue;
                if (onChanged_) onChanged_(currentValue.data());
            }
		}
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_));
    }
}

bool MemoryWatcher::IsReadableAddress(const void* addr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
    return mbi.State == MEM_COMMIT
        && !(mbi.Protect & PAGE_NOACCESS)
        && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}