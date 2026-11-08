#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <filesystem>

DWORD FindProcessByName(const wchar_t* processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, processName) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

bool InjectDll(DWORD pid, const std::string& dllPath)
{
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!process)
    {
        std::cerr << "Failed to open process (error " << GetLastError() << ")\n";
        return false;
    }

    void* remoteMem = VirtualAllocEx(process, nullptr, dllPath.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        std::cerr << "Failed to allocate memory in target (error " << GetLastError() << ")\n";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), dllPath.size() + 1, nullptr))
    {
        std::cerr << "Failed to write DLL path (error " << GetLastError() << ")\n";
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLibAddr = GetProcAddress(kernel32, "LoadLibraryA");

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)loadLibAddr, remoteMem, 0, nullptr);
    if (!thread)
    {
        std::cerr << "Failed to create remote thread (error " << GetLastError() << ")\n";
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    CloseHandle(process);

    return exitCode != 0;
}

void ListProcesses(const wchar_t* filter)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (!filter || wcsstr(entry.szExeFile, filter))
                std::wcout << L"  [" << entry.th32ProcessID << L"] " << entry.szExeFile << L"\n";
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

int main(int argc, char* argv[])
{
    std::cout << "=== Dumper-7 UE5.8 Loader ===\n\n";

    std::string dllPath;
    if (argc >= 3)
    {
        dllPath = argv[2];
    }
    else
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        dllPath = (exeDir / "Dumper-7.dll").string();
    }

    if (!std::filesystem::exists(dllPath))
    {
        std::cerr << "DLL not found: " << dllPath << "\n";
        std::cerr << "Place Dumper-7.dll next to this exe, or pass the path as second argument.\n";
        return 1;
    }

    dllPath = std::filesystem::absolute(dllPath).string();
    std::cout << "DLL: " << dllPath << "\n\n";

    std::string target = "ArkAscended.exe";
    if (argc >= 2)
        target = argv[1];

    DWORD pid = 0;
    bool allDigits = !target.empty() && std::all_of(target.begin(), target.end(), ::isdigit);
    if (allDigits)
    {
        pid = std::stoul(target);
    }
    else
    {
        std::cout << "Waiting for " << target << "...\n";
        while (!pid)
        {
            std::wstring wTarget(target.begin(), target.end());
            pid = FindProcessByName(wTarget.c_str());
            if (!pid)
                Sleep(1000);
        }
        std::cout << "Found " << target << " (PID " << pid << ")\n";
    }

    std::cout << "Injecting into PID " << pid << "...\n";

    if (InjectDll(pid, dllPath))
    {
        std::cout << "Dumper-7 injected successfully.\n";
        return 0;
    }

    std::cerr << "Injection failed.\n";
    return 1;
}
