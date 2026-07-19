#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static DWORD find_process(const WCHAR *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    DWORD result = 0;

    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!_wcsicmp(entry.szExeFile, name) && entry.th32ProcessID != GetCurrentProcessId()) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

static int inject(DWORD process_id, const WCHAR *dll_path)
{
    HANDLE process;
    HANDLE thread;
    void *remote_path;
    SIZE_T bytes = (wcslen(dll_path) + 1) * sizeof(WCHAR);
    SIZE_T written = 0;
    HMODULE kernel32;
    FARPROC load_library;
    DWORD remote_result = 0;
    int ok = 0;

    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                          PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                          FALSE, process_id);
    if (!process) return 0;
    remote_path = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) goto done;
    if (!WriteProcessMemory(process, remote_path, dll_path, bytes, &written) || written != bytes)
        goto done;
    kernel32 = GetModuleHandleW(L"kernel32.dll");
    load_library = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : NULL;
    if (!load_library) goto done;
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library,
                                remote_path, 0, NULL);
    if (!thread) goto done;
    if (WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &remote_result) && remote_result)
        ok = 1;
    CloseHandle(thread);

done:
    if (remote_path) VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int wmain(int argc, WCHAR **argv)
{
    DWORD process_id = 0;
    const WCHAR *process_name;
    int attempt;

    if (argc < 2 || argc > 3) {
        fwprintf(stderr, L"usage: mailbird-dnd-injector.exe <hook-dll-path> [process-name]\n");
        return 2;
    }
    process_name = argc == 3 ? argv[2] : L"Mailbird.exe";
    for (attempt = 0; attempt < 300 && !process_id; ++attempt) {
        process_id = find_process(process_name);
        if (!process_id) Sleep(100);
    }
    if (!process_id) {
        fwprintf(stderr, L"mailbird-dnd: %ls not found\n", process_name);
        return 1;
    }
    if (!inject(process_id, argv[1])) {
        fwprintf(stderr, L"mailbird-dnd: DLL injection failed for process %lu (error %lu)\n",
                 process_id, GetLastError());
        return 1;
    }
    return 0;
}
