#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>

#ifndef CFSTR_FILEDESCRIPTORA
#define CFSTR_FILEDESCRIPTORA "FileGroupDescriptor"
#endif
#ifndef CFSTR_FILEDESCRIPTORW
#define CFSTR_FILEDESCRIPTORW L"FileGroupDescriptorW"
#endif
#ifndef CFSTR_FILECONTENTS
#define CFSTR_FILECONTENTS L"FileContents"
#endif

#define DEFAULT_PORT 45981
#define MAX_DRAG_FILES 4096U

typedef HRESULT (WINAPI *do_drag_drop_fn)(IDataObject *, IDropSource *, DWORD, DWORD *);

static do_drag_drop_fn original_do_drag_drop;
static LONG drag_counter;

struct file_list {
    char **unix_paths;
    UINT count;
};

static void log_line(const char *message)
{
    WCHAR root[MAX_PATH * 4];
    WCHAR path[MAX_PATH * 4];
    HANDLE file;
    DWORD written;

    if (!GetEnvironmentVariableW(L"MAILBIRD_DND_STAGE_WIN", root, ARRAYSIZE(root))) return;
    _snwprintf(path, ARRAYSIZE(path), L"%ls\\hook.log", root);
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
    WriteFile(file, "\r\n", 2, &written, NULL);
    CloseHandle(file);
}

static void set_copy_cursor(void)
{
    HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
    HCURSOR cursor = ole32 ? LoadCursorW(ole32, MAKEINTRESOURCEW(3)) : NULL;
    if (!cursor) cursor = LoadCursorW(NULL, IDC_ARROW);
    if (cursor) SetCursor(cursor);
}

static char *wide_to_utf8(const WCHAR *text)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *result;
    if (size <= 0) return NULL;
    result = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size);
    if (!result) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, result, size, NULL, NULL)) {
        HeapFree(GetProcessHeap(), 0, result);
        return NULL;
    }
    return result;
}

static char *dos_to_unix(const WCHAR *path)
{
    char *utf8 = wide_to_utf8(path);
    char prefix[4096];
    char *result;
    const char *rest;
    size_t prefix_len;
    size_t rest_len;
    size_t i;

    if (!utf8) return NULL;
    for (i = 0; utf8[i]; ++i) if (utf8[i] == '\\') utf8[i] = '/';
    if ((utf8[0] == 'Z' || utf8[0] == 'z') && utf8[1] == ':') {
        result = HeapAlloc(GetProcessHeap(), 0, strlen(utf8 + 2) + 1);
        if (result) strcpy(result, utf8 + 2);
        HeapFree(GetProcessHeap(), 0, utf8);
        return result;
    }
    if (!((utf8[0] >= 'A' && utf8[0] <= 'Z') ||
          (utf8[0] >= 'a' && utf8[0] <= 'z')) || utf8[1] != ':') {
        HeapFree(GetProcessHeap(), 0, utf8);
        return NULL;
    }
    if (!GetEnvironmentVariableA("MAILBIRD_DND_PREFIX_UNIX", prefix, sizeof(prefix))) {
        HeapFree(GetProcessHeap(), 0, utf8);
        return NULL;
    }
    rest = utf8 + 2;
    while (*rest == '/') ++rest;
    prefix_len = strlen(prefix);
    rest_len = strlen(rest);
    result = HeapAlloc(GetProcessHeap(), 0, prefix_len + 16 + rest_len + 1);
    if (result) {
        char drive = utf8[0];
        if (drive >= 'A' && drive <= 'Z') drive = (char)(drive - 'A' + 'a');
        sprintf(result, "%s/dosdevices/%c:/%s", prefix, drive, rest);
    }
    HeapFree(GetProcessHeap(), 0, utf8);
    return result;
}

static void free_file_list(struct file_list *list)
{
    UINT i;
    for (i = 0; i < list->count; ++i) HeapFree(GetProcessHeap(), 0, list->unix_paths[i]);
    HeapFree(GetProcessHeap(), 0, list->unix_paths);
    list->unix_paths = NULL;
    list->count = 0;
}

