// inject.exe <pid> <full path to dll>
// classic LoadLibraryA injection (32-bit -> 32-bit)
#include <windows.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: inject.exe <pid> <dll path>\n"); return 2; }

    DWORD pid = (DWORD)atoi(argv[1]);
    const char* dll = argv[2];

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) { printf("OpenProcess failed %lu\n", GetLastError()); return 3; }

    size_t len = strlen(dll) + 1;
    LPVOID remote = VirtualAllocEx(hProc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed %lu\n", GetLastError()); return 4; }

    if (!WriteProcessMemory(hProc, remote, dll, len, nullptr)) {
        printf("WriteProcessMemory failed %lu\n", GetLastError()); return 5;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    printf("LoadLibraryA at %p\n", (void*)loadLib);

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)loadLib, remote, 0, nullptr);
    if (!hThread) { printf("CreateRemoteThread failed %lu\n", GetLastError()); return 6; }

    WaitForSingleObject(hThread, 8000);
    DWORD code = 0;
    GetExitCodeThread(hThread, &code);
    printf("remote thread exit=0x%08lX (hmodule)\n", code);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return code ? 0 : 7;
}
