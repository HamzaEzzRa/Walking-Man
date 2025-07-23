#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "GameData.h"

#include "Logger.h"

class PatternScanner {
public:
    struct ScanProgress
    {
        const char* message = "";
        std::atomic<size_t> matchedPatterns{ 0 };
        size_t totalPatterns = 0;

        float GetProgress() const
        {
            float progress = 1.0f;
            if (totalPatterns > 0)
            {
                progress = static_cast<float>(matchedPatterns.load()) / totalPatterns;
            }
            return std::clamp(progress, 0.0f, 1.0f);
        }
    };

    inline static Logger logger{ "PatternScanner" };
    inline static std::atomic<bool> exiting = false;
    static constexpr uint16_t WILDCARD = 0xFFFF;

    inline static const std::vector<std::string> badModules = {
        "dxgi", "d3d11", "amd", "nv", "intel"
    };

    static std::vector<uint16_t> ParsePattern(const std::string& patternStr) {
        std::vector<uint16_t> pattern;
        std::stringstream ss(patternStr);
        std::string byte;
        while (ss >> byte) {
            if (byte == "?" || byte == "??")
                pattern.push_back(WILDCARD);
            else
                pattern.push_back(static_cast<uint16_t>(std::stoul(byte, nullptr, 16)));
        }
        return pattern;
    }

    static bool IsSafeRegion(const MEMORY_BASIC_INFORMATION& mbi, size_t patternSize) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t size = mbi.RegionSize;

