
#include <format>
#include <cstdio>

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"

extern FILE* g_LogFile;

static void DebugLog(const char* msg)
{
	std::cerr << msg << std::endl;
	if (g_LogFile)
	{
		fputs(msg, g_LogFile);
		fputc('\n', g_LogFile);
		fflush(g_LogFile);
	}
}

uint8* NameArray::GNames = nullptr;

FNameEntry::FNameEntry(void* Ptr)
	: Address((uint8*)Ptr)
{
}

std::wstring FNameEntry::GetWString()
{
	if (!Address)
		return L"";

	return GetStr(Address);
}

std::string FNameEntry::GetString()
{
	if (!Address)
		return "";

	return UtfN::WStringToString(GetWString());
}

void* FNameEntry::GetAddress()
{
	return Address;
}

void FNameEntry::Init(const uint8_t* FirstChunkPtr, int64 NameEntryStringOffset)
{
	if (Settings::Internal::bUseNamePool)
	{
		constexpr int64 NoneStrLen = 0x4;
		constexpr uint16 BytePropertyStrLen = 0xC;

		constexpr uint32 BytePropertyStartAsUint32 = 'etyB'; // "Byte" part of "ByteProperty"

		Off::FNameEntry::NamePool::StringOffset = NameEntryStringOffset;
		Off::FNameEntry::NamePool::HeaderOffset = NameEntryStringOffset == 6 ? 4 : 0;

		const uint8* AssumedBytePropertyEntry = *reinterpret_cast<uint8* const*>(FirstChunkPtr) + NameEntryStringOffset + NoneStrLen;

		/* Check if there's pading after an FNameEntry. Check if there's up to 0x4 bytes padding. */
		for (int i = 0; i < 0x4; i++)
		{
			const uint32 FirstPartOfByteProperty = *reinterpret_cast<const uint32*>(AssumedBytePropertyEntry + NameEntryStringOffset);

			if (FirstPartOfByteProperty == BytePropertyStartAsUint32)
				break;

			AssumedBytePropertyEntry += 0x1;
		}

		uint16 BytePropertyHeader = *reinterpret_cast<const uint16*>(AssumedBytePropertyEntry + Off::FNameEntry::NamePool::HeaderOffset);

		/* Shifiting past the size of the header is not allowed, so limmit the shiftcount here */
		constexpr int32 MaxAllowedShiftCount = sizeof(BytePropertyHeader) * 0x8;

		while (BytePropertyHeader != BytePropertyStrLen && FNameEntryLengthShiftCount < MaxAllowedShiftCount)
		{			
			FNameEntryLengthShiftCount++;
			BytePropertyHeader >>= 1;
		}

		if (FNameEntryLengthShiftCount == MaxAllowedShiftCount)
		{
			std::cerr << "\nDumper-7: Error, couldn't get FNameEntryLengthShiftCount!\n" << std::endl;
			GetStr = [](uint8* NameEntry) -> std::wstring { return L"Invalid FNameEntryLengthShiftCount!"; };
			return;
		}

		GetStr = [](uint8* NameEntry) -> std::wstring
		{
			const uint16 HeaderWithoutNumber = *reinterpret_cast<uint16*>(NameEntry + Off::FNameEntry::NamePool::HeaderOffset);
			const int32 NameLen = HeaderWithoutNumber >> FNameEntry::FNameEntryLengthShiftCount;

			if (NameLen == 0)
			{
				const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);

				const int32 NextEntryIndex = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset);
				const int32 Number = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset + sizeof(int32));

				if (Number > 0)
					return NameArray::GetNameEntry(NextEntryIndex).GetWString() + L'_' + std::to_wstring(Number - 1);

				return NameArray::GetNameEntry(NextEntryIndex).GetWString();
			}

			if (HeaderWithoutNumber & NameWideMask)
				return std::wstring(reinterpret_cast<const wchar_t*>(NameEntry + Off::FNameEntry::NamePool::StringOffset), NameLen);

			return UtfN::StringToWString(std::string(reinterpret_cast<const char*>(NameEntry + Off::FNameEntry::NamePool::StringOffset), NameLen));
		};
	}
	else
	{
		const uint8_t* FNameEntryNone =     static_cast<uint8_t*>(NameArray::GetNameEntry(0x0).GetAddress());
		const uint8_t* FNameEntryIdxThree = static_cast<uint8_t*>(NameArray::GetNameEntry(0x3).GetAddress());
		const uint8_t* FNameEntryIdxEight = static_cast<uint8_t*>(NameArray::GetNameEntry(0x8).GetAddress());

		for (int i = 0; i < 0x20; i++)
		{
			if (*reinterpret_cast<const uint32*>(FNameEntryNone + i) == 'enoN') // None
			{
				Off::FNameEntry::NameArray::StringOffset = i;
				break;
			}
		}

		for (int i = 0; i < 0x20; i++)
		{
			// lowest bit is bIsWide mask, shift right by 1 to get the index
			if ((*reinterpret_cast<const uint32*>(FNameEntryIdxThree + i) >> 1) == 0x3 &&
				(*reinterpret_cast<const uint32*>(FNameEntryIdxEight + i) >> 1) == 0x8)
			{
				Off::FNameEntry::NameArray::IndexOffset = i;
				break;
			}
		}

		GetStr = [](uint8* NameEntry) -> std::wstring
		{
			const int32 NameIdx = *reinterpret_cast<int32*>(NameEntry + Off::FNameEntry::NameArray::IndexOffset);
			const void* NameString = reinterpret_cast<void*>(NameEntry + Off::FNameEntry::NameArray::StringOffset);

			if (NameIdx & NameWideMask)
				return std::wstring(reinterpret_cast<const wchar_t*>(NameString));

			return UtfN::StringToWString<std::string>(reinterpret_cast<const char*>(NameString));
		};
	}
}

bool NameArray::InitializeNameArray(uint8_t* NameArray)
{
	int32 ValidPtrCount = 0x0;
	int32 ZeroQWordCount = 0x0;

	int32 PerChunk = 0x0;

	if (!NameArray || Platform::IsBadReadPtr(NameArray))
		return false;

	for (int i = 0; i < 0x800; i += sizeof(void*))
	{
		uint8_t* SomePtr = *reinterpret_cast<uint8_t**>(NameArray + i);

		if (SomePtr == 0)
		{
			ZeroQWordCount++;
		}
		else if (ZeroQWordCount == 0x0 && SomePtr != nullptr)
		{
			ValidPtrCount++;
		}
		else if (ZeroQWordCount > 0 && SomePtr != 0)
		{
			int32 NumElements = *reinterpret_cast<int32_t*>(NameArray + i);
			int32 NumChunks = *reinterpret_cast<int32_t*>(NameArray + i + 4);

			if (NumChunks == ValidPtrCount)
			{
				Off::NameArray::NumElements = i;
				Off::NameArray::MaxChunkIndex = i + 4;

				ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
				{
					const int32 ChunkIdx = ComparisonIndex / 0x4000;
					const int32 InChunk = ComparisonIndex % 0x4000;

					if (ComparisonIndex >= NameArray::GetNumElements())
						return nullptr;

					return reinterpret_cast<void***>(NamesArray)[ChunkIdx][InChunk];
				};

				return true;
			}
		}
	}

	return false;
}

