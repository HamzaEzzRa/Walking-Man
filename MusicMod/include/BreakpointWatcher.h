#pragma once

#include <Windows.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <TlHelp32.h> 

class BreakpointWatcher {
public:
    BreakpointWatcher(
        std::function<void()> onAccess, std::function<void()> onStop, uint32_t pollInterval = 500, uint32_t timeout = 2000
    ); // pollInterval and timeout in milliseconds

    void Install(uintptr_t watchAddr, uintptr_t accessInstructionAddr, uintptr_t accessInstructionLength);
    void Uninstall();

private:
    static LONG CALLBACK ExceptionHandler(PEXCEPTION_POINTERS info);

    uintptr_t watchAddr_ = 0;
    uintptr_t accessInstructionAddr_ = 0;
    uintptr_t accessInstructionLength_ = 0;
    int pollIntervalMs_;
    int timeoutMs_;
    std::function<void()> onAccess_;
    std::function<void()> onStop_;
    std::thread monitorThread_;
    std::atomic<bool> active_ = false;
    std::atomic<std::chrono::steady_clock::time_point> lastAccessTime_;
    std::atomic<bool> accessSeen_ = false;
    std::vector<HANDLE> threadHandles_;

    static inline std::vector<BreakpointWatcher*> watchers_;
    static inline std::mutex registryMutex_;
    static inline bool handlerInstalled_ = false;
};