        // Filter unsafe types
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE)) return false;
        if (size < patternSize) return false;
        if (base + size < base) return false; // overflow check

        // Skip known-bad DLLs
        if (mbi.Type == MEM_IMAGE) {
            char path[MAX_PATH];
            if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
                std::string mod = path;
                for (auto& c : mod) c = tolower(c); // case-insensitive
                for (const auto& bad : badModules) {
                    if (mod.find(bad) != std::string::npos)
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    static std::vector<uintptr_t> ScanAll(
        const std::string& patternStr,
        DWORD protectionFlags = PAGE_EXECUTE_READ,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        size_t maxResults = 0,
        bool reverse = false,
        uintptr_t startAddress = 0x10000,
        uintptr_t endAddress = 0,
        int maxThreads = std::thread::hardware_concurrency()
    )
    {
        exiting.store(false);
        auto startTime = std::chrono::high_resolution_clock::now();
        auto pattern = ParsePattern(patternStr);

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        uintptr_t regionStart = startAddress;
        uintptr_t regionEnd = endAddress;
        if (regionEnd <= startAddress)
        {
            regionEnd = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);
        }

        struct Region { uint8_t* base; size_t size; };
        std::vector<Region> regions;
        MEMORY_BASIC_INFORMATION mbi{};

        while (regionStart < regionEnd && VirtualQuery((LPCVOID)regionStart, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if ((mbi.Protect & protectionFlags) && IsSafeRegion(mbi, pattern.size())) {
                regions.push_back({ reinterpret_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize });
            }
            regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        if (reverse) {
            std::reverse(regions.begin(), regions.end());
        }

        logger.Log("ScanAll: %zu regions", regions.size());

        std::atomic<size_t> index = 0;
        std::mutex resultsLock;
        std::vector<uintptr_t> results;

        auto worker = [&]() {
            while (!exiting.load()) {
                if (timeout.count() > 0 &&
                    std::chrono::high_resolution_clock::now() - startTime >= timeout) {
                    logger.Log("ScanAll timed out");
                    exiting.store(true);
                    return;
                }

                size_t i = index.fetch_add(1);
                if (i >= regions.size()) return;

                auto& region = regions[i];
                uint8_t* start = region.base;
                uint8_t* end = start + region.size;

                for (auto it = start; it <= end - pattern.size(); ++it) {
                    if (exiting.load()) return;

                    bool matched = true;
                    for (size_t j = 0; j < pattern.size(); ++j) {
                        if (pattern[j] == WILDCARD) continue;
                        if (it[j] != static_cast<uint8_t>(pattern[j])) {
                            matched = false;
                            break;
                        }
                    }

                    if (matched) {
                        {
                            std::lock_guard<std::mutex> lock(resultsLock);
                            results.push_back(reinterpret_cast<uintptr_t>(it));
                            if (maxResults > 0 && results.size() >= maxResults) {
                                exiting.store(true);
                                return;
                            }
                        }
                    }
                }
            }
            };

        std::vector<std::thread> threads;
        for (int i = 0; i < maxThreads; ++i)
            threads.emplace_back(worker);
        for (auto& t : threads)
            t.join();

        logger.Log("ScanAll: found %zu results", results.size());
        exiting.store(false);
        return results;
    }

    static uintptr_t ScanFirst(
        const std::string& patternStr,
        DWORD protectionFlags = PAGE_EXECUTE_READ,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        bool reverse = false,
        uintptr_t startAddress = 0x10000,
        uintptr_t endAddress = 0,
        int maxThreads = std::thread::hardware_concurrency())
    {
        exiting.store(false);
        auto startTime = std::chrono::high_resolution_clock::now();
        auto pattern = ParsePattern(patternStr);

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        uintptr_t regionStart = startAddress;
        uintptr_t regionEnd = endAddress;
        if (regionEnd <= startAddress)
        {
            regionEnd = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);
        }

        struct Region { uint8_t* base; size_t size; };
        std::vector<Region> regions;
        MEMORY_BASIC_INFORMATION mbi{};

        while (regionStart < regionEnd && VirtualQuery((LPCVOID)regionStart, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if ((mbi.Protect & protectionFlags) && IsSafeRegion(mbi, pattern.size())) {
                regions.push_back({ reinterpret_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize });
            }
            regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        if (reverse) {
            std::reverse(regions.begin(), regions.end());
        }

        logger.Log("ScanFirst: %zu regions", regions.size());

        std::atomic<bool> found = false;
        std::atomic<uintptr_t> result = 0;
        std::atomic<size_t> index = 0;

        auto worker = [&]() {
            while (!found.load(std::memory_order_acquire) && !exiting.load()) {
                if (timeout.count() > 0 &&
                    std::chrono::high_resolution_clock::now() - startTime >= timeout) {
                    logger.Log("ScanFirst timed out");
                    exiting.store(true);
                    return;
                }

                size_t i = index.fetch_add(1);
                if (i >= regions.size()) return;

                auto& region = regions[i];
                uint8_t* start = region.base;
                uint8_t* end = start + region.size;

                for (auto it = start; it < end - pattern.size(); ++it) {
                    if (exiting.load()) return;

                    bool matched = true;
                    for (size_t j = 0; j < pattern.size(); ++j) {
                        if (pattern[j] == WILDCARD) continue;
                        if (it[j] != static_cast<uint8_t>(pattern[j])) {
                            matched = false;
                            break;
                        }
                    }
                    if (matched) {
                        result.store(reinterpret_cast<uintptr_t>(it), std::memory_order_release);
                        found.store(true, std::memory_order_release);
                        exiting.store(true);
                        return;
                    }
                }
            }
            };

        std::vector<std::thread> threads;
        for (int i = 0; i < maxThreads; ++i)
            threads.emplace_back(worker);
        for (auto& t : threads)
            t.join();

        logger.Log("ScanFirst: result = %p", (void*)result.load());
        exiting.store(false);
        return result.load();
    }

    template<typename T>
    static void ScanAsync(
        std::unordered_map<std::string, T>& scanTargets,
        std::function<void()> onComplete = nullptr,
        ScanProgress* progress = nullptr,
        DWORD protectionFlags = PAGE_EXECUTE_READ,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        bool reverse = false,
        uintptr_t startAddress = 0x10000,
        uintptr_t endAddress = 0,
        int maxThreads = std::thread::hardware_concurrency()
    )
    {
        std::thread([=, &scanTargets]() mutable {
            if (scanTargets.empty()) {
                logger.Log("ScanAsync: No targets to scan.");
                if (onComplete) onComplete();
                return;
            }

            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            if (endAddress <= startAddress)
                endAddress = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);

            auto startTime = std::chrono::high_resolution_clock::now();

            std::unordered_map<std::string, std::vector<uint16_t>> parsedPatterns;
            std::unordered_map<std::string, std::atomic<bool>> foundFlags;
            size_t maxPatternSize = 0;
            for (const auto& [name, target] : scanTargets) {
                auto pattern = ParsePattern(ScanTarget<T>::GetSignature(target));
                maxPatternSize = std::max(maxPatternSize, pattern.size());
                parsedPatterns[name] = std::move(pattern);
                foundFlags[name] = false;
            }

            if (progress) {
                progress->matchedPatterns.store(0);
                progress->totalPatterns = parsedPatterns.size();
            }

            struct Region { uint8_t* base; size_t size; };
            std::vector<Region> regions;
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t cur = startAddress;
            while (cur < endAddress &&
                VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if ((mbi.Protect & protectionFlags) && IsSafeRegion(mbi, maxPatternSize)) {
                    regions.push_back({ reinterpret_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize });
                }
                cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }

            if (reverse)
                std::reverse(regions.begin(), regions.end());

            logger.Log("ScanAsync: %zu regions, %zu patterns", regions.size(), parsedPatterns.size());

            std::atomic<size_t> regionIndex = 0;
            std::unordered_map<std::string, uintptr_t> foundAddresses;
            std::mutex resultMutex;
            std::atomic<size_t> patternsFound = 0;
            const size_t totalPatterns = parsedPatterns.size();
            std::atomic<bool> done{ false };

            auto worker = [&]() {
                while (!done.load()) {
                    if (timeout.count() > 0 &&
                        std::chrono::high_resolution_clock::now() - startTime >= timeout) {
                        logger.Log("ScanAsync: Timed out");
                        done.store(true);
                        return;
                    }

                    size_t i = regionIndex.fetch_add(1);
                    if (i >= regions.size())
                        return;

                    auto& region = regions[i];
                    uint8_t* start = region.base;
                    uint8_t* end = start + region.size;

                    for (auto& [name, pattern] : parsedPatterns) {
                        if (done.load()) return; // new: early exit
                        if (foundFlags[name].load(std::memory_order_acquire))
                            continue;

                        for (uint8_t* it = start; it <= end - pattern.size(); ++it) {
                            if (done.load()) return; // new: double check early exit
                            bool match = true;
                            for (size_t j = 0; j < pattern.size(); ++j) {
                                if (pattern[j] == WILDCARD) continue;
                                if (it[j] != static_cast<uint8_t>(pattern[j])) {
                                    match = false;
                                    break;
                                }
                            }

                            if (match) {
                                foundFlags[name].store(true, std::memory_order_release);
                                {
                                    std::lock_guard<std::mutex> lock(resultMutex);
                                    foundAddresses[name] = reinterpret_cast<uintptr_t>(it);
                                }

                                if (progress)
                                    progress->matchedPatterns.fetch_add(1);

                                size_t matched = patternsFound.fetch_add(1) + 1;
                                if (matched == totalPatterns) {
                                    done.store(true);
                                }

                                break;
                            }
                        }
                    }
                }
                };

            std::vector<std::thread> threads;
            for (int i = 0; i < maxThreads; ++i)
                threads.emplace_back(worker);
            for (auto& t : threads)
                t.join();

            for (const auto& [name, addr] : foundAddresses) {
                auto it = scanTargets.find(name);
                if (it != scanTargets.end()) {
                    ScanTarget<T>::SetAddress(it->second, addr);
                }
            }

            logger.Log("ScanAsync: found %zu/%zu patterns", foundAddresses.size(), totalPatterns);
            if (onComplete) onComplete();
        }).detach();
    }

    // Should extract the internal logic and reuse in both functions
    template<typename T>
    static void ScanAsyncPtr(
        std::unordered_map<std::string, T*>& scanTargets,
        std::function<void()> onComplete = nullptr,
        ScanProgress* progress = nullptr,
        DWORD protectionFlags = PAGE_EXECUTE_READ,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        bool reverse = false,
        uintptr_t startAddress = 0x10000,
        uintptr_t endAddress = 0,
        int maxThreads = std::thread::hardware_concurrency()
    )
    {
        std::thread([=]() mutable {
            if (scanTargets.empty()) {
                logger.Log("ScanAsyncPtr: No targets to scan.");
                if (onComplete) onComplete();
                return;
            }

            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            if (endAddress <= startAddress)
                endAddress = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);

            auto startTime = std::chrono::high_resolution_clock::now();

            std::unordered_map<std::string, std::vector<uint16_t>> parsedPatterns;
            std::unordered_map<std::string, std::atomic<bool>> foundFlags;
            size_t maxPatternSize = 0;
            for (const auto& [name, target] : scanTargets) {
                auto pattern = ParsePattern(ScanTarget<T>::GetSignature(*target));
                maxPatternSize = std::max(maxPatternSize, pattern.size());
                parsedPatterns[name] = std::move(pattern);
                foundFlags[name] = false;
            }

            if (progress) {
                progress->matchedPatterns.store(0);
                progress->totalPatterns = parsedPatterns.size();
            }

            struct Region { uint8_t* base; size_t size; };
            std::vector<Region> regions;
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t cur = startAddress;
            while (cur < endAddress &&
                VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if ((mbi.Protect & protectionFlags) && IsSafeRegion(mbi, maxPatternSize)) {
                    regions.push_back({ reinterpret_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize });
                }
                cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }

            if (reverse)
                std::reverse(regions.begin(), regions.end());

            logger.Log("ScanAsyncPtr: %zu regions, %zu patterns", regions.size(), parsedPatterns.size());

            std::atomic<size_t> regionIndex = 0;
            std::unordered_map<std::string, uintptr_t> foundAddresses;
            std::mutex resultMutex;
            std::atomic<size_t> patternsFound = 0;
            const size_t totalPatterns = parsedPatterns.size();
            std::atomic<bool> done{ false };

            auto worker = [&]() {
                while (!done.load()) {
                    if (timeout.count() > 0 &&
                        std::chrono::high_resolution_clock::now() - startTime >= timeout) {
                        logger.Log("ScanAsyncPtr: Timed out");
                        done.store(true);
                        return;
                    }

                    size_t i = regionIndex.fetch_add(1);
                    if (i >= regions.size())
                        return;

                    auto& region = regions[i];
                    uint8_t* start = region.base;
                    uint8_t* end = start + region.size;

                    for (auto& [name, pattern] : parsedPatterns) {
                        if (done.load()) return; // new: early exit
                        if (foundFlags[name].load(std::memory_order_acquire))
                            continue;

                        for (uint8_t* it = start; it <= end - pattern.size(); ++it) {
                            if (done.load()) return; // new: double check early exit
                            bool match = true;
                            for (size_t j = 0; j < pattern.size(); ++j) {
                                if (pattern[j] == WILDCARD) continue;
                                if (it[j] != static_cast<uint8_t>(pattern[j])) {
                                    match = false;
                                    break;
                                }
                            }

                            if (match) {
                                foundFlags[name].store(true, std::memory_order_release);
                                {
                                    std::lock_guard<std::mutex> lock(resultMutex);
                                    foundAddresses[name] = reinterpret_cast<uintptr_t>(it);
                                }

                                if (progress)
                                    progress->matchedPatterns.fetch_add(1);

                                size_t matched = patternsFound.fetch_add(1) + 1;
                                if (matched == totalPatterns) {
                                    done.store(true);
                                }

                                break;
                            }
                        }
                    }
                }
                };

            std::vector<std::thread> threads;
            for (int i = 0; i < maxThreads; ++i)
                threads.emplace_back(worker);
            for (auto& t : threads)
                t.join();

            for (const auto& [name, addr] : foundAddresses) {
                auto it = scanTargets.find(name);
                if (it != scanTargets.end()) {
                    ScanTarget<T>::SetAddress(*(it->second), addr);
                }
            }

            logger.Log("ScanAsyncPtr: found %zu/%zu patterns", foundAddresses.size(), totalPatterns);
            if (onComplete) onComplete();
        }).detach();
    }
};