bool NameArray::InitializeNamePool(uint8_t* NamePool)
{
	Off::NameArray::MaxChunkIndex = 0x0;
	Off::NameArray::ByteCursor = 0x4;

	Off::NameArray::ChunksStart = 0x10;

	// Dump first 0x40 bytes of NamePool header for diagnostics
	{
		char hexLine[256];
		sprintf_s(hexLine, "  NamePool header dump (first 0x40 bytes at %p):", NamePool);
		DebugLog(hexLine);
		for (int row = 0; row < 4; row++)
		{
			int off = row * 16;
			sprintf_s(hexLine, "    +0x%02X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
				off,
				NamePool[off+0], NamePool[off+1], NamePool[off+2], NamePool[off+3],
				NamePool[off+4], NamePool[off+5], NamePool[off+6], NamePool[off+7],
				NamePool[off+8], NamePool[off+9], NamePool[off+10], NamePool[off+11],
				NamePool[off+12], NamePool[off+13], NamePool[off+14], NamePool[off+15]);
			DebugLog(hexLine);
		}
	}

	bool bWasMaxChunkIndexFound = false;

	for (int i = 0x0; i < 0x20; i += 4)
	{
		const int32 PossibleMaxChunkIdx = *reinterpret_cast<int32*>(NamePool + i);

		if (PossibleMaxChunkIdx <= 0 || PossibleMaxChunkIdx > 0x10000)
			continue;

		int32 NotNullptrCount = 0x0;
		bool bFoundFirstPtr = false;

		constexpr int32 MaxAllowedNumInvalidPtrs = 0x500;
		int32 NumPtrsSinceLastValid = 0x0;

		for (int j = 0x0; j < 0x10000; j += 8)
		{
			const int32 ChunkOffset = i + 8 + j + (i % 8);

			if ((*reinterpret_cast<uint8_t**>(NamePool + ChunkOffset)) != nullptr)
			{
				NotNullptrCount++;
				NumPtrsSinceLastValid = 0;

				if (!bFoundFirstPtr)
				{
					bFoundFirstPtr = true;
					Off::NameArray::ChunksStart = i + 8 + j + (i % 8);
				}
			}
			else
			{
				NumPtrsSinceLastValid++;

				if (NumPtrsSinceLastValid == MaxAllowedNumInvalidPtrs)
					break;
			}
		}

		{
			char buf[128];
			sprintf_s(buf, "  MaxChunkIdx candidate at +0x%X: value=%d, nonNullPtrs=%d (need %d)",
				i, PossibleMaxChunkIdx, NotNullptrCount, PossibleMaxChunkIdx + 1);
			DebugLog(buf);
		}

		if (PossibleMaxChunkIdx == (NotNullptrCount - 1))
		{
			Off::NameArray::MaxChunkIndex = i;
			Off::NameArray::ByteCursor = i + 4;
			bWasMaxChunkIndexFound = true;
			break;
		}
	}

	if (!bWasMaxChunkIndexFound)
	{
		DebugLog("  InitializeNamePool FAILED: MaxChunkIndex not found");
		return false;
	}

	{
		char buf[128];
		sprintf_s(buf, "  MaxChunkIndex found at +0x%X, ChunksStart at +0x%X",
			Off::NameArray::MaxChunkIndex, Off::NameArray::ChunksStart);
		DebugLog(buf);
	}

	constexpr uint64 CoreUObjAsUint64 = 0x6A624F5565726F43; // little endian "jbOUeroC" ["/Script/CoreUObject"]
	constexpr uint32 NoneAsUint32 = 0x656E6F4E; // little endian "None"

	uint8_t** ChunkPtr = reinterpret_cast<uint8_t**>(NamePool + Off::NameArray::ChunksStart);

	{
		char buf[128];
		sprintf_s(buf, "  First chunk ptr: %p", *ChunkPtr);
		DebugLog(buf);
	}

	// Dump first 64 bytes of first chunk for diagnostics
	if (*ChunkPtr && !Platform::IsBadReadPtr(*ChunkPtr))
	{
		char hexLine[256];
		DebugLog("  First chunk data (first 64 bytes):");
		for (int row = 0; row < 4; row++)
		{
			int off = row * 16;
			sprintf_s(hexLine, "    +0x%02X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
				off,
				(*ChunkPtr)[off+0], (*ChunkPtr)[off+1], (*ChunkPtr)[off+2], (*ChunkPtr)[off+3],
				(*ChunkPtr)[off+4], (*ChunkPtr)[off+5], (*ChunkPtr)[off+6], (*ChunkPtr)[off+7],
				(*ChunkPtr)[off+8], (*ChunkPtr)[off+9], (*ChunkPtr)[off+10], (*ChunkPtr)[off+11],
				(*ChunkPtr)[off+12], (*ChunkPtr)[off+13], (*ChunkPtr)[off+14], (*ChunkPtr)[off+15]);
			DebugLog(hexLine);
		}
	}

	bool bFoundCoreUObjectString = false;
	int64 FNameEntryHeaderSize = 0x0;

	constexpr int32 LoopLimit = 0x1000;

	for (int i = 0; i < LoopLimit; i++)
	{
		if (*reinterpret_cast<uint32*>(*ChunkPtr + i) == NoneAsUint32 && FNameEntryHeaderSize == 0)
		{
			FNameEntryHeaderSize = i;
			char buf[64];
			sprintf_s(buf, "  Found 'None' at chunk offset +0x%X (headerSize=%lld)", i, FNameEntryHeaderSize);
			DebugLog(buf);
		}
		else if (*reinterpret_cast<uint64*>(*ChunkPtr + i) == CoreUObjAsUint64)
		{
			bFoundCoreUObjectString = true;
			char buf[64];
			sprintf_s(buf, "  Found 'CoreUObject' at chunk offset +0x%X", i);
			DebugLog(buf);
			break;
		}
	}

	if (!bFoundCoreUObjectString)
	{
		DebugLog("  InitializeNamePool FAILED: CoreUObject string not found in first chunk");
		return false;
	}

	NameEntryStride = FNameEntryHeaderSize == 2 ? 2 : 4;
	Off::InSDK::NameArray::FNameEntryStride = NameEntryStride;

	ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
	{
		const int32 ChunkIdx = ComparisonIndex >> NamePoolBlockOffsetBits;
		const int32 InChunkOffset = (ComparisonIndex & ((1 << NamePoolBlockOffsetBits) - 1)) * NameEntryStride;

		const bool bIsBeyondLastChunk = ChunkIdx == NameArray::GetNumChunks() && InChunkOffset > NameArray::GetByteCursor();

		if (ChunkIdx < 0 || ChunkIdx >= GetNumChunks() || bIsBeyondLastChunk)
			return nullptr;

		uint8_t* ChunkPtr = reinterpret_cast<uint8_t*>(NamesArray) + 0x10;

		return reinterpret_cast<uint8_t**>(ChunkPtr)[ChunkIdx] + InChunkOffset;
	};

	Settings::Internal::bUseNamePool = true;
	FNameEntry::Init(reinterpret_cast<uint8*>(ChunkPtr), FNameEntryHeaderSize);

	return true;
}


