#pragma once

#include <Windows.h>
#include <map>
#include <vector>
#include <Psapi.h>
#include <sstream>
#include <unordered_map>
#include <iomanip>
#include <string>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <algorithm>

#define NMD_ASSEMBLY_IMPLEMENTATION
#define NMD_ASSEMBLY_PRIVATE
#include "nmd_assembly.h"

#include "Logger.h"

#define PAGE_EXEC_ALL (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
#define PAGE_READ_ALL (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)

// Contains various memory manipulation functions related to hooking or modding
namespace MemoryUtils
{
	static Logger logger{ "MemoryUtils" };
	static constexpr int maskBytes = 0xFFFF;
	
	struct HookInformation
	{
		std::vector<unsigned char> originalBytes = { 0 };
		uintptr_t trampolineInstructionsAddress = 0;
	};
	static std::unordered_map<uintptr_t, HookInformation> InfoBufferForHookedAddresses;

	// Disables or enables the memory protection in a given region. 
	// Remembers and restores the original memory protection type of the given addresses.
	static void ToggleMemoryProtection(bool enableProtection, uintptr_t address, size_t size)
	{
		static std::map<uintptr_t, DWORD> protectionHistory;
		if (enableProtection && protectionHistory.find(address) != protectionHistory.end())
		{
			VirtualProtect((void*)address, size, protectionHistory[address], &protectionHistory[address]);
			protectionHistory.erase(address);
		}
		else if (!enableProtection && protectionHistory.find(address) == protectionHistory.end())
		{
			DWORD oldProtection = 0;
			VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &oldProtection);
			protectionHistory[address] = oldProtection;
		}
	}

	// Copies memory after changing the permissions at both the source and destination so we don't get an access violation.
	static void MemCopy(uintptr_t destination, uintptr_t source, size_t numBytes)
	{
		ToggleMemoryProtection(false, destination, numBytes);
		ToggleMemoryProtection(false, source, numBytes);
		memcpy((void*)destination, (void*)source, numBytes);
		ToggleMemoryProtection(true, source, numBytes);
		ToggleMemoryProtection(true, destination, numBytes);
	}

	// Simple wrapper around memset
	static void MemSet(uintptr_t address, unsigned char byte, size_t numBytes)
	{
		ToggleMemoryProtection(false, address, numBytes);
		memset((void*)address, byte, numBytes);
		ToggleMemoryProtection(true, address, numBytes);
	}

	// Gets the base address of the game's memory.
	static uintptr_t GetProcessBaseAddress(DWORD processId)
	{
		DWORD_PTR baseAddress = 0;
		HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);

		if (processHandle)
		{
			DWORD bytesRequired = 0;
			if (EnumProcessModules(processHandle, NULL, 0, &bytesRequired))
			{
				if (bytesRequired)
				{
					LPBYTE moduleArrayBytes = (LPBYTE)LocalAlloc(LPTR, bytesRequired);
					if (moduleArrayBytes)
					{
						HMODULE* moduleArray = (HMODULE*)moduleArrayBytes;
						if (EnumProcessModules(processHandle, moduleArray, bytesRequired, &bytesRequired))
						{
							baseAddress = (DWORD_PTR)moduleArray[0];
						}

						LocalFree(moduleArrayBytes);
					}
				}
			}

			CloseHandle(processHandle);
		}

		return baseAddress;
	}

	static std::string GetCurrentProcessName()
	{
		char lpFilename[MAX_PATH];
		GetModuleFileNameA(NULL, lpFilename, sizeof(lpFilename));
		std::string moduleName = strrchr(lpFilename, '\\');
		moduleName = moduleName.substr(1, moduleName.length());
		return moduleName;
	}

	static std::string GetCurrentModuleName()
	{
		HMODULE module = NULL;

		static char dummyStaticVariableToGetModuleName = 'x';
		GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, 
			&dummyStaticVariableToGetModuleName, 
			&module);

		char lpFilename[MAX_PATH];
		GetModuleFileNameA(module, lpFilename, sizeof(lpFilename));
		char* lastSlash = strrchr(lpFilename, '\\');
		std::string moduleName = "";
		if (lastSlash != nullptr)
		{
			moduleName = lastSlash;
			moduleName = moduleName.substr(1, moduleName.length());
			moduleName.erase(moduleName.find(".dll"), moduleName.length());
		}
		return moduleName;
	}

	static uintptr_t GetMainModuleBase()
	{
		return reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
	}

	static size_t GetMainModuleSize()
	{
		MODULEINFO moduleInfo = { 0 };
		if (GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &moduleInfo, sizeof(moduleInfo)))
		{
			return moduleInfo.SizeOfImage;
		}
		return 0;
	}

	static void ShowErrorPopup(std::string error, std::string title="")
	{
		logger.Log("Raised error: %s", error.c_str());
		if (title.empty())
		{
			title = GetCurrentModuleName();
		}
		MessageBox(NULL, error.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
	}

	static void PrintPattern(std::vector<uint16_t> pattern)
	{
		std::string patternString = "";
		for (auto bytes : pattern)
		{
			std::stringstream stream;
			std::string byte = "";
			if (bytes == maskBytes)
			{
				byte = "??";
			}
			else
			{
				stream << "0x" << std::hex << bytes;
				byte = stream.str();
			}
			patternString.append(byte + " ");
		}
		logger.Log("Pattern: %s", patternString.c_str());
	}

	static void ParseHexString(const std::string& pattern, std::vector<uint8_t>& bytes, std::vector<bool>& mask)
	{
		std::istringstream stream(pattern);
		std::string token;

		while (stream >> token) {
			if (token == "?" || token == "??") {
				bytes.push_back(0x00); // Placeholder value
				mask.push_back(false);
			}
			else {
				if (token.length() != 2 || !isxdigit(token[0]) || !isxdigit(token[1])) {
					throw std::invalid_argument("Invalid hex byte: " + token);
				}
				uint8_t byte = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
				bytes.push_back(byte);
				mask.push_back(true);
			}
		}
	}

	// Scans the memory of the main process module for the given signature.
	static uintptr_t SigScan(
		const std::string& patternStr,
		DWORD protectionFlags
		//int maxThreads = std::thread::hardware_concurrency()
	) {
		std::vector<uint16_t> pattern;
		std::stringstream ss(patternStr);
		std::string byte;
		while (ss >> byte) {
			if (byte == "?" || byte == "??") {
				pattern.push_back(maskBytes);
			}
			else {
				try {
					uint16_t byteValue = static_cast<uint16_t>(std::stoul(byte, nullptr, 16));
					pattern.push_back(byteValue);
				}
				catch (...) {
					logger.Log("Error parsing pattern at: %s", byte.c_str());
				}
			}
		}

		//uintptr_t regionStart = GetProcessBaseAddress(GetCurrentProcessId());
		uintptr_t regionStart = 0x000000000010000;
		size_t numRegionsChecked = 0;
		uintptr_t currentAddress = 0;
		MEMORY_BASIC_INFORMATION memoryInfo;

		while (VirtualQuery((void*)regionStart, &memoryInfo, sizeof(memoryInfo)) && regionStart < 0x7FFFFFFFFFFF) {
			bool isReadable = (
				memoryInfo.State == MEM_COMMIT &&
				(memoryInfo.Protect & protectionFlags) &&
				!(memoryInfo.Protect & PAGE_GUARD)
			);

			if (isReadable) {
				uint8_t* region = reinterpret_cast<uint8_t*>(memoryInfo.BaseAddress);
				size_t regionSize = memoryInfo.RegionSize;
				logger.Log("Scanning: %p - %p", memoryInfo.BaseAddress,
					(void*)((uintptr_t)memoryInfo.BaseAddress + memoryInfo.RegionSize));

				uint8_t* regionStartPtr = reinterpret_cast<uint8_t*>(memoryInfo.BaseAddress);
				uint8_t* regionEndPtr = regionStartPtr + memoryInfo.RegionSize;

				auto it = std::search(
					regionStartPtr,
					regionEndPtr - pattern.size(),
					pattern.begin(),
					pattern.end(),
					[](uint8_t memByte, uint16_t patByte) {
						return patByte == maskBytes || memByte == static_cast<uint8_t>(patByte);
					});

				if (it != (regionEndPtr - pattern.size())) {
					uintptr_t foundAddr = reinterpret_cast<uintptr_t>(it);
					logger.Log("Found signature at: %p", (void*)foundAddr);
					return foundAddr;
				}
			}
			regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress) + memoryInfo.RegionSize;
			numRegionsChecked++;
		}

		logger.Log("Signature not found after scanning %d regions.", numRegionsChecked);
		ShowErrorPopup("Could not find signature!");
		return 0;
	}

	static std::vector<uintptr_t> SigScanAllMatches(const std::string& patternStr) {
		std::vector<uint16_t> pattern;
		std::stringstream ss(patternStr);
		std::string byte;
		while (ss >> byte) {
			if (byte == "?" || byte == "??") {
				pattern.push_back(maskBytes);
			}
			else {
				try {
					uint16_t byteValue = static_cast<uint16_t>(std::stoul(byte, nullptr, 16));
					pattern.push_back(byteValue);
				}
				catch (...) {
					logger.Log("Error parsing pattern at: %s", byte.c_str());
				}
			}
		}

		std::vector<uintptr_t> matches;
		//uintptr_t regionStart = GetProcessBaseAddress(GetCurrentProcessId());
		uintptr_t regionStart = 0x000000000010000;
		size_t numRegionsChecked = 0;
		uintptr_t currentAddress = 0;
		MEMORY_BASIC_INFORMATION memoryInfo;

		while (VirtualQuery((void*)regionStart, &memoryInfo, sizeof(memoryInfo)) && regionStart < 0x7FFFFFFFFFFF) {
			bool isReadable = (
				memoryInfo.State == MEM_COMMIT &&
				(memoryInfo.Protect & (PAGE_EXEC_ALL | PAGE_READ_ALL)) &&
				!(memoryInfo.Protect & PAGE_GUARD)
				);

			if (isReadable) {
				uint8_t* region = reinterpret_cast<uint8_t*>(memoryInfo.BaseAddress);
				size_t regionSize = memoryInfo.RegionSize;
				/*logger.Log("Scanning: %p - %p", memoryInfo.BaseAddress,
					(void*)((uintptr_t)memoryInfo.BaseAddress + memoryInfo.RegionSize));*/

				for (size_t offset = 0; offset < regionSize - pattern.size(); ++offset) {
					bool matched = true;

					for (size_t i = 0; i < pattern.size(); ++i) {
						uint8_t byteToCheck = region[offset + i];
						if (pattern[i] != maskBytes && byteToCheck != static_cast<uint8_t>(pattern[i])) {
							matched = false;
							break;
						}
					}

					if (matched) {
						uintptr_t addr = reinterpret_cast<uintptr_t>(region + offset);
						logger.Log("Found signature at: %p", addr);
						matches.push_back(addr);
					}
				}
			}
			regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress) + memoryInfo.RegionSize;
			numRegionsChecked++;
		}

		if (matches.empty()) {
			logger.Log("Signature not found after scanning %d regions.", numRegionsChecked);
			ShowErrorPopup("Could not find signature!");
		}

		return matches;
	}

	static uintptr_t AllocateMemory(size_t numBytes)
	{
		uintptr_t memoryAddress = NULL;
		memoryAddress = (uintptr_t)VirtualAlloc(NULL, numBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

		if (memoryAddress == NULL)
		{
			logger.Log("Failed to allocate %i bytes of memory", numBytes);
		}
		else
		{
			logger.Log("Allocated %i bytes of memory at %p", numBytes, memoryAddress);
			MemSet(memoryAddress, 0x90, numBytes);
		}

		return memoryAddress;
	}

	static uintptr_t AllocateMemoryWithin32BitRange(size_t numBytes, uintptr_t origin)
	{
		uintptr_t memoryAddress = NULL;
		size_t unidirectionalRange = 0x7fffffff;
		uintptr_t lowerBound = origin - unidirectionalRange;
		uintptr_t higherBound = origin + unidirectionalRange;
		size_t numAttempts = 0;
		for (size_t i = lowerBound; i < higherBound;)
		{
			numAttempts++;
			memoryAddress = (uintptr_t)VirtualAlloc((void*)i, numBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

			if (memoryAddress != NULL)
			{
				bool memoryAddressIsAcceptable = memoryAddress >= lowerBound && memoryAddress <= higherBound;
				if (memoryAddressIsAcceptable)
				{
					break;
				}
				else
				{
					MEMORY_BASIC_INFORMATION info;
					VirtualQuery((void*)memoryAddress, &info, sizeof(MEMORY_BASIC_INFORMATION));
					i += info.RegionSize;
					VirtualFree((void*)memoryAddress, 0, MEM_RELEASE);
				}
			}
			else
			{
				size_t arbitraryIncrement = 10000;
				i += arbitraryIncrement;
			}
		}

		if (memoryAddress == NULL)
		{
			logger.Log(
				"Failed to allocate %i bytes of memory. Origin: %p, lower: %p, higher: %p, attempts: %i", 
				numBytes,
				origin,
				lowerBound,
				higherBound,
				numAttempts);
		}
		else
		{
			logger.Log("Allocated %i bytes of memory at %p", numBytes, memoryAddress);
			MemSet(memoryAddress, 0x90, numBytes);
		}

		return memoryAddress;
	}

	static bool IsRelativeNearJumpPresentAtAddress(uintptr_t address)
	{
		std::vector<unsigned char> buffer(1, 0x90);
		std::vector<unsigned char> assemblyRelativeNearJumpByte = { 0xe9 };
		MemCopy((uintptr_t)&buffer[0], address, 1);
		if (buffer == assemblyRelativeNearJumpByte)
		{
			return true;
		};
		return false;
	}

	static bool IsAbsoluteIndirectNearJumpPresentAtAddress(uintptr_t address)
	{
		std::vector<unsigned char> buffer(3, 0x90);
		std::vector<unsigned char> absoluteIndirectNearJumpBytes = { 0x48, 0xff, 0x25 };
		MemCopy((uintptr_t)&buffer[0], address, 3);
		if (buffer == absoluteIndirectNearJumpBytes)
		{
			return true;
		}
		return false;
	}

	static bool IsAbsoluteDirectFarJumpPresentAtAddress(uintptr_t address)
	{
		std::vector<unsigned char> absoluteDirectFarJump = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 };
		std::vector<unsigned char> buffer(absoluteDirectFarJump.size(), 0x90);
		MemCopy((uintptr_t)&buffer[0], address, buffer.size());
		if (buffer == absoluteDirectFarJump)
		{
			return true;
		}
		return false;
	}

	static bool IsAddressHooked(uintptr_t address)
	{
		if (
			IsRelativeNearJumpPresentAtAddress(address)
			|| IsAbsoluteIndirectNearJumpPresentAtAddress(address)
			|| IsAbsoluteDirectFarJumpPresentAtAddress(address))
		{
			return true;
		}
		return false;
	}

	static size_t CalculateRequiredAsmClearance(uintptr_t address, size_t minimumClearance)
	{
		size_t maximumAmountOfBytesToCheck = 30;
		std::vector<uint8_t> bytesBuffer(maximumAmountOfBytesToCheck, 0x90);
		MemCopy((uintptr_t)&bytesBuffer[0], address, maximumAmountOfBytesToCheck);

		if (IsAbsoluteDirectFarJumpPresentAtAddress(address))
		{
			return 14;
		}

		size_t requiredClearance = 0;
		for (size_t byteCount = 0; byteCount < maximumAmountOfBytesToCheck;)
		{
			size_t instructionSize = nmd_x86_ldisasm(
				&bytesBuffer[byteCount],
				maximumAmountOfBytesToCheck - byteCount,
				NMD_X86_MODE_64);

			if (instructionSize <= 0)
			{
				logger.Log("Instruction invalid, could not check length!");
				return minimumClearance;
			}

			byteCount += instructionSize;
			requiredClearance = byteCount;
			if (requiredClearance >= minimumClearance)
			{
				break;
			}

			//byteCount += instructionSize;
		}

		return requiredClearance;
	}

	static uintptr_t CalculateAbsoluteDestinationFromRelativeNearJumpAtAddress(uintptr_t relativeNearJumpMemoryLocation)
	{
		int32_t offset = 0;
		MemCopy((uintptr_t)&offset, relativeNearJumpMemoryLocation + 1, 4);
		uintptr_t absoluteAddress = relativeNearJumpMemoryLocation + 5 + offset;
		return absoluteAddress;
	}

	static uintptr_t CalculateAbsoluteDestinationFromAbsoluteIndirectNearJumpAtAddress(uintptr_t absoluteIndirectNearJumpMemoryLocation)
	{
		int32_t offset = 0;
		MemCopy((uintptr_t)&offset, absoluteIndirectNearJumpMemoryLocation + 3, 4);
		uintptr_t memoryContainingAbsoluteAddress = absoluteIndirectNearJumpMemoryLocation + 7 + offset;
		uintptr_t absoluteAddress = *(uintptr_t*)memoryContainingAbsoluteAddress;
		return absoluteAddress;
	}

	static uintptr_t CalculateAbsoluteDestinationFromAbsoluteDirectFarJumpAtAddress(uintptr_t absoluteDirectFarJumpMemoryLocation)
	{
		uintptr_t absoluteAddress = 0;
		MemCopy((uintptr_t)&absoluteAddress, absoluteDirectFarJumpMemoryLocation + 6, 8);
		return absoluteAddress;
	}

	static int32_t CalculateRelativeDisplacementForRelativeJump(uintptr_t relativeJumpAddress, uintptr_t destinationAddress)
	{
		return -int32_t(relativeJumpAddress + 5 - destinationAddress);
	}

	// Places a 14-byte absolutely addressed jump from A to B. 
	// Add extra clearance when the jump doesn't fit cleanly.
	static void PlaceAbsoluteJump(uintptr_t address, uintptr_t destinationAddress, size_t clearance = 14)
	{
		MemSet(address, 0x90, clearance);
		unsigned char absoluteJumpBytes[6] = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
		MemCopy(address, (uintptr_t)&absoluteJumpBytes[0], 6);
		MemCopy(address + 6, (uintptr_t)&destinationAddress, 8);
		logger.Log("Created absolute jump from %p to %p with a clearance of %i", address, destinationAddress, clearance);
	}

	// Places a 5-byte relatively addressed jump from A to B. 
	// Add extra clearance when the jump doesn't fit cleanly.
	static void PlaceRelativeJump(uintptr_t address, uintptr_t destinationAddress, size_t clearance = 5)
	{
		MemSet(address, 0x90, clearance);
		unsigned char relativeJumpBytes[5] = { 0xe9, 0x00, 0x00, 0x00, 0x00 };
		MemCopy(address, (uintptr_t)&relativeJumpBytes[0], 5);
		int32_t relativeAddress = CalculateRelativeDisplacementForRelativeJump(address, destinationAddress);
		MemCopy((address + 1), (uintptr_t)&relativeAddress, 4);
		logger.Log("Created relative jump from %p to %p with a clearance of %i", address, destinationAddress, clearance);
	}

	static std::string ConvertVectorOfBytesToStringOfHex(std::vector<uint8_t> bytes)
	{
		std::string hexString = "";
		for (auto byte : bytes)
		{
			std::stringstream stream;
			std::string byteAsHex = "";
			stream << std::hex << std::setfill('0') << std::setw(2) << (int)byte;
			byteAsHex = stream.str();
			hexString.append("0x" + byteAsHex + " ");
		}
		return hexString;
	}

	static void PrintBytesAtAddress(uintptr_t address, size_t numBytes)
	{
		std::vector<uint8_t> bytesBuffer(numBytes, 0x90);
		MemCopy((uintptr_t)&bytesBuffer[0], address, bytesBuffer.size());
		std::string hexString = ConvertVectorOfBytesToStringOfHex(bytesBuffer);
		logger.Log("Bytes: %s", hexString.c_str());
	}

	// Place a trampoline hook from A to B while taking third-party hooks into consideration.
	// Add extra clearance when the jump doesn't fit cleanly.
	static void PlaceHook(uintptr_t addressToHook, uintptr_t destinationAddress, uintptr_t* returnAddress)
	{
		logger.Log("Hooking...");

		// Most overlays don't care if we overwrite the 0xE9 jump and place it somewhere else, but MSI Afterburner does.
		// So instead of overwriting jumps we follow them and place our jump at the final destination.
		int maxFollowAttempts = 50;
		int countFollowAttempts = 0;
		while (IsAddressHooked(addressToHook))
		{
			if (IsRelativeNearJumpPresentAtAddress(addressToHook))
			{
				addressToHook = CalculateAbsoluteDestinationFromRelativeNearJumpAtAddress(addressToHook);
			}
			else if (IsAbsoluteIndirectNearJumpPresentAtAddress(addressToHook))
			{
				addressToHook = CalculateAbsoluteDestinationFromAbsoluteIndirectNearJumpAtAddress(addressToHook);
			}
			else if (IsAbsoluteDirectFarJumpPresentAtAddress(addressToHook))
			{
				//addressToHook = CalculateAbsoluteDestinationFromAbsoluteDirectFarJumpAtAddress(addressToHook);
			}

			countFollowAttempts++;
			if (countFollowAttempts >= maxFollowAttempts)
			{
				break;
			}
		}

		PrintBytesAtAddress(addressToHook, 20);

		const size_t assemblyShortJumpSize = 5;
		const size_t assemblyFarJumpSize = 14;
		size_t trampolineSize = 0;
		uintptr_t trampolineAddress = 0;
		uintptr_t trampolineReturnAddress = 0;
		size_t thirdPartyHookProtectionBuffer = assemblyFarJumpSize;

		size_t clearance = CalculateRequiredAsmClearance(addressToHook, assemblyShortJumpSize);

		trampolineSize = assemblyFarJumpSize * 3 + clearance + thirdPartyHookProtectionBuffer;

#ifdef _WIN64
		trampolineAddress = AllocateMemoryWithin32BitRange(trampolineSize, addressToHook + assemblyShortJumpSize);
#else
		trampolineAddress = AllocateMemory(trampolineSize);
#endif

		trampolineReturnAddress = addressToHook + clearance;
		MemCopy(trampolineAddress + assemblyFarJumpSize + thirdPartyHookProtectionBuffer, addressToHook, clearance);

		HookInformation hookInfo;
		hookInfo.originalBytes = std::vector<unsigned char>(clearance);
		hookInfo.trampolineInstructionsAddress = trampolineAddress + assemblyFarJumpSize + thirdPartyHookProtectionBuffer;
		InfoBufferForHookedAddresses[addressToHook] = hookInfo;
		MemCopy(
			(uintptr_t)&InfoBufferForHookedAddresses[addressToHook].originalBytes[0],
			trampolineAddress + assemblyFarJumpSize + thirdPartyHookProtectionBuffer,
			InfoBufferForHookedAddresses[addressToHook].originalBytes.size());
#ifdef _WIN64
		PlaceAbsoluteJump(trampolineAddress + thirdPartyHookProtectionBuffer, destinationAddress);
		PlaceAbsoluteJump(trampolineAddress + trampolineSize - assemblyFarJumpSize, trampolineReturnAddress);
#else
		PlaceRelativeJump(trampolineAddress + thirdPartyHookProtectionBuffer, destinationAddress);
		PlaceRelativeJump(trampolineAddress + trampolineSize - assemblyFarJumpSize, trampolineReturnAddress);
#endif
		*returnAddress = trampolineAddress + assemblyFarJumpSize + thirdPartyHookProtectionBuffer;
		PlaceRelativeJump(addressToHook, trampolineAddress, clearance);
	}

	static void Unhook(uintptr_t hookedAddress) 
	{
		auto search = InfoBufferForHookedAddresses.find(hookedAddress);
		if (search != InfoBufferForHookedAddresses.end())
		{
			MemSet(
				InfoBufferForHookedAddresses[hookedAddress].trampolineInstructionsAddress, 
				0x90, 
				InfoBufferForHookedAddresses[hookedAddress].originalBytes.size());
			MemCopy(
				hookedAddress, 
				(uintptr_t)&InfoBufferForHookedAddresses[hookedAddress].originalBytes[0], 
				InfoBufferForHookedAddresses[hookedAddress].originalBytes.size());
			logger.Log("Removed hook from %p", hookedAddress);
		}
	}

	static uintptr_t ReadPointerChain(std::vector<uintptr_t> pointerOffsets)
	{
		DWORD processId = GetCurrentProcessId();
		uintptr_t baseAddress = GetProcessBaseAddress(processId);
		uintptr_t pointer = baseAddress;
		for (size_t i = 0; i < pointerOffsets.size(); i++)
		{
			pointer += pointerOffsets[i];
			if (i != pointerOffsets.size() - 1)
			{
				MemCopy((uintptr_t)&pointer, pointer, sizeof(uintptr_t));
			}
			if (pointer == 0)
			{
				return 0;
			}
		}
		return pointer;
	}

	static uintptr_t GetPointerDMA(HANDLE hProc, uintptr_t baseAddr, std::vector<uintptr_t> offsets) {
		uintptr_t pointerDMA = NULL;
		size_t len = offsets.size();

		if (!ReadProcessMemory(hProc, (LPVOID)(baseAddr + offsets[0]), &pointerDMA, sizeof(pointerDMA), 0)) {
			DWORD error = GetLastError();
			//std::cerr << "Memory Read Error : " << error << std::endl;
			return NULL;
		}
		for (size_t i = 1; i < len - 1; i++) {
			if (!ReadProcessMemory(hProc, (LPVOID)(pointerDMA + offsets[i]), &pointerDMA, sizeof(pointerDMA), 0)) {
				DWORD error = GetLastError();
				//std::cerr << "Memory Read Error : " << error << std::endl;
				return NULL;
			}
		}

		pointerDMA += offsets[len - 1];
		return pointerDMA;
	}

	static bool IsReadablePointer(void* ptr, size_t size = 1) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
			return false;

		bool isCommitted = mbi.State == MEM_COMMIT;
		bool isReadable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
		bool isAccessible = !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS);

		return isCommitted && isReadable && isAccessible && ((uintptr_t)ptr + size <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize);
	}

	static std::vector<uint8_t> DumpMemory(const void* address, size_t size)
	{
		std::vector<uint8_t> buffer(size);
		if (!IsReadablePointer((void*)address, size)) {
			logger.Log("Memory at %p is not readable", address);
			return buffer;
		}

		std::memcpy(buffer.data(), address, size);
		return buffer;
	}

	static void DumpMemoryRecursive(void* basePtr, size_t size = 0x40, int depth = 0, int maxDepth = 2) {
		Logger logger("Memory Dump");

		if (!basePtr || depth > maxDepth) return;

		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(basePtr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)))
		{
			logger.Log("%*sMemory at %p not readable or not committed", depth * 2, "", basePtr);
			return;
		}

		uint8_t* data = reinterpret_cast<uint8_t*>(basePtr);

		for (size_t offset = 0; offset < size; offset += 4) {
			if (!IsReadablePointer(data + offset, 4)) {
				logger.Log("%*s[0x%02X] Not readable", depth * 2, "", (int)offset);
				continue;
			}

			uint32_t raw = *(uint32_t*)(data + offset);  // Direct read
			float asFloat = *reinterpret_cast<float*>(&raw);
			void* asPtr = reinterpret_cast<void*>(raw);

			logger.Log("%*s[0x%02X] Int: %10u | Float: %10.4f | Ptr: %p", depth * 2, "", (int)offset, raw, asFloat, asPtr);

			// Check for possible string
			if (raw > 0x10000 && raw < 0x7FFFFFFFFFFF && IsReadablePointer(asPtr, 8) && isprint(((char*)asPtr)[0])) {
				logger.Log("%*s↳ Possible string: \"%.8s\"", depth * 2, "", (char*)asPtr);
			}

			// Recursively dump
			if (depth + 1 <= maxDepth && raw > 0x10000 && raw < 0x7FFFFFFFFFFF) {
				MEMORY_BASIC_INFORMATION innerMbi{};
				if (VirtualQuery(asPtr, &innerMbi, sizeof(innerMbi)) &&
					innerMbi.State == MEM_COMMIT &&
					(innerMbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
					logger.Log("%*s↳ Recursing into pointer at offset 0x%02X", depth * 2, "", (int)offset);
					DumpMemoryRecursive(asPtr, size, depth + 1, maxDepth);
				}
			}
		}
	}

	static void DumpMemoryRecursive(
		void* basePtr,
		std::vector<size_t>& outOffsets,
		std::vector<uint32_t>& outValues,
		size_t size = 0x40,
		int depth = 0,
		int maxDepth = 2
	) {
		Logger logger("Memory Dump");

		if (!basePtr || depth > maxDepth) return;

		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(basePtr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
			logger.Log("%*sMemory at %p not readable or not committed", depth * 2, "", basePtr);
			return;
		}

		uint8_t* data = reinterpret_cast<uint8_t*>(basePtr);

		for (size_t offset = 0; offset < size; offset += 4) {
			if (!IsReadablePointer(data + offset, 4)) {
				logger.Log("%*s[0x%02X] Not readable", depth * 2, "", (int)offset);
				continue;
			}

			uint32_t raw = *(uint32_t*)(data + offset);
			float asFloat = *reinterpret_cast<float*>(&raw);
			void* asPtr = reinterpret_cast<void*>(raw);

			logger.Log("%*s[0x%02X] Int: %10u | Float: %10.4f | Ptr: %p", depth * 2, "", (int)offset, raw, asFloat, asPtr);

			// Store values in output only at top level (or modify logic if you want deep recursion saved too)
			if (depth == 0) {
				outOffsets.push_back(offset);
				outValues.push_back(raw);
			}

			if (raw > 0x10000 && raw < 0x7FFFFFFFFFFF && IsReadablePointer(asPtr, 8) && isprint(((char*)asPtr)[0])) {
				logger.Log("%*s↳ Possible string: \"%.8s\"", depth * 2, "", (char*)asPtr);
			}

			// Recursion
			if (depth + 1 <= maxDepth && raw > 0x10000 && raw < 0x7FFFFFFFFFFF) {
				MEMORY_BASIC_INFORMATION innerMbi{};
				if (VirtualQuery(asPtr, &innerMbi, sizeof(innerMbi)) &&
					innerMbi.State == MEM_COMMIT &&
					(innerMbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
					logger.Log("%*s↳ Recursing into pointer at offset 0x%02X", depth * 2, "", (int)offset);
					DumpMemoryRecursive(asPtr, outOffsets, outValues, size, depth + 1, maxDepth);
				}
			}
		}
	}

	static std::string FindCommonPattern(const std::vector<void*>& addresses, size_t length) {
		if (addresses.empty()) return "";
		Logger logger{ "Pattern Finder" };

		std::ostringstream oss;
		const uint8_t* base = reinterpret_cast<const uint8_t*>(addresses[0]);

		for (size_t i = 0; i < length; ++i) {
			bool allMatch = true;
			uint8_t byteVal = base[i];

			for (size_t j = 1; j < addresses.size(); ++j) {
				const uint8_t* other = reinterpret_cast<const uint8_t*>(addresses[j]);
				if (other[i] != byteVal) {
					allMatch = false;
					break;
				}
			}

			if (allMatch)
				oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(byteVal) << " ";
			else
				oss << "?? ";
		}

		std::string pattern = oss.str();
		logger.Log("Common pattern: %s", pattern.c_str());
		return pattern;
	}
}