static int make_directory(const WCHAR *path)
{
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int make_staging_directory(WCHAR *directory, size_t capacity)
{
    WCHAR root[4096];
    LONG counter = InterlockedIncrement(&drag_counter);

    if (!GetEnvironmentVariableW(L"MAILBIRD_DND_STAGE_WIN", root, ARRAYSIZE(root))) return 0;
    if (!make_directory(root)) return 0;
    _snwprintf(directory, capacity, L"%ls\\drag-%lu-%ld-%llu", root,
               GetCurrentProcessId(), counter, (unsigned long long)GetTickCount64());
    return make_directory(directory);
}

static void sanitize_name(WCHAR *output, size_t capacity, const WCHAR *input, UINT index)
{
    const WCHAR *base = input;
    size_t length = 0;
    size_t i;

    while (*input) {
        if (*input == L'\\' || *input == L'/') base = input + 1;
        ++input;
    }
    while (base[length] && length + 1 < capacity) ++length;
    if (!length) {
        _snwprintf(output, capacity, L"item-%u", index + 1);
        return;
    }
    memcpy(output, base, length * sizeof(WCHAR));
    output[length] = 0;
    for (i = 0; i < length; ++i) {
        if (output[i] < 32 || wcschr(L"<>:\"/\\|?*", output[i])) output[i] = L'_';
    }
    while (length && (output[length - 1] == L'.' || output[length - 1] == L' '))
        output[--length] = 0;
    if (!length) _snwprintf(output, capacity, L"item-%u", index + 1);
}

static int unique_path(WCHAR *output, size_t capacity, const WCHAR *directory, const WCHAR *name)
{
    const WCHAR *extension = wcsrchr(name, L'.');
    size_t stem_length = extension && extension != name ? (size_t)(extension - name) : wcslen(name);
    UINT suffix;

    _snwprintf(output, capacity, L"%ls\\%ls", directory, name);
    output[capacity - 1] = 0;
    if (GetFileAttributesW(output) == INVALID_FILE_ATTRIBUTES) return 1;
    if (!extension || extension == name) extension = name + wcslen(name);
    for (suffix = 2; suffix < 10000; ++suffix) {
        _snwprintf(output, capacity, L"%ls\\%.*ls (%u)%ls", directory,
                   (int)stem_length, name, suffix, extension);
        output[capacity - 1] = 0;
        if (GetFileAttributesW(output) == INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

static int write_medium(const WCHAR *path, STGMEDIUM *medium)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;

    if (file == INVALID_HANDLE_VALUE) return 0;
    if (medium->tymed == TYMED_ISTREAM) {
        BYTE buffer[65536];
        HRESULT hr;
        ULONG got;
        ok = 1;
        LARGE_INTEGER start;
        start.QuadPart = 0;
        IStream_Seek(medium->pstm, start, STREAM_SEEK_SET, NULL);
        do {
            DWORD written = 0;
            got = 0;
            hr = IStream_Read(medium->pstm, buffer, sizeof(buffer), &got);
            if (FAILED(hr) || (got && (!WriteFile(file, buffer, got, &written, NULL) || written != got))) {
                ok = 0;
                break;
            }
        } while (got);
    } else if (medium->tymed == TYMED_HGLOBAL) {
        SIZE_T size = GlobalSize(medium->hGlobal);
        void *data = GlobalLock(medium->hGlobal);
        DWORD written = 0;
        ok = data && size <= MAXDWORD &&
             WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
        if (data) GlobalUnlock(medium->hGlobal);
    }
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok;
}

static HRESULT get_file_content(IDataObject *object, UINT index, STGMEDIUM *medium)
{
    FORMATETC format;
    CLIPFORMAT contents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    HRESULT hr;

    memset(&format, 0, sizeof(format));
    format.cfFormat = contents;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = (LONG)index;
    format.tymed = TYMED_ISTREAM;
    memset(medium, 0, sizeof(*medium));
    hr = IDataObject_GetData(object, &format, medium);
    if (FAILED(hr)) {
        format.tymed = TYMED_HGLOBAL;
        hr = IDataObject_GetData(object, &format, medium);
    }
    return hr;
}

static int reserve_paths(struct file_list *list, UINT count)
{
    if (!count || count > MAX_DRAG_FILES) return 0;
    list->unix_paths = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 (SIZE_T)count * sizeof(*list->unix_paths));
    if (!list->unix_paths) return 0;
    list->count = count;
    return 1;
}

static int materialize_descriptor_w(IDataObject *object, struct file_list *list)
{
    FORMATETC format = {(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW),
                        NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM descriptor_medium;
    FILEGROUPDESCRIPTORW *group;
    SIZE_T global_size;
    WCHAR directory[4096];
    UINT i;
    int ok = 0;

    memset(&descriptor_medium, 0, sizeof(descriptor_medium));
    if (FAILED(IDataObject_GetData(object, &format, &descriptor_medium)) ||
        descriptor_medium.tymed != TYMED_HGLOBAL) return 0;
    global_size = GlobalSize(descriptor_medium.hGlobal);
    group = GlobalLock(descriptor_medium.hGlobal);
    if (!group || global_size < sizeof(UINT) ||
        group->cItems > (global_size - sizeof(UINT)) / sizeof(FILEDESCRIPTORW) ||
        !reserve_paths(list, group->cItems) || !make_staging_directory(directory, ARRAYSIZE(directory)))
        goto done;

    for (i = 0; i < group->cItems; ++i) {
        WCHAR name[MAX_PATH];
        WCHAR path[4096];
        STGMEDIUM content;
        sanitize_name(name, ARRAYSIZE(name), group->fgd[i].cFileName, i);
        if (!unique_path(path, ARRAYSIZE(path), directory, name)) goto done;
        if (FAILED(get_file_content(object, i, &content)) || !write_medium(path, &content)) {
            if (content.tymed) ReleaseStgMedium(&content);
            goto done;
        }
        ReleaseStgMedium(&content);
        list->unix_paths[i] = dos_to_unix(path);
        if (!list->unix_paths[i]) goto done;
    }
    ok = 1;

done:
    if (group) GlobalUnlock(descriptor_medium.hGlobal);
    ReleaseStgMedium(&descriptor_medium);
    if (!ok) free_file_list(list);
    return ok;
}

static int materialize_descriptor_a(IDataObject *object, struct file_list *list)
{
    FORMATETC format = {(CLIPFORMAT)RegisterClipboardFormatA("FileGroupDescriptor"),
                        NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM descriptor_medium;
    FILEGROUPDESCRIPTORA *group;
    SIZE_T global_size;
    WCHAR directory[4096];
    UINT i;
    int ok = 0;

    memset(&descriptor_medium, 0, sizeof(descriptor_medium));
    if (FAILED(IDataObject_GetData(object, &format, &descriptor_medium)) ||
        descriptor_medium.tymed != TYMED_HGLOBAL) return 0;
    global_size = GlobalSize(descriptor_medium.hGlobal);
    group = GlobalLock(descriptor_medium.hGlobal);
    if (!group || global_size < sizeof(UINT) ||
        group->cItems > (global_size - sizeof(UINT)) / sizeof(FILEDESCRIPTORA) ||
        !reserve_paths(list, group->cItems) || !make_staging_directory(directory, ARRAYSIZE(directory)))
        goto done;

    for (i = 0; i < group->cItems; ++i) {
        WCHAR original[MAX_PATH];
        WCHAR name[MAX_PATH];
        WCHAR path[4096];
        STGMEDIUM content;
        MultiByteToWideChar(CP_ACP, 0, group->fgd[i].cFileName, -1, original, ARRAYSIZE(original));
        original[ARRAYSIZE(original) - 1] = 0;
        sanitize_name(name, ARRAYSIZE(name), original, i);
        if (!unique_path(path, ARRAYSIZE(path), directory, name)) goto done;
        if (FAILED(get_file_content(object, i, &content)) || !write_medium(path, &content)) {
            if (content.tymed) ReleaseStgMedium(&content);
            goto done;
        }
        ReleaseStgMedium(&content);
        list->unix_paths[i] = dos_to_unix(path);
        if (!list->unix_paths[i]) goto done;
    }
    ok = 1;

done:
    if (group) GlobalUnlock(descriptor_medium.hGlobal);
    ReleaseStgMedium(&descriptor_medium);
    if (!ok) free_file_list(list);
    return ok;
}

static int collect_hdrop(IDataObject *object, struct file_list *list)
{
    FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium;
    UINT count;
    UINT i;
    int ok = 0;

    memset(&medium, 0, sizeof(medium));
    if (FAILED(IDataObject_GetData(object, &format, &medium)) || medium.tymed != TYMED_HGLOBAL)
        return 0;
    count = DragQueryFileW((HDROP)medium.hGlobal, 0xffffffffU, NULL, 0);
    if (!reserve_paths(list, count)) goto done;
    for (i = 0; i < count; ++i) {
        UINT length = DragQueryFileW((HDROP)medium.hGlobal, i, NULL, 0);
        WCHAR *path = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)length + 1) * sizeof(WCHAR));
        if (!path || !DragQueryFileW((HDROP)medium.hGlobal, i, path, length + 1)) {
            HeapFree(GetProcessHeap(), 0, path);
            goto done;
        }
        list->unix_paths[i] = dos_to_unix(path);
        HeapFree(GetProcessHeap(), 0, path);
        if (!list->unix_paths[i]) goto done;
    }
    ok = 1;

done:
    ReleaseStgMedium(&medium);
    if (!ok) free_file_list(list);
    return ok;
}

static int send_full(SOCKET socket_fd, const void *data, size_t size)
{
    const char *cursor = data;
    while (size) {
        int chunk = size > INT_MAX ? INT_MAX : (int)size;
        int sent = send(socket_fd, cursor, chunk, 0);
        if (sent <= 0) return 0;
        cursor += sent;
        size -= (size_t)sent;
    }
    return 1;
}

static void encode_u32(unsigned char output[4], uint32_t value)
{
    output[0] = (unsigned char)value;
    output[1] = (unsigned char)(value >> 8);
    output[2] = (unsigned char)(value >> 16);
    output[3] = (unsigned char)(value >> 24);
}

static int broker_drag(const struct file_list *list)
{
    WSADATA winsock;
    SOCKET socket_fd = INVALID_SOCKET;
    struct sockaddr_in address;
    unsigned char header[8];
    unsigned char response = 0;
    char port_text[16];
    int port = DEFAULT_PORT;
    UINT i;
    int ok = 0;

    if (GetEnvironmentVariableA("MAILBIRD_DND_PORT", port_text, sizeof(port_text)))
        port = atoi(port_text);
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 0;
    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd == INVALID_SOCKET) goto done;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((u_short)port);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) goto done;

    memcpy(header, "MBDD", 4);
    encode_u32(header + 4, list->count);
    if (!send_full(socket_fd, header, sizeof(header))) goto done;
    for (i = 0; i < list->count; ++i) {
        size_t len = strlen(list->unix_paths[i]);
        unsigned char encoded_len[4];
        if (len > UINT32_MAX) goto done;
        encode_u32(encoded_len, (uint32_t)len);
        if (!send_full(socket_fd, encoded_len, sizeof(encoded_len)) ||
            !send_full(socket_fd, list->unix_paths[i], len)) goto done;
    }
    ok = recv(socket_fd, (char *)&response, 1, 0) == 1 && response == 1;