/* 
 * Finds a call to FName::GetNames, OR a reference to GNames directly, if the call has been inlined
 * 
 * returns { GetNames/GNames, bIsGNamesDirectly };
*/
inline std::pair<uintptr_t, bool> FindFNameGetNamesOrGNames_Windows(const uintptr_t EnterCriticalSectionAddress, const uintptr_t StartAddress)
{
#ifdef PLATFORM_WINDOWS

	/* 2 bytes operation + 4 bytes relative offset */
	constexpr int32 ASMRelativeCallSizeBytes = 0x6;

	/* Range from "ByteProperty" which we want to search upwards for "GetNames" call */
	constexpr int32 GetNamesCallSearchRange = 0x150;

	/* Find a reference to the string "ByteProperty" in 'FName::StaticInit' */
	const uint8* BytePropertyStringAddress = static_cast<uint8*>(Platform::FindByStringInAllSections(L"ByteProperty", StartAddress, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings));

	/* Important to prevent infinite-recursion */
	if (!BytePropertyStringAddress)
		return { 0x0, false };

	for (int i = 0; i < GetNamesCallSearchRange; i++)
	{
		/* Check upwards (yes negative indexing) for a relative call opcode */
		if (BytePropertyStringAddress[-i] != 0xFF)
			continue;

#if defined(_WIN64)
		const uintptr_t CallTarget = Architecture_x86_64::Resolve32BitSectionRelativeCall(reinterpret_cast<uintptr_t>(BytePropertyStringAddress - i));
#elif defined(_WIN32)
		uintptr_t CallTarget = Architecture_x86_64::Resolve32bitAbsoluteCall(reinterpret_cast<uintptr_t>(BytePropertyStringAddress - i));
#endif

		if (CallTarget != EnterCriticalSectionAddress)
			continue;

		const uintptr_t InstructionAfterCall = reinterpret_cast<uintptr_t>(BytePropertyStringAddress - (i - ASMRelativeCallSizeBytes));
		
		/* Check if we're dealing with a 'call' opcode */
		if (*reinterpret_cast<const uint8*>(InstructionAfterCall) == 0xE8)
			return { Architecture_x86_64::Resolve32BitRelativeCall(InstructionAfterCall), false };

		// Looks like on 32bit like literally everything is absolute???? fuck you
#if defined(_WIN64)
		return { Architecture_x86_64::Resolve32BitRelativeMove(InstructionAfterCall), true };
#elif defined(_WIN32)
		return { Architecture_x86_64::Resolve32bitAbsoluteMove(InstructionAfterCall), true };
#endif
	}

	/* Continue and search for another reference to "ByteProperty", safe because we're checking if another string-ref was found*/
	return FindFNameGetNamesOrGNames_Windows(EnterCriticalSectionAddress, reinterpret_cast<uintptr_t>(BytePropertyStringAddress) + ASMRelativeCallSizeBytes);

#endif // PLATFORM_WINDOWS
};

bool NameArray::TryFindNameArray_Windows()
{
#ifdef PLATFORM_WINDOWS

	/* Type of 'static TNameEntryArray& FName::GetNames()' */
	using GetNameType = void* (*)();

	/* Range from 'FName::GetNames' which we want to search down for 'mov register, GNames' */
	constexpr int32 GetNamesCallSearchRange = 0x100;

	const void* EnterCriticalSectionAddress = Platform::GetAddressOfImportedFunctionFromAnyModule("kernel32.dll", "EnterCriticalSection");

	auto [Address, bIsGNamesDirectly] = FindFNameGetNamesOrGNames_Windows(reinterpret_cast<uintptr_t>(EnterCriticalSectionAddress), Platform::GetModuleBase());

	if (Address == 0x0)
		return false;

	if (bIsGNamesDirectly)
	{
		if (!Platform::IsAddressInProcessRange(Address) || Platform::IsBadReadPtr(*reinterpret_cast<void**>(Address)))
			return false;

		Off::InSDK::NameArray::GNames = Platform::GetOffset(Address);
		return true;
	}

	// TODO (encryqed): Fix below for 32-bit ue shit 

	/* Call GetNames to retreive the pointer to the allocation of the name-table, used for later comparison */
	void* Names = reinterpret_cast<GetNameType>(Address)();

	for (int i = 0; i < GetNamesCallSearchRange; i++)
	{
		/* Check upwards (yes negative indexing) for a relative call opcode */
		if (*reinterpret_cast<const uint16*>(Address + i) != 0x8B48)
			continue;

		const uintptr_t MoveTarget = Architecture_x86_64::Resolve32BitRelativeMove(Address + i);

		if (!Platform::IsAddressInProcessRange(MoveTarget))
			continue;

		const void* ValueOfMoveTargetAsPtr = *reinterpret_cast<void**>(MoveTarget);

		if (Platform::IsBadReadPtr(ValueOfMoveTargetAsPtr) || ValueOfMoveTargetAsPtr != Names)
			continue;

		Off::InSDK::NameArray::GNames = Platform::GetOffset(MoveTarget);
		return true;
	}
	
	return false;

#endif // PLATFORM_WINDOWS
}

bool NameArray::TryFindNamePool_Windows()
{
#ifdef PLATFORM_WINDOWS

	// TODO (encryqed): Fix this below for 32-bit ue games ig?

	/* Number of bytes we want to search for an indirect call to InitializeSRWLock */
	constexpr int32 InitSRWLockSearchRange = 0x50;

	/* Number of bytes we want to search for lea instruction loading the string "ByteProperty" */
	constexpr int32 BytePropertySearchRange = 0x2A0;

	/* FNamePool::FNamePool contains a call to InitializeSRWLock or RtlInitializeSRWLock, we're going to check for that later */
	const uintptr_t InitSRWLockAddress = reinterpret_cast<uintptr_t>(Platform::GetAddressOfImportedFunctionFromAnyModule("kernel32.dll", "InitializeSRWLock"));
	const uintptr_t RtlInitSRWLockAddress = reinterpret_cast<uintptr_t>(Platform::GetAddressOfImportedFunctionFromAnyModule("ntdll.dll", "RtlInitializeSRWLock"));

	/* Singleton instance of FNamePool, which is passed as a parameter to FNamePool::FNamePool */
	void* NamePoolIntance = nullptr;

	uintptr_t SigOccurrence = 0x0;;

	uintptr_t Counter = 0x0;

	while (!NamePoolIntance)
	{
		/* add 0x1 so we don't find the same occurence again and cause an infinite loop (20min. of debugging for that) */
		if (SigOccurrence > 0x0)
			SigOccurrence += 0x1;

		/* Find the next occurence of this signature to see if that may be a call to the FNamePool constructor */
		SigOccurrence = reinterpret_cast<uintptr_t>(Platform::FindPattern("48 8D 0D ? ? ? ? E8", 0x0, true, SigOccurrence));

		if (SigOccurrence == 0x0)
			break;

		constexpr int32 SizeOfMovInstructionBytes = 0x7;

		const uintptr_t PossibleConstructorAddress = Architecture_x86_64::Resolve32BitRelativeCall(SigOccurrence + SizeOfMovInstructionBytes);

		if (!Platform::IsAddressInProcessRange(PossibleConstructorAddress))
			continue;

		for (int i = 0; i < InitSRWLockSearchRange; i++)
		{
			/* Check for a relative call with the opcodes FF 15 00 00 00 00 */
			if (*reinterpret_cast<uint16*>(PossibleConstructorAddress + i) != 0x15FF)
				continue;

			const uintptr_t RelativeCallTarget = Architecture_x86_64::Resolve32BitSectionRelativeCall(PossibleConstructorAddress + i);

			if (!Platform::IsAddressInProcessRange(RelativeCallTarget))
				continue;

			const uintptr_t ValueOfCallTarget = *reinterpret_cast<uintptr_t*>(RelativeCallTarget);

			if (ValueOfCallTarget != InitSRWLockAddress && ValueOfCallTarget != RtlInitSRWLockAddress)
				continue;

			/* Try to find the "ByteProperty" string, as it's always referenced in FNamePool::FNamePool, so we use it to verify that we got the right function */
			const void* StringRef = Platform::FindByStringInAllSections(L"ByteProperty", PossibleConstructorAddress, BytePropertySearchRange, Settings::General::bSearchOnlyExecutableSectionsForStrings);

			/* We couldn't find a wchar_t string L"ByteProperty", now see if we can find a char string "ByteProperty" */
			if (StringRef == nullptr)
				StringRef = Platform::FindByStringInAllSections("ByteProperty", PossibleConstructorAddress, BytePropertySearchRange, Settings::General::bSearchOnlyExecutableSectionsForStrings);

			if (StringRef)
			{
				NamePoolIntance = reinterpret_cast<void*>(Architecture_x86_64::Resolve32BitRelativeMove(SigOccurrence));
				break;
			}
		}
	}

	if (NamePoolIntance)
	{
		Off::InSDK::NameArray::GNames = Platform::GetOffset(NamePoolIntance);
		return true;
	}

	return false;

#endif // PLATFORM_WINDOWS
}

