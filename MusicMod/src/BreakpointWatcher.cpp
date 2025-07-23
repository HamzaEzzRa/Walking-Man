#include "BreakpointWatcher.h"
#include "Logger.h"

BreakpointWatcher::BreakpointWatcher(
    std::function<void()> onAccess,
    std::function<void()> onStop,
    uint32_t pollInterval,
    uint32_t timeout
) : onAccess_(onAccess), onStop_(onStop), pollIntervalMs_(pollInterval), timeoutMs_(timeout) {}

void BreakpointWatcher::Install(
    uintptr_t watchAddr, uintptr_t accessInstructionAddr, uintptr_t accessInstructionLength
) {
    watchAddr_ = watchAddr;
    accessInstructionAddr_ = accessInstructionAddr;
	accessInstructionLength_ = accessInstructionLength;
    active_ = true;
    accessSeen_ = false;
    lastAccessTime_ = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        watchers_.push_back(this);

        if (!handlerInstalled_) {
            AddVectoredExceptionHandler(1, ExceptionHandler);
            handlerInstalled_ = true;
        }
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(snapshot, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) {
                    HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                    if (hThread) {
                        CONTEXT ctx = {};
                        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (GetThreadContext(hThread, &ctx)) {
                            ctx.Dr0 = watchAddr_;
                            ctx.Dr7 |= 0x1;              // Enable local breakpoint
                            ctx.Dr7 |= (0b11 << 16);     // Read/Write
                            ctx.Dr7 |= (0b10 << 18);     // 8-byte watch
                            if (SetThreadContext(hThread, &ctx)) {
                                /*Logger logger("Breakpoint Watcher");
                                logger.Log("Dr0 set on thread %lu", te.th32ThreadID);*/
                            }
                            threadHandles_.push_back(hThread); // Store it for cleanup
                        }
                        else {
                            CloseHandle(hThread); // Only close on failure
                        }
                    }
                }
            } while (Thread32Next(snapshot, &te));
        }
        CloseHandle(snapshot);
    }

    Logger logger("Breakpoint Watcher");
    logger.Log(
        "Installed breakpoint watcher at %p, matching access from %p",
        (void*)watchAddr_, (void*)accessInstructionAddr_
    );

    monitorThread_ = std::thread([this]() {
        while (active_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_));
            if (!accessSeen_) continue;

            auto now = std::chrono::steady_clock::now();
            if (now - lastAccessTime_.load() > std::chrono::milliseconds(timeoutMs_)) {
                Logger logger("Breakpoint Watcher");
                logger.Log("Access timeout triggered for %p", (void*)watchAddr_);
                active_ = false;
                if (onStop_) onStop_();
                break;
            }
        }
    });
}

void BreakpointWatcher::Uninstall() {
    active_ = false;

    if (monitorThread_.joinable())
        monitorThread_.join();

    for (HANDLE hThread : threadHandles_) {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(hThread, &ctx)) {
            ctx.Dr0 = 0;
            ctx.Dr7 &= ~0x1;
            SetThreadContext(hThread, &ctx);
        }
        CloseHandle(hThread);
    }
    threadHandles_.clear();

    std::lock_guard<std::mutex> lock(registryMutex_);
    watchers_.erase(std::remove(watchers_.begin(), watchers_.end(), this), watchers_.end());

    if (watchers_.empty() && handlerInstalled_) {
        RemoveVectoredExceptionHandler(ExceptionHandler);
        handlerInstalled_ = false;
    }
}

LONG CALLBACK BreakpointWatcher::ExceptionHandler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t ip = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);

    std::lock_guard<std::mutex> lock(registryMutex_);
    for (auto* watcher : watchers_) {
        if (!watcher->active_)
        {
			/*Logger logger("Breakpoint Watcher");
			logger.Log("Watcher at %p is inactive, skipping", (void*)watcher->watchAddr_);*/
            continue;
        }

        if (ip - watcher->accessInstructionLength_ == watcher->accessInstructionAddr_) {
            /*Logger logger("Breakpoint Watcher");
            logger.Log("Detected access from address %p", (void*)ip);*/
            watcher->lastAccessTime_ = std::chrono::steady_clock::now();
            watcher->accessSeen_ = true;
            if (watcher->onAccess_) watcher->onAccess_();
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}