done:
    if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
    WSACleanup();
    return ok;
}

static HRESULT WINAPI hooked_do_drag_drop(IDataObject *object, IDropSource *source,
                                           DWORD allowed_effects, DWORD *effect)
{
    struct file_list files = {0};
    const char *kind = NULL;
    char line[128];
    int dropped;

    if (!object || !source || !effect)
        return original_do_drag_drop(object, source, allowed_effects, effect);
    log_line("drag: DoDragDrop called");
    if (materialize_descriptor_w(object, &files))
        kind = "FileGroupDescriptorW";
    else if (materialize_descriptor_a(object, &files))
        kind = "FileGroupDescriptor";
    else if (collect_hdrop(object, &files))
        kind = "CF_HDROP";
    if (!kind) {
        log_line("drag: no supported file format; using original OLE path");
        return original_do_drag_drop(object, source, allowed_effects, effect);
    }

    _snprintf(line, sizeof(line), "drag: %s count=%u", kind, files.count);
    log_line(line);
    set_copy_cursor();

    dropped = broker_drag(&files);
    if (dropped) {
        log_line("drag: native drop completed");
        *effect = DROPEFFECT_COPY & allowed_effects;
        free_file_list(&files);
        return DRAGDROP_S_DROP;
    }
    log_line("drag: native drop canceled or rejected");
    free_file_list(&files);
    *effect = DROPEFFECT_NONE;
    return DRAGDROP_S_CANCEL;
}