/*
 * UE 5.8: FNamePool::FNamePool no longer calls InitializeSRWLock (the FRWLock is zero-initialized with a plain store) and every
 * inlined GetNamePool() lazy-init site looks like 'lea rcx, NamePoolData; <spill>; jne skip; call FNamePool::FNamePool'. Because of
 * that neither the InitializeSRWLock check in TryFindNamePool_Windows nor the adjacent 'lea rcx; call' signature match anymore
 * (observed on ARK: Survival Ascended v97 / UE 5.8).
 *
 * This finder follows the chain that is still visible in the binary:
 *   1. find the constructor body through its reference to the hardcoded "ByteProperty" name, which directly follows a "None" reference
 *   2. resolve the constructor's start address through the exception directory (RtlLookupFunctionEntry, following chained unwind info)
 *   3. find every 'call FNamePool::FNamePool' in .text and take the 'lea rcx, [rip+X]' shortly before the call as NamePoolData
 *   4. validate the candidate: CurrentBlock must be sane and Blocks[0] must start with the "None" entry
 */
bool NameArray::TryFindNamePoolCtorXref_Windows()
{
#ifdef PLATFORM_WINDOWS

	/* The constructor registers the hardcoded names in EName order, so the "None" reference sits right before the "ByteProperty" one */
	constexpr int32 NoneRefSearchRangeBefore = 0x80;

	/* Max distance between 'lea rcx, NamePoolData' and 'call FNamePool::FNamePool' at an inlined lazy-init site */
	constexpr int32 LeaSearchRangeBeforeCall = 0x30;

	auto LooksLikeNamePool = [](const uint8* Pool) -> bool
	{
		if (Platform::IsBadReadPtr(Pool) || Platform::IsBadReadPtr(Pool + 0x18))
			return false;

		/* FNameEntryAllocator { FRWLock Lock; uint32 CurrentBlock; uint32 CurrentByteCursor; uint8* Blocks[FNameMaxBlocks]; } */
		const int32 CurrentBlock = *reinterpret_cast<const int32*>(Pool + 0x8);

		if (CurrentBlock < 0 || CurrentBlock > 0x2000)
			return false;

		const uint8* FirstBlock = *reinterpret_cast<const uint8* const*>(Pool + 0x10);

		if (!FirstBlock || Platform::IsBadReadPtr(FirstBlock) || Platform::IsBadReadPtr(FirstBlock + 0x10))
			return false;

		/* FNameEntryHeader { uint16 bIsWide : 1; uint16 LowercaseProbeHash : 5; uint16 Len : 10; } followed by the unterminated chars. Entry 0 is always "None". */
		const uint16 Header = *reinterpret_cast<const uint16*>(FirstBlock);

		if ((Header & 0x1) == 0 && (Header >> 6) == 4 && memcmp(FirstBlock + 2, "None", 4) == 0)
			return true;

		/* Case-preserving builds store a 4-byte comparison id in front of the header */
		const uint16 HeaderCasePreserving = *reinterpret_cast<const uint16*>(FirstBlock + 4);

		return (HeaderCasePreserving & 0x1) == 0 && (HeaderCasePreserving >> 6) == 4 && memcmp(FirstBlock + 6, "None", 4) == 0;
	};

	/* 1. Find the 'lea reg, "ByteProperty"' inside the constructor. UE5 registers the hardcoded names as ANSI strings, older builds used wide strings. */
	auto FindCtorBodyRef = [&]<typename CharType>(const CharType* BytePropertyStr, const CharType* NoneStr) -> uintptr_t
	{
		uintptr_t SearchStart = 0x0;

		for (int i = 0; i < 0x20; i++)
		{
			const void* Ref = Platform::FindByStringInAllSections<false, CharType>(BytePropertyStr, SearchStart, 0x0, true);

			if (!Ref)
				return 0x0;

			const uintptr_t RefAddr = reinterpret_cast<uintptr_t>(Ref);

			const void* NoneRef = Platform::FindByStringInAllSections<false, CharType>(NoneStr, RefAddr - NoneRefSearchRangeBefore, NoneRefSearchRangeBefore, true);
			const uintptr_t NoneRefAddr = reinterpret_cast<uintptr_t>(NoneRef);

			if (NoneRef && NoneRefAddr >= (RefAddr - NoneRefSearchRangeBefore) && NoneRefAddr < RefAddr)
				return RefAddr;

			SearchStart = RefAddr + 0x1;
		}

		return 0x0;
	};

	uintptr_t CtorBodyRef = FindCtorBodyRef("ByteProperty", "None");

	if (CtorBodyRef == 0x0)
		CtorBodyRef = FindCtorBodyRef(L"ByteProperty", L"None");

	if (CtorBodyRef == 0x0)
	{
		DebugLog("  CtorXref: no 'None' + 'ByteProperty' reference pair found");
		return false;
	}

	/* 2. Resolve the start of the function containing the reference, following chained unwind infos up to the primary entry */
	DWORD64 ImageBase = 0x0;
	PRUNTIME_FUNCTION Entry = RtlLookupFunctionEntry(static_cast<DWORD64>(CtorBodyRef), &ImageBase, nullptr);

	if (!Entry)
	{
		DebugLog("  CtorXref: RtlLookupFunctionEntry failed for the constructor body");
		return false;
	}

	for (int Depth = 0; Depth < 8; Depth++)
	{
		const uint8* UnwindInfo = reinterpret_cast<const uint8*>(ImageBase + Entry->UnwindInfoAddress);
		const uint8 Flags = UnwindInfo[0] >> 3;

		constexpr uint8 UnwFlagChainInfo = 0x4;

		if (!(Flags & UnwFlagChainInfo))
			break;

		const uint8 CountOfCodes = UnwindInfo[2];
		Entry = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8*>(UnwindInfo + 4 + ((CountOfCodes + 1) & ~1) * 2));
	}

	const uintptr_t CtorAddress = static_cast<uintptr_t>(ImageBase + Entry->BeginAddress);

	{
		char buf[160];
		sprintf_s(buf, "  CtorXref: 'ByteProperty' ref at offset 0x%llX, FNamePool::FNamePool at offset 0x%llX",
			static_cast<unsigned long long>(Platform::GetOffset(CtorBodyRef)), static_cast<unsigned long long>(Platform::GetOffset(CtorAddress)));
		DebugLog(buf);
	}

	/* 3. Walk every call to the constructor and look for the 'lea rcx, [rip+disp32]' that loads NamePoolData right before it */
	const SectionInfo TextInfo = Platform::GetSectionInfo(".text");

	if (!TextInfo.IsValid())
	{
		DebugLog("  CtorXref: .text section not found");
		return false;
	}

	uintptr_t TextStart = 0x0;

	uintptr_t FoundPool = 0x0;
	int32 NumCallSites = 0x0;

	auto CheckCallSite = [&](void* Address) -> bool
	{
		const uint8* Bytes = static_cast<const uint8*>(Address);

		if (TextStart == 0x0)
			TextStart = reinterpret_cast<uintptr_t>(Address);

		if (Bytes[0] != 0xE8)
			return false;

		if (Architecture_x86_64::Resolve32BitRelativeCall(reinterpret_cast<uintptr_t>(Address)) != CtorAddress)
			return false;

		NumCallSites++;

		if (reinterpret_cast<uintptr_t>(Address) < (TextStart + LeaSearchRangeBeforeCall))
			return false;

		for (int i = 0x7; i <= LeaSearchRangeBeforeCall; i++)
		{
			const uint8* Lea = Bytes - i;

			/* lea rcx, [rip+disp32] */
			if (Lea[0] != 0x48 || Lea[1] != 0x8D || Lea[2] != 0x0D)
				continue;

			const uintptr_t Target = Architecture_x86_64::Resolve32BitRelativeLea(reinterpret_cast<uintptr_t>(Lea));

			if (!Platform::IsAddressInWritableSection(Target))
				continue;

			if (!LooksLikeNamePool(reinterpret_cast<const uint8*>(Target)))
				continue;

			FoundPool = Target;
			return true;
		}

		return false;
	};

	Platform::IterateSectionWithCallback(TextInfo, CheckCallSite, 0x1, 0x40);

	if (FoundPool == 0x0)
	{
		char buf[128];
		sprintf_s(buf, "  CtorXref: %d constructor call sites, none with a valid 'lea rcx, NamePoolData'", NumCallSites);
		DebugLog(buf);
		return false;
	}

	Off::InSDK::NameArray::GNames = Platform::GetOffset(FoundPool);
	return true;

#endif // PLATFORM_WINDOWS
}

/*
 * Finds GFNamePool by scanning FName::AppendString's prologue for the lazy-init pattern
 *
 * UE 5.4.4+ inlines the NamePool init at every AppendString site, producing a 'lea r8, GFNamePool' /
 * 'lea rcx, GFNamePool' pair within a few bytes of each other. TryFindNamePool_Windows can't see
 * GFNamePool because modern UE has no standalone FNamePool::FNamePool constructor matching its
 * lea+call signature.
*/
bool NameArray::TryFindNamePoolLazyInit_Windows()
{
#ifdef PLATFORM_WINDOWS

	if (Off::InSDK::Name::AppendNameToString == 0x0)
		return false;

	const uintptr_t AppendStringAddress = Platform::GetModuleBase() + Off::InSDK::Name::AppendNameToString;

	constexpr int32 ScanRange = 0x300;
	constexpr int32 PairSearchWindow = 0x60;

	const uint8* Begin = reinterpret_cast<const uint8*>(AppendStringAddress);
	const uint8* End = Begin + ScanRange - PairSearchWindow;

	auto IsLeaToWritable = [](const uint8* Ptr, uintptr_t& OutTarget) -> bool
	{
		// lea rcx, [rip+disp32] = 48 8D 0D
		// lea rdx, [rip+disp32] = 48 8D 15
		// lea rax, [rip+disp32] = 48 8D 05
		// lea r8,  [rip+disp32] = 4C 8D 05
		// lea r9,  [rip+disp32] = 4C 8D 0D
		const bool bIs48Lea = Ptr[0] == 0x48 && Ptr[1] == 0x8D && (Ptr[2] == 0x0D || Ptr[2] == 0x15 || Ptr[2] == 0x05);
		const bool bIs4CLea = Ptr[0] == 0x4C && Ptr[1] == 0x8D && (Ptr[2] == 0x05 || Ptr[2] == 0x0D);

		if (!bIs48Lea && !bIs4CLea)
			return false;

		OutTarget = Architecture_x86_64::Resolve32BitRelativeLea(reinterpret_cast<uintptr_t>(Ptr));
		return Platform::IsAddressInWritableSection(OutTarget);
	};

	// First pass: look for a pair of lea instructions loading the same writable address
	for (const uint8* Bytes = Begin; Bytes < End; Bytes++)
	{
		uintptr_t TargetA = 0;
		if (!IsLeaToWritable(Bytes, TargetA))
			continue;

		for (int i = 0x7; i < PairSearchWindow; i++)
		{
			uintptr_t TargetB = 0;
			if (!IsLeaToWritable(Bytes + i, TargetB))
				continue;

			if (TargetB != TargetA)
				continue;

			Off::InSDK::NameArray::GNames = Platform::GetOffset(TargetA);
			return true;
		}
	}

	// Second pass: single lea fallback — validate as FNamePool by checking structure
	const uint8* FallbackEnd = Begin + ScanRange;
	uintptr_t BestCandidate = 0;

	for (const uint8* Bytes = Begin; Bytes < FallbackEnd; Bytes++)
	{
		uintptr_t Target = 0;
		if (!IsLeaToWritable(Bytes, Target))
			continue;

		const uint8_t* Pool = reinterpret_cast<const uint8_t*>(Target);
		if (Platform::IsBadReadPtr(Pool) || Platform::IsBadReadPtr(Pool + 0x100))
			continue;

		// Check if it looks like a FNamePool: first 0x20 bytes should contain a small
		// positive int (MaxChunkIndex) and further in should have valid pointers (chunk array)
		bool bHasSmallInt = false;
		for (int j = 0; j < 0x20; j += 4)
		{
			int32 Val = *reinterpret_cast<const int32*>(Pool + j);
			if (Val > 0 && Val < 0x10000)
			{
				bHasSmallInt = true;
				break;
			}
		}

		if (!bHasSmallInt)
			continue;

		// Check for valid pointers in the chunk region (after the header)
		int32 ValidPtrCount = 0;
		for (int j = 0x10; j < 0x100; j += 8)
		{
			const void* Ptr = *reinterpret_cast<void* const*>(Pool + j);
			if (Ptr && !Platform::IsBadReadPtr(Ptr))
				ValidPtrCount++;
		}

		if (ValidPtrCount >= 2)
		{
			BestCandidate = Target;
			break;
		}
	}

	if (BestCandidate)
	{
		Off::InSDK::NameArray::GNames = Platform::GetOffset(BestCandidate);
		return true;
	}

	return false;

#endif // PLATFORM_WINDOWS
}

bool NameArray::TryFindNamePoolBruteForce_Windows()
{
#ifdef PLATFORM_WINDOWS

	// Phase 0: If AppendString offset is known, scan its code body for ALL lea
	// register encodings pointing to writable sections. TryFindNamePoolLazyInit
	// only checks rax/rcx/rdx/r8/r9 — UE 5.8 may use any register.
	if (Off::InSDK::Name::AppendNameToString != 0)
	{
		DebugLog("  Brute-force Phase 0: scanning AppendString for FNamePool lea/mov...");
		const uintptr_t AppendAddr = Platform::GetModuleBase() + Off::InSDK::Name::AppendNameToString;
		const uint8_t* code = reinterpret_cast<const uint8_t*>(AppendAddr);

		// Dump first 48 bytes of AppendString for disassembly reference
		{
			DebugLog("  AppendString code (first 48 bytes):");
			char hexLine[256];
			for (int row = 0; row < 3; row++)
			{
				int off = row * 16;
				sprintf_s(hexLine, "    +0x%02X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
					off,
					code[off+0], code[off+1], code[off+2], code[off+3],
					code[off+4], code[off+5], code[off+6], code[off+7],
					code[off+8], code[off+9], code[off+10], code[off+11],
					code[off+12], code[off+13], code[off+14], code[off+15]);
				DebugLog(hexLine);
			}
		}

		constexpr int ScanBytes = 0x800;
		int instrCount = 0;

		for (int i = 0; i < ScanBytes - 7; i++)
		{
			// lea/mov reg, [rip+disp32]: REX.W (0x48 or 0x4C) + opcode + ModRM
			// 0x8D = lea (loads address), 0x8B = mov (loads value at address)
			// ModRM & 0xC7 == 0x05 means mod=00, rm=101 (RIP-relative), any reg
			if (code[i] != 0x48 && code[i] != 0x4C)
				continue;
			const bool isLea = (code[i + 1] == 0x8D);
			const bool isMov = (code[i + 1] == 0x8B);
			if (!isLea && !isMov)
				continue;
			if ((code[i + 2] & 0xC7) != 0x05)
				continue;

			uintptr_t ripTarget = Architecture_x86_64::Resolve32BitRelativeLea(
				reinterpret_cast<uintptr_t>(&code[i]));

			uintptr_t target;
			if (isLea)
			{
				target = ripTarget;
				if (!Platform::IsAddressInWritableSection(target))
					continue;
				if (Platform::IsBadReadPtr(reinterpret_cast<const void*>(target)))
					continue;
			}
			else
			{
				if (Platform::IsBadReadPtr(reinterpret_cast<const void*>(ripTarget)))
					continue;
				target = *reinterpret_cast<const uintptr_t*>(ripTarget);
				if (target < 0x10000 || target > 0x7FFFFFFFFFFF)
					continue;
				if (Platform::IsBadReadPtr(reinterpret_cast<const void*>(target)))
					continue;
			}

			instrCount++;
			{
				char buf[256];
				if (isMov)
					sprintf_s(buf, "    mov #%d at AppendString+0x%X: [0x%llX] -> 0x%llX",
						instrCount, i, (unsigned long long)ripTarget, (unsigned long long)target);
				else
					sprintf_s(buf, "    lea #%d at AppendString+0x%X -> 0x%llX",
						instrCount, i, (unsigned long long)target);
				DebugLog(buf);
			}

			// Try the exact lea target first
			if (InitializeNamePool(reinterpret_cast<uint8_t*>(target)))
			{
				DebugLog("    SUCCESS! FNamePool found at exact lea target");
				Off::InSDK::NameArray::GNames = Platform::GetOffset(target);
				return true;
			}

			// UE 5.8: the lea may point to the pool's Lock/header, with actual
			// data (MaxChunkIndex, Blocks[]) at a larger offset due to sharding/alignment.
			constexpr uint32 NoneVal = 0x656E6F4E;
			constexpr uint32 ByteVal = 0x65747942;
			constexpr uint32 PropVal = 0x706F7250;

			const uint8_t* leaBase = reinterpret_cast<const uint8_t*>(target);

			// Diagnostic: find how far the zeros extend from the lea target
			int firstNonZero = -1;
			for (int off = 0; off < 0x4000; off += 8)
			{
				if (Platform::IsBadReadPtr(leaBase + off)) { firstNonZero = -2; break; }
				if (*reinterpret_cast<const uint64_t*>(leaBase + off) != 0)
				{
					firstNonZero = off;
					break;
				}
			}
			{
				char buf2[256];
				if (firstNonZero == -1)
					sprintf_s(buf2, "    First 0x4000 bytes from lea are ALL zeros — lea is NOT the FNamePool");
				else if (firstNonZero == -2)
					sprintf_s(buf2, "    Hit unreadable memory scanning from lea target");
				else
					sprintf_s(buf2, "    First non-zero at lea+0x%X = 0x%llX",
						firstNonZero, (unsigned long long)*reinterpret_cast<const uint64_t*>(leaBase + firstNonZero));
				DebugLog(buf2);
			}

			// Scan up to 0x4000 bytes from lea target for heap pointers to "None" chunk
			DebugLog("    Scanning lea target region for Blocks[0] pointer...");
			int scanEnd = (firstNonZero > 0) ? firstNonZero + 0x800 : 0x4000;
			if (scanEnd > 0x4000) scanEnd = 0x4000;

			for (int off = (firstNonZero > 0 ? firstNonZero : 0); off < scanEnd; off += 8)
			{
				if (Platform::IsBadReadPtr(leaBase + off)) break;
				uintptr_t ptrVal = *reinterpret_cast<const uintptr_t*>(leaBase + off);
				if (ptrVal < 0x10000 || ptrVal > 0x7FFFFFFFFFFF) continue;

				const uint8_t* chunk = reinterpret_cast<const uint8_t*>(ptrVal);
				if (Platform::IsBadReadPtr(chunk) || Platform::IsBadReadPtr(chunk + 0x30))
					continue;

				for (int hdr = 2; hdr <= 6; hdr += 2)
				{
					if (*reinterpret_cast<const uint32_t*>(chunk + hdr) != NoneVal)
						continue;

					bool hasBP = false;
					for (int k = hdr + 5; k < hdr + 0x30; k++)
					{
						if (*reinterpret_cast<const uint32_t*>(chunk + k) == ByteVal
							&& *reinterpret_cast<const uint32_t*>(chunk + k + 4) == PropVal)
						{ hasBP = true; break; }
					}
					if (!hasBP) continue;

					char buf2[256];
					sprintf_s(buf2, "    Found Blocks[0] at lea+0x%X -> chunk with 'None' (hdrOff=%d)", off, hdr);
					DebugLog(buf2);

					// Try InitializeNamePool at offsets back from Blocks[0]
					for (int hs = off; hs >= 0 && hs >= off - 0x100; hs -= 8)
					{
						const uint8_t* pool = leaBase + hs;
						if (hs != off && Platform::IsBadReadPtr(pool)) continue;

						if (InitializeNamePool(const_cast<uint8_t*>(pool)))
						{
							sprintf_s(buf2, "    SUCCESS! FNamePool at lea+0x%X (Blocks[0] at lea+0x%X)", hs, off);
							DebugLog(buf2);
							Off::InSDK::NameArray::GNames = Platform::GetOffset(pool);
							return true;
						}
					}
					goto nextInstr;
				}
			}
			nextInstr:;
		}

		{
			char buf[128];
			sprintf_s(buf, "  Phase 0 done: checked %d lea/mov targets, none valid", instrCount);
			DebugLog(buf);
		}
	}
	else
	{
		DebugLog("  Phase 0 skipped: AppendString offset not known");
	}

	// Phase 0.5: Scan ALL committed memory (including MEM_IMAGE) for "None\0".
	// Phase 1 skips MEM_IMAGE and requires "ByteProperty" nearby — this is a
	// broader diagnostic to find WHERE "None" actually lives in this process.
	DebugLog("  Phase 0.5: scanning ALL memory for 'None\\0' (incl. MEM_IMAGE)...");
	{
		MEMORY_BASIC_INFORMATION mbi05;
		uintptr_t addr05 = 0x10000;
		int noneMatches = 0;
		int regions05 = 0;
		constexpr int MaxNoneMatches = 30;

		while (noneMatches < MaxNoneMatches &&
			VirtualQuery(reinterpret_cast<const void*>(addr05), &mbi05, sizeof(mbi05)) == sizeof(mbi05))
		{
			const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi05.BaseAddress);
			addr05 = regionBase + mbi05.RegionSize;
			if (addr05 <= regionBase) break;

			if (mbi05.State != MEM_COMMIT) continue;
			if (mbi05.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
			if (!(mbi05.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
				PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) continue;
			if (mbi05.RegionSize < 0x100) continue;

			regions05++;
			if (regions05 % 5000 == 0)
			{
				char pbuf[128];
				sprintf_s(pbuf, "    ... Phase 0.5: %d regions, %d matches so far", regions05, noneMatches);
				DebugLog(pbuf);
			}

			const uint8_t* base = reinterpret_cast<const uint8_t*>(regionBase);
			const size_t regionSize = mbi05.RegionSize;
			// MEM_IMAGE: scan entire region (names may be in .data section)
			// Other: scan first 256KB per region
			const size_t scanLimit = (mbi05.Type == MEM_IMAGE)
				? regionSize
				: ((regionSize < 0x40000) ? regionSize : 0x40000);

			for (size_t off = 0; off + 5 <= scanLimit; off++)
			{
				if (*reinterpret_cast<const uint32_t*>(base + off) != 0x656E6F4E)
					continue;
				if (base[off + 4] != 0x00)
					continue;

				noneMatches++;
				uintptr_t matchAddr = regionBase + off;

				char buf[256];
				sprintf_s(buf, "    #%d: 'None\\0' at 0x%llX (%s, rgn 0x%llX sz 0x%llX)",
					noneMatches, (unsigned long long)matchAddr,
					(mbi05.Type == MEM_IMAGE) ? "IMAGE" :
					(mbi05.Type == MEM_PRIVATE) ? "PRIVATE" : "MAPPED",
					(unsigned long long)regionBase, (unsigned long long)regionSize);
				DebugLog(buf);

				// Dump 8 bytes before + 24 bytes from match for context
				int ctxBefore = (off >= 8) ? 8 : (int)off;
				char hex[300];
				int pos = sprintf_s(hex, "      ");
				for (int b = 0; b < 32 && (off - ctxBefore + b) < regionSize; b++)
					pos += sprintf_s(hex + pos, sizeof(hex) - pos, "%02X ", base[off - ctxBefore + b]);
				DebugLog(hex);

				if (noneMatches >= MaxNoneMatches) break;
			}
		}

		char buf[128];
		sprintf_s(buf, "  Phase 0.5 done: %d regions, %d 'None\\0' matches", regions05, noneMatches);
		DebugLog(buf);
	}

	// Phase 1: VirtualQuery scan — search all committed readable memory for the
	// "None\0" + "ByteProperty" byte pattern, then find pointer in .data.
	DebugLog("  Brute-force Phase 1: scanning process memory for FName chunk pattern...");

	constexpr uint32 NoneAsUint32 = 0x656E6F4E;
	constexpr uint32 ByteAsUint32 = 0x65747942;
	constexpr uint32 PropAsUint32 = 0x706F7250;

	SectionInfo DataInfo = Platform::GetSectionInfo(".data");
	if (!DataInfo.IsValid())
	{
		DebugLog("  .data section not found");
		return false;
	}

	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t scanAddr = 0x10000;
	int regionsScanned = 0;
	int candidatesFound = 0;

	while (VirtualQuery(reinterpret_cast<const void*>(scanAddr), &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		scanAddr = regionBase + mbi.RegionSize;
		if (scanAddr <= regionBase) break;

		if (mbi.State != MEM_COMMIT) continue;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
		if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) continue;
		if (mbi.RegionSize < 0x1000) continue;
		if (mbi.Type == MEM_IMAGE) continue; // skip PE images — chunks are heap-allocated

		regionsScanned++;
		if (regionsScanned % 5000 == 0)
		{
			char buf[128];
			sprintf_s(buf, "    ... %d regions scanned so far", regionsScanned);
			DebugLog(buf);
		}

		const uint8_t* base = reinterpret_cast<const uint8_t*>(regionBase);
		const size_t regionSize = mbi.RegionSize;

		// Limit scan to first 64KB per region — chunk data starts near allocation base
		const size_t scanLimit = (regionSize < 0x10000) ? regionSize : 0x10000;

		for (size_t offset = 0; offset + 0x40 < scanLimit; offset += 2)
		{
			if (*reinterpret_cast<const uint32_t*>(base + offset) != NoneAsUint32)
				continue;
			if (base[offset + 4] != 0 && (*reinterpret_cast<const uint16_t*>(base + offset + 4) >> 6) != 12) // UE4: NUL-terminated, UE5 FNamePool: next entry header (Len 12 = "ByteProperty")
				continue;

			bool hasByteProp = false;
			for (int k = 5; k < 0x30 && offset + k + 8 < regionSize; k++)
			{
				if (*reinterpret_cast<const uint32_t*>(base + offset + k) == ByteAsUint32
					&& *reinterpret_cast<const uint32_t*>(base + offset + k + 4) == PropAsUint32)
				{
					hasByteProp = true;
					break;
				}
			}

			if (!hasByteProp)
				continue;

			candidatesFound++;
			const uintptr_t noneAddr = reinterpret_cast<uintptr_t>(base + offset);

			{
				char buf[256];
				sprintf_s(buf, "  Candidate #%d: 'None'+'ByteProperty' at 0x%llX",
					candidatesFound, (unsigned long long)noneAddr);
				DebugLog(buf);
			}

			uintptr_t targets[3] = { noneAddr - 2, noneAddr - 4, noneAddr - 6 };
			const uint8_t* foundPtrLocs[3] = { nullptr, nullptr, nullptr };
			int foundCount = 0;

			auto findPtr = [&](void* Addr) -> bool
			{
				const uintptr_t val = *reinterpret_cast<const uintptr_t*>(Addr);
				for (int i = 0; i < 3; i++)
				{
					if (val == targets[i] && !foundPtrLocs[i])
					{
						foundPtrLocs[i] = static_cast<const uint8_t*>(Addr);
						foundCount++;
						break;
					}
				}
				return foundCount >= 3;
			};

			Platform::IterateSectionWithCallback(DataInfo, findPtr, sizeof(void*));

			for (int t = 0; t < 3; t++)
			{
				if (!foundPtrLocs[t]) continue;
				int hdrOff = (t + 1) * 2;

				{
					char buf[256];
					sprintf_s(buf, "    Blocks[0] ptr at %p (hdrOff=%d)", foundPtrLocs[t], hdrOff);
					DebugLog(buf);
				}

				const int HeaderSizes[] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40 };
				for (int hs : HeaderSizes)
				{
					const uint8_t* pool = foundPtrLocs[t] - hs;
					if (Platform::IsBadReadPtr(pool)) continue;

					if (InitializeNamePool(const_cast<uint8_t*>(pool)))
					{
						char buf[128];
						sprintf_s(buf, "    SUCCESS! Pool at ptr-0x%X", hs);
						DebugLog(buf);
						Off::InSDK::NameArray::GNames = Platform::GetOffset(pool);
						return true;
					}
				}
			}

			break;
		}

		if (candidatesFound > 20) break;
	}

	{
		char buf[128];
		sprintf_s(buf, "  Phase 1: scanned %d regions, %d candidates", regionsScanned, candidatesFound);
		DebugLog(buf);
	}

	if (candidatesFound == 0)
		DebugLog("  No FName chunk pattern found in process memory");
	else
		DebugLog("  InitializeNamePool failed for all candidates");

	return false;

#endif // PLATFORM_WINDOWS
}

