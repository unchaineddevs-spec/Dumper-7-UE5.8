
#include <format>
#include <cstdio>
#include <cstring>

#include "Unreal/UnrealTypes.h"
#include "Unreal/NameArray.h"

#include "Encoding/UnicodeNames.h"

#include "Architecture.h"

extern FILE* g_LogFile;

static void DebugLogFN(const char* msg)
{
	std::cerr << msg << std::endl;
	if (g_LogFile) { fputs(msg, g_LogFile); fputc('\n', g_LogFile); fflush(g_LogFile); }
}

// SEH wrapper for individual AppendString calls during iteration
static bool SafeAppendStringCall(void(*Fn)(const void*, FString&), const void* Name, FString& OutStr)
{
	__try
	{
		Fn(Name, OutStr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// Standalone SEH function — no C++ objects with destructors allowed
static bool TestAppendStringSafe(void(*Fn)(const void*, FString&))
{
	static wchar_t s_TestBuf[256];
	memset(s_TestBuf, 0, sizeof(s_TestBuf));

	// POD struct matching FString (TArray<wchar_t>) layout: {wchar_t* Data, int32 Num, int32 Max}
	struct RawFStr { void* Data; int Num; int Max; };
	RawFStr rawStr;
	rawStr.Data = s_TestBuf;
	rawStr.Num = 0;
	rawStr.Max = 256;

	// FName with ComparisonIndex=0 ("None"), Number=0
	uint8_t noneName[8];
	memset(noneName, 0, sizeof(noneName));

	__try
	{
		Fn(noneName, reinterpret_cast<FString&>(rawStr));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		DWORD code = GetExceptionCode();
		char buf[128];
		sprintf_s(buf, "  AppendString test CRASHED (exception 0x%08X)", code);
		DebugLogFN(buf);
		return false;
	}
}


std::string MakeNameValid(std::wstring&& Name)
{
	static constexpr const wchar_t* Numbers[10] =
	{
		L"Zero",
		L"One",
		L"Two",
		L"Three",
		L"Four",
		L"Five",
		L"Six",
		L"Seven",
		L"Eight",
		L"Nine"
	};

	if (Name == L"bool")
		return "Bool";

	if (Name == L"NULL")
		return "NULLL";

	/* Replace 0 with Zero or 9 with Nine, if it is the first letter of the name. */
	if (Name[0] <= '9' && Name[0] >= '0')
	{
		Name.replace(0, 1, Numbers[Name[0] - '0']);
	}
	
	std::u32string Strrr;
	Strrr += UtfN::utf_cp32_t{ 200 };

	std::u32string Utf32Name = UtfN::Utf16StringToUtf32String<std::u32string>(Name);

	bool bIsFirstIteration = true;
	for (auto It = UtfN::utf32_iterator<std::u32string::iterator>(Utf32Name); It; ++It)
	{
		if (bIsFirstIteration && !IsUnicodeCharXIDStart(Name[0]))
		{
			/* Replace invalid starting character with 'm' character. 'm' for "member" */
			Name[0] = 'm';

			bIsFirstIteration = false;
		}

		if (!IsUnicodeCharXIDContinue((*It).Get()))
			It.Replace('_');
	}

	return  UtfN::Utf32StringToUtf8String<std::string>(Utf32Name);;
}


FName::FName(const void* Ptr)
	: Address(static_cast<const uint8*>(Ptr))
{
}

void FName::Init_Windows(bool bForceGNames)
{
#ifdef PLATFORM_WINDOWS

#if defined(_WIN64)
	constexpr std::array<const char*, 6> PossibleSigs = 
	{ 
		"48 8D ? ? 48 8D ? ? E8",
		"48 8D ? ? ? 48 8D ? ? E8",
		"48 8D ? ? 49 8B ? E8",
		"48 8D ? ? ? 49 8B ? E8",
		"48 8D ? ? 48 8B ? E8",
		"48 8D ? ? ? 48 8B ? E8",
	};
#elif defined(_WIN32)
	constexpr std::array<const char*, 1> PossibleSigs = 
	{
		"8D 44 24 ? 8D 4C 24 ? 50 E8",
	};
#endif

	const void* StringRef = Platform::FindByStringInAllSections("ForwardShadingQuality_", 0x0, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings);
	
	bool bFoundPotentiallyOverlappingSig = false;

	if (StringRef)
	{
		const char* MatchingSig = nullptr;

		for (int i = 0; !AppendString && i < PossibleSigs.size(); i++)
		{
			AppendString = static_cast<decltype(AppendString)>(Platform::FindPatternInRange(PossibleSigs[i], StringRef, 0x50, true, -1/* auto */));

			if (AppendString)
				MatchingSig = PossibleSigs[i];
		}

		// This signature partially overlaps with the signature for an inlined FName::AppendString call (see comment below)
		bFoundPotentiallyOverlappingSig = MatchingSig && strcmp(MatchingSig, "48 8D ? ? ? 48 8B ? E8") == 0;
	}

	// Test if AppendString was inlined
	if ((!AppendString || bFoundPotentiallyOverlappingSig) && !bForceGNames && StringRef)
	{
		/*
		* 0x00: 8B ? ?          mov     ecx, [...]
		* 0x03: E8 ? ? ? ?      call    FName::GetComparisonNameEntry
		* 0x08: 48 8D ? ?       lea     rdx, [...]
		* 0x0B: 48 8B C8        mov     rcx, rax
		* 0x10: E8 ? ? ? ?      call    FNameEntry::GetName
		*/
		if (void* SigScanResult = Platform::FindPatternInRange("8B ? ? E8 ? ? ? ? 48 8D ? ? ? 48 8B C8 E8 ? ? ? ?", StringRef, 0x180))
		{
			const uintptr_t ResultAsInt = reinterpret_cast<const uintptr_t>(SigScanResult);

			GetNameEntryFromName = reinterpret_cast<decltype(GetNameEntryFromName)>(Architecture_x86_64::Resolve32BitRelativeCall(ResultAsInt + 0x3));
			AppendString = reinterpret_cast<decltype(AppendString)>(Architecture_x86_64::Resolve32BitRelativeCall(ResultAsInt + 0x10));

			Off::InSDK::Name::GetNameEntryFromName = Platform::GetOffset(GetNameEntryFromName);
			Off::InSDK::Name::bIsAppendStringInlinedAndUsed = true;

			ToStr = [](const void* Name) -> std::wstring
			{
				thread_local FFreableString TempString(1024);

				AppendString(GetNameEntryFromName(FName(Name).GetCompIdx()), TempString);

				std::wstring OutputString = TempString.ToWString();
				TempString.ResetNum();

				const uint32 Number = FName(Name).GetNumber();

				if (Number > 0)
					return OutputString + L'_' + std::to_wstring(Number - 1);

				return OutputString;
			};
		}
	}

	if (AppendString == nullptr)
		AppendString = static_cast<decltype(AppendString)>(TryFindApendStringBackupStringRef_Windows());

	// Verify AppendString actually works before committing to it
	if (AppendString && !bForceGNames)
	{
		char addrBuf[128];
		sprintf_s(addrBuf, "  Testing AppendString at offset 0x%llX...", (unsigned long long)Platform::GetOffset(AppendString));
		DebugLogFN(addrBuf);

		if (!TestAppendStringSafe(AppendString))
		{
			DebugLogFN("  AppendString FAILED — falling back to GNames mode");
			AppendString = nullptr;
			ToStr = nullptr;
			GetNameEntryFromName = nullptr;
			Off::InSDK::Name::bIsAppendStringInlinedAndUsed = false;
		}
		else
		{
			DebugLogFN("  AppendString test OK");
		}
	}

	// Always set the offset if AppendString was found — TryFindNamePoolLazyInit and
	// the brute-force Phase 0 need it to scan AppendString's code for FNamePool lea.
	Off::InSDK::Name::AppendNameToString = AppendString ? Platform::GetOffset(AppendString) : 0x0;

	if (!AppendString || bForceGNames)
	{
		const bool bInitializedSuccessfully = NameArray::TryInit();

		if (bInitializedSuccessfully)
		{
			ToStr = [](const void* Name) -> std::wstring
			{
				if (!Settings::Internal::bUseOutlineNumberName)
				{
					const uint32 Number = FName(Name).GetNumber();

					if (Number > 0)
						return NameArray::GetNameEntry(Name).GetWString() + L'_' + std::to_wstring(Number - 1);
				}

				return NameArray::GetNameEntry(Name).GetWString();
			};

			return;
		}
		else /* Attempt to find FName::ToString as a final fallback */
		{
			/* Initialize GNames offset without committing to use GNames during the dumping process or in the SDK */
			NameArray::SetGNamesWithoutCommitting();
			FName::InitFallback();
		}
	}

	std::cerr << std::format("Found FName::{} at Offset 0x{:X}\n\n", (Off::InSDK::Name::bIsUsingAppendStringOverToString ? "AppendString" : "ToString"), Off::InSDK::Name::AppendNameToString);

	/* Initialize GNames offset without committing to use GNames during the dumping process or in the SDK */
	NameArray::SetGNamesWithoutCommitting();

	if (ToStr)
		return;

	ToStr = [](const void* Name) -> std::wstring
	{
		thread_local FFreableString TempString(1024);

		if (!SafeAppendStringCall(AppendString, Name, TempString))
		{
			static bool bLoggedFirstCrash = false;
			if (!bLoggedFirstCrash)
			{
				bLoggedFirstCrash = true;
				const uint8_t* raw = static_cast<const uint8_t*>(Name);
				char buf[256];
				sprintf_s(buf, "  AppendString CRASH during iteration! FName bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
					raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
				DebugLogFN(buf);
			}
			TempString.ResetNum();
			return std::wstring(L"INVALID_NAME");
		}

		std::wstring OutputString = TempString.ToWString();
		TempString.ResetNum();

		return OutputString;
	};

#endif // PLATFORM_WINDOWS
}

void FName::Init(int32 OverrideOffset, EOffsetOverrideType OverrideType, bool bIsNamePool, const char* const ModuleName)
{
	if (OverrideType == EOffsetOverrideType::GNames)
	{
		const bool bInitializedSuccessfully = NameArray::TryInit(OverrideOffset, bIsNamePool, ModuleName);

		if (bInitializedSuccessfully)
		{
			ToStr = [](const void* Name) -> std::wstring
			{
				if (!Settings::Internal::bUseOutlineNumberName)
				{
					const uint32 Number = FName(Name).GetNumber();

					if (Number > 0)
						return NameArray::GetNameEntry(Name).GetWString() + L'_' + std::to_wstring(Number - 1);
				}

				return NameArray::GetNameEntry(Name).GetWString();
			};
		}

		return;
	}

	AppendString = reinterpret_cast<decltype(AppendString)>(Platform::GetModuleBase(ModuleName) + OverrideOffset);

	Off::InSDK::Name::AppendNameToString = OverrideOffset;
	Off::InSDK::Name::bIsUsingAppendStringOverToString = OverrideType == EOffsetOverrideType::AppendString;

	ToStr = [](const void* Name) -> std::wstring
	{
		thread_local FFreableString TempString(1024);

		AppendString(Name, TempString);

		std::wstring OutputString = TempString.ToWString();
		TempString.ResetNum();

		return OutputString;
	};

	std::cerr << std::format("Manual-Override: FName::{} --> Offset 0x{:X}\n\n", (Off::InSDK::Name::bIsUsingAppendStringOverToString ? "AppendString" : "ToString"), Off::InSDK::Name::AppendNameToString);
}


void* FName::TryFindApendStringBackupStringRef_Windows()
{

#ifdef PLATFORM_WINDOWS

#if defined(_WIN64)
	constexpr std::array<const char*, 3> PossibleSigs =
	{
		"48 8B ? 48 8B ? ? E8",
		"48 8B ? ? 48 89 ? ? E8",
		"48 8B ? 48 89 ? ? ? E8"
	};
#elif defined(_WIN32)
	constexpr std::array<const char*, 0> PossibleSigs =
	{
		// Todo I guess.
	};
#endif

	const void* StringRef = Platform::FindByStringInAllSections(L" Bone: ", 0x0, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings);

	if (StringRef)
	{
		const char* MatchingSig = nullptr;

		// AppendString comes before the string ref, so search upwards (in IDA terms)
		const uintptr_t SigSearchStartAddress = reinterpret_cast<uintptr_t>(StringRef) - 0xB0;

		for (int i = 0; !AppendString && i < PossibleSigs.size(); i++)
		{
			AppendString = static_cast<decltype(AppendString)>(Platform::FindPatternInRange(PossibleSigs[i], SigSearchStartAddress, 0x100, true, -1/* auto */));

			if (AppendString)
				return AppendString;
		}
	}
#endif // PLATFORM_WINDOWS

	return nullptr;
}

void FName::InitFallback()
{
	Off::InSDK::Name::bIsUsingAppendStringOverToString = false;

	void* Conv_NameToStringAddress = FindUnrealExecFunctionByString("Conv_NameToString");

	constexpr std::array<const char*, 3> PossibleSigs =
	{
		"89 44 ? ? 48 01 ? ? E8",
		"48 89 ? ? 48 8D ? ? ? E8",
		"48 89 ? ? ? 48 89 ? ? E8",
	};

	int i = 0;
	while (!AppendString && i < PossibleSigs.size())
	{
		AppendString = static_cast<decltype(AppendString)>(Platform::FindPatternInRange(PossibleSigs[i], Conv_NameToStringAddress, 0x90, -1 /* auto */));

		i++;
	}

	Off::InSDK::Name::AppendNameToString = AppendString ? Platform::GetOffset(AppendString) : 0x0;
}


std::wstring FName::ToRawWString() const
{
	if (!Address)
		return L"None";

	return ToStr(Address);
}

std::wstring FName::ToWString() const
{
	std::wstring OutputString = ToRawWString();

	size_t pos = OutputString.rfind('/');

	if (pos == std::wstring::npos)
		return OutputString;

	return OutputString.substr(pos + 1);
}

std::string FName::ToRawString() const
{
	if (!Address)
		return "None";

	return UtfN::WStringToString(ToRawWString());
}

std::string FName::ToString() const
{
	if (!Address)
		return "None";

	return UtfN::WStringToString(ToWString());
}

std::string FName::ToValidString() const
{
	return MakeNameValid(ToWString());
}

int32 FName::GetCompIdx() const 
{
	return *reinterpret_cast<const int32*>(Address + Off::FName::CompIdx);
}

uint32 FName::GetNumber() const
{
	if (Settings::Internal::bUseOutlineNumberName)
		return 0x0;

	if (Settings::Internal::bUseNamePool)
		return *reinterpret_cast<const uint32*>(Address + Off::FName::Number); // The number is uint32 on versions <= UE4.23 

	return static_cast<uint32_t>(*reinterpret_cast<const int32*>(Address + Off::FName::Number));
}

bool FName::operator==(FName Other) const
{
	return GetCompIdx() == Other.GetCompIdx();
}

bool FName::operator!=(FName Other) const
{
	return GetCompIdx() != Other.GetCompIdx();
}

std::string FName::CompIdxToString(int CmpIdx)
{
	if (!Settings::Internal::bUseCasePreservingName)
	{
		struct FakeFName
		{
			int CompIdx;
			uint8 Pad[0x4];
		} Name(CmpIdx);

		return FName(&Name).ToString();
	}
	else
	{
		struct FakeFName
		{
			int CompIdx;
			uint8 Pad[0xC];
		} Name(CmpIdx);

		return FName(&Name).ToString();
	}
}

void* FName::DEBUGGetAppendString()
{
	return (void*)(AppendString);
}