static int install_hook(void)
{
    HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
    BYTE *target;
    BYTE *trampoline;
    DWORD old_protection;
    intptr_t relative;

    if (!ole32) return 0;
    target = (BYTE *)GetProcAddress(ole32, "DoDragDrop");
    if (!target || target[0] != 0x55 || target[1] != 0x89 || target[2] != 0xe5) {
        log_line("hook: unsupported ole32 DoDragDrop prologue");
        return -1;
    }
    trampoline = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return -1;
    memcpy(trampoline, target, 5);
    trampoline[5] = 0xe9;
    relative = (target + 5) - (trampoline + 10);
    *(int32_t *)(trampoline + 6) = (int32_t)relative;
    original_do_drag_drop = (do_drag_drop_fn)trampoline;

    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protection)) return -1;
    target[0] = 0xe9;
    relative = (BYTE *)hooked_do_drag_drop - (target + 5);
    *(int32_t *)(target + 1) = (int32_t)relative;
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    VirtualProtect(target, 5, old_protection, &old_protection);
    log_line("hook: DoDragDrop active");
    return 1;
}

static DWORD WINAPI hook_thread(void *unused)
{
    int attempts;
    (void)unused;
    for (attempts = 0; attempts < 600; ++attempts) {
        int result = install_hook();
        if (result != 0) return 0;
        Sleep(100);
    }
    log_line("hook: ole32 not loaded");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        DisableThreadLibraryCalls(instance);
        thread = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, WCHAR *command_line, int show)
{
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;
    return 0;
}