static bool SafeCallDiscovery(bool(*Fn)())
{
	__try
	{
		return Fn();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		DWORD code = GetExceptionCode();
		char buf[128];
		sprintf_s(buf, "    -> CRASHED (exception 0x%08X)", code);
		DebugLog(buf);
		return false;
	}
}

bool NameArray::TryInit(bool bIsTestOnly)
{
	const uintptr_t ImageBase = Platform::GetModuleBase();

	uint8* GNamesAddress = nullptr;

	bool bFoundNameArray = false;
	bool bFoundnamePool = false;

	DebugLog("\nSearching for GNames...\n");

	DebugLog("  [1/5] TryFindNameArray (UE4 TNameEntryArray)...");
	if (SafeCallDiscovery([]() { return CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNameArray); }))
	{
		char buf[128]; sprintf_s(buf, "  FOUND 'TNameEntryArray GNames' at offset 0x%X", Off::InSDK::NameArray::GNames); DebugLog(buf);
		GNamesAddress = *reinterpret_cast<uint8**>(ImageBase + Off::InSDK::NameArray::GNames);
		Settings::Internal::bUseNamePool = false;
		bFoundNameArray = true;
	}
	else DebugLog("    -> not found");

	if (!bFoundNameArray)
	{
		DebugLog("  [2/5] TryFindNamePoolCtorXref (UE 5.8: 'ByteProperty' ref -> FNamePool::FNamePool -> lea rcx)...");
		if (SafeCallDiscovery([]() { return CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePoolCtorXref); }))
		{
			char buf[128]; sprintf_s(buf, "  FOUND 'FNamePool GNames' (ctor xref) at offset 0x%X", Off::InSDK::NameArray::GNames); DebugLog(buf);
			GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
			Settings::Internal::bUseNamePool = true;
			bFoundnamePool = true;
		}
		else DebugLog("    -> not found");
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		DebugLog("  [3/5] TryFindNamePoolLazyInit (UE 5.4+ lea-pair scan)...");
		if (SafeCallDiscovery([]() { return CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePoolLazyInit); }))
		{
			char buf[128]; sprintf_s(buf, "  FOUND 'FNamePool GNames' (lazy-init) at offset 0x%X", Off::InSDK::NameArray::GNames); DebugLog(buf);
			GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
			Settings::Internal::bUseNamePool = true;
			bFoundnamePool = true;
		}
		else DebugLog("    -> not found");
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		DebugLog("  [4/5] TryFindNamePool (constructor pattern)...");
		if (SafeCallDiscovery([]() { return CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePool); }))
		{
			char buf[128]; sprintf_s(buf, "  FOUND 'FNamePool GNames' at offset 0x%X", Off::InSDK::NameArray::GNames); DebugLog(buf);
			GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
			Settings::Internal::bUseNamePool = true;
			bFoundnamePool = true;
		}
		else DebugLog("    -> not found");
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		DebugLog("  [5/5] TryFindNamePoolBruteForce (data section scan)...");
		if (SafeCallDiscovery([]() { return CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePoolBruteForce); }))
		{
			char buf[128]; sprintf_s(buf, "  FOUND 'FNamePool GNames' (brute-force) at offset 0x%X", Off::InSDK::NameArray::GNames); DebugLog(buf);
			GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
			Settings::Internal::bUseNamePool = true;
			bFoundnamePool = true;
		}
		else DebugLog("    -> not found");
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		DebugLog("\n\nCould not find GNames!\n");
		return false;
	}

	if (bIsTestOnly)
		return false;

	DebugLog("  Initializing name pool/array...");

	if (bFoundNameArray && NameArray::InitializeNameArray(GNamesAddress))
	{
		DebugLog("  InitializeNameArray succeeded");
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = false;
		FNameEntry::Init();
		return true;
	}
	else if (bFoundnamePool && NameArray::InitializeNamePool(reinterpret_cast<uint8_t*>(GNamesAddress)))
	{
		DebugLog("  InitializeNamePool succeeded");
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = true;
		/* FNameEntry::Init() was moved into NameArray::InitializeNamePool to avoid duplicated logic */
		return true;
	}

	std::cerr << "The address that was found couldn't be used by the generator, this might be due to GNames-encryption.\n" << std::endl;

	return false;
}


