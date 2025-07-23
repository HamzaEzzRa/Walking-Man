#pragma once

#include <Windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class MemoryWatcher {
public:
    MemoryWatcher(std::function<void(const void* newValue)>, uint32_t);

    void Install(const void*, size_t);
    void Uninstall();

private:
    void MonitorLoop();
    bool IsReadableAddress(const void*, size_t);

    const void* watchAddr_ = nullptr;
    size_t watchSize_ = 0;
    std::vector<uint8_t> previousValue_;
    std::function<void(const void* newValue)> onChanged_;
    std::thread monitorThread_;
    std::atomic<bool> active_ = false;
    uint32_t pollIntervalMs_;
};