bool NameArray::TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName)
{
	const uintptr_t ImageBase = Platform::GetModuleBase(ModuleName);

	uint8* GNamesAddress = nullptr;

	const bool bIsNameArrayOverride = !bIsNamePool;
	const bool bIsNamePoolOverride = bIsNamePool;

	bool bFoundNameArray = false;
	bool bFoundnamePool = false;

	Off::InSDK::NameArray::GNames = OffsetOverride;

	if (bIsNameArrayOverride)
	{
		std::cerr << std::format("Overwrote offset: 'TNameEntryArray GNames' set as offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = *reinterpret_cast<uint8**>(ImageBase + Off::InSDK::NameArray::GNames);// Derefernce
		Settings::Internal::bUseNamePool = false;
		bFoundNameArray = true;
	}
	else if (bIsNamePoolOverride)
	{
		std::cerr << std::format("Overwrote offset: 'FNamePool GNames' set as offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames); // No derefernce
		Settings::Internal::bUseNamePool = true;
		bFoundnamePool = true;
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		std::cerr << "\n\nCould not find GNames!\n\n" << std::endl;
		return false;
	}

	if (bFoundNameArray && NameArray::InitializeNameArray(GNamesAddress))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = false;
		FNameEntry::Init();
		return true;
	}
	else if (bFoundnamePool && NameArray::InitializeNamePool(reinterpret_cast<uint8_t*>(GNamesAddress)))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = true;
		/* FNameEntry::Init() was moved into NameArray::InitializeNamePool to avoid duplicated logic */
		return true;
	}

	std::cerr << "The address was overwritten, but couldn't be used. This might be due to GNames-encryption.\n" << std::endl;

	return false;
}

bool NameArray::SetGNamesWithoutCommitting()
{
	/* GNames is already set */
	if (Off::InSDK::NameArray::GNames != 0x0)
		return false;

	if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNameArray))
	{
		std::cerr << std::format("Found 'TNameEntryArray GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = false;
		return true;
	}
	else if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePoolCtorXref))
	{
		std::cerr << std::format("Found 'FNamePool GNames' (ctor xref) at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = true;
		return true;
	}
	else if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePoolLazyInit))
	{
		std::cerr << std::format("Found 'FNamePool GNames' (lazy-init) at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = true;
		return true;
	}
	else if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePool))
	{
		std::cerr << std::format("Found 'FNamePool GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = true;
		return true;
	}
	DebugLog("  SetGNamesWithoutCommitting: all methods failed (brute-force skipped here)");
	return false;
}

void NameArray::PostInit()
{
	if (GNames && Settings::Internal::bUseNamePool)
	{
		// UE 5.8+: FNamePool supports non-power-of-two shard counts, but the chunk storage
		// still uses power-of-two block offset bits. This loop finds the correct bit count by
		// iterating from 0xE upward until all object names resolve to valid chunk indices.

		NameArray::FNameBlockOffsetBits = 0xE;

		int i = ObjectArray::Num();
		while (i >= 0)
		{
			const int32 CurrentBlock = NameArray::GetNumChunks();

			UEObject Obj = ObjectArray::GetByIndex(i);

			if (!Obj)
			{
				i--;
				continue;
			}

			const int32 ObjNameChunkIdx = Obj.GetFName().GetCompIdx() >> NameArray::FNameBlockOffsetBits;

			if (ObjNameChunkIdx == CurrentBlock)
				break;

			if (ObjNameChunkIdx > CurrentBlock)
			{
				NameArray::FNameBlockOffsetBits++;

				// Safety: block offset bits should never exceed 0x1F (31 bits)
				if (NameArray::FNameBlockOffsetBits > 0x1F)
				{
					std::cerr << "NameArray::PostInit: FNameBlockOffsetBits exceeded max (0x1F), aborting\n" << std::endl;
					break;
				}

				i = ObjectArray::Num();
			}

			i--;
		}
		Off::InSDK::NameArray::FNamePoolBlockOffsetBits = NameArray::FNameBlockOffsetBits;

		std::cerr << "NameArray::FNameBlockOffsetBits: 0x" << std::hex << NameArray::FNameBlockOffsetBits << "\n" << std::endl;
	}
}

int32 NameArray::GetNumChunks()
{
	return *reinterpret_cast<int32*>(GNames + Off::NameArray::MaxChunkIndex);
}

int32 NameArray::GetNumElements()
{
	return !Settings::Internal::bUseNamePool ? *reinterpret_cast<int32*>(GNames + Off::NameArray::NumElements) : 0;
}

int32 NameArray::GetByteCursor()
{
	return Settings::Internal::bUseNamePool ? *reinterpret_cast<int32*>(GNames + Off::NameArray::ByteCursor) : 0;
}

FNameEntry NameArray::GetNameEntry(const void* Name)
{
	return ByIndex(GNames, FName(Name).GetCompIdx(), FNameBlockOffsetBits);
}

FNameEntry NameArray::GetNameEntry(int32 Idx)
{
	return ByIndex(GNames, Idx, FNameBlockOffsetBits);
}

