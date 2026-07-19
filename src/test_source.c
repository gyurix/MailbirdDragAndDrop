#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <stdint.h>

static const WCHAR *test_names[] = {L"attachment.txt", L"attachment.txt", L"mail subject.eml"};
static const char *test_contents[] = {"first attachment\n", "second attachment\n",
                                      "From: sender@example.test\r\nSubject: mail subject\r\n\r\nbody\r\n"};

struct data_object {
    IDataObject IDataObject_iface;
    LONG refs;
};

struct drop_source {
    IDropSource IDropSource_iface;
    LONG refs;
};

static HRESULT WINAPI data_query_interface(IDataObject *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject)) {
        *out = iface;
        IDataObject_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_add_ref(IDataObject *iface)
{
    struct data_object *object = CONTAINING_RECORD(iface, struct data_object, IDataObject_iface);
    return (ULONG)InterlockedIncrement(&object->refs);
}

static ULONG WINAPI data_release(IDataObject *iface)
{
    struct data_object *object = CONTAINING_RECORD(iface, struct data_object, IDataObject_iface);
    return (ULONG)InterlockedDecrement(&object->refs);
}

static HRESULT descriptor_data(STGMEDIUM *medium)
{
    const UINT count = (UINT)(sizeof(test_names) / sizeof(test_names[0]));
    SIZE_T size = sizeof(UINT) + count * sizeof(FILEDESCRIPTORW);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    FILEGROUPDESCRIPTORW *group;
    UINT i;

    if (!memory) return E_OUTOFMEMORY;
    group = GlobalLock(memory);
    group->cItems = count;
    for (i = 0; i < count; ++i) {
        size_t content_size = strlen(test_contents[i]);
        group->fgd[i].dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
        group->fgd[i].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        group->fgd[i].nFileSizeLow = (DWORD)content_size;
        lstrcpynW(group->fgd[i].cFileName, test_names[i], ARRAYSIZE(group->fgd[i].cFileName));
    }
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = NULL;
    return S_OK;
}

static HRESULT content_data(LONG index, STGMEDIUM *medium)
{
    const UINT count = (UINT)(sizeof(test_contents) / sizeof(test_contents[0]));
    size_t size;
    HGLOBAL memory;
    void *data;
    IStream *stream;
    HRESULT hr;

    if (index < 0 || (UINT)index >= count) return DV_E_LINDEX;
    size = strlen(test_contents[index]);
    memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return E_OUTOFMEMORY;
    data = GlobalLock(memory);
    memcpy(data, test_contents[index], size);
    GlobalUnlock(memory);
    hr = CreateStreamOnHGlobal(memory, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(memory);
        return hr;
    }
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = stream;
    medium->pUnkForRelease = NULL;
    return S_OK;
}

static HRESULT WINAPI data_get_data(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
    CLIPFORMAT descriptor = (CLIPFORMAT)RegisterClipboardFormatW(L"FileGroupDescriptorW");
    CLIPFORMAT contents = (CLIPFORMAT)RegisterClipboardFormatW(L"FileContents");
    (void)iface;
    memset(medium, 0, sizeof(*medium));
    if (format->cfFormat == descriptor && (format->tymed & TYMED_HGLOBAL))
        return descriptor_data(medium);
    if (format->cfFormat == contents && (format->tymed & TYMED_ISTREAM))
        return content_data(format->lindex, medium);
    return DV_E_FORMATETC;
}

static HRESULT WINAPI data_get_data_here(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
    (void)iface;
    (void)format;
    (void)medium;
    return E_NOTIMPL;
}

static HRESULT WINAPI data_query_get_data(IDataObject *iface, FORMATETC *format)
{
    STGMEDIUM medium;
    HRESULT hr = data_get_data(iface, format, &medium);
    if (SUCCEEDED(hr)) ReleaseStgMedium(&medium);
    return hr;
}

static HRESULT WINAPI data_canonical(IDataObject *iface, FORMATETC *input, FORMATETC *output)
{
    (void)iface;
    (void)input;
    output->ptd = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI data_set_data(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium,
                                    BOOL release)
{
    (void)iface;
    (void)format;
    (void)medium;
    (void)release;
    return E_NOTIMPL;
}

static HRESULT WINAPI data_enum_formats(IDataObject *iface, DWORD direction, IEnumFORMATETC **formats)
{
    (void)iface;
    (void)direction;
    (void)formats;
    return E_NOTIMPL;
}

static HRESULT WINAPI data_advise(IDataObject *iface, FORMATETC *format, DWORD flags,
                                  IAdviseSink *sink, DWORD *connection)
{
    (void)iface; (void)format; (void)flags; (void)sink; (void)connection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_unadvise(IDataObject *iface, DWORD connection)
{
    (void)iface; (void)connection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_enum_advise(IDataObject *iface, IEnumSTATDATA **enumerator)
{
    (void)iface; (void)enumerator;
    return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl data_vtable = {
    data_query_interface, data_add_ref, data_release, data_get_data, data_get_data_here,
    data_query_get_data, data_canonical, data_set_data, data_enum_formats, data_advise,
    data_unadvise, data_enum_advise
};

static HRESULT WINAPI source_query_interface(IDropSource *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropSource)) {
        *out = iface;
        IDropSource_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI source_add_ref(IDropSource *iface)
{
    struct drop_source *source = CONTAINING_RECORD(iface, struct drop_source, IDropSource_iface);
    return (ULONG)InterlockedIncrement(&source->refs);
}

static ULONG WINAPI source_release(IDropSource *iface)
{
    struct drop_source *source = CONTAINING_RECORD(iface, struct drop_source, IDropSource_iface);
    return (ULONG)InterlockedDecrement(&source->refs);
}

static HRESULT WINAPI source_continue(IDropSource *iface, BOOL escape, DWORD key_state)
{
    (void)iface;
    if (escape) return DRAGDROP_S_CANCEL;
    return (key_state & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
}

static HRESULT WINAPI source_feedback(IDropSource *iface, DWORD effect)
{
    (void)iface; (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl source_vtable = {
    source_query_interface, source_add_ref, source_release, source_continue, source_feedback
};

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;
    if (message == WM_LBUTTONDOWN) {
        struct data_object object = {{&data_vtable}, 1};
        struct drop_source source = {{&source_vtable}, 1};
        DWORD effect = DROPEFFECT_NONE;
        HRESULT hr = DoDragDrop(&object.IDataObject_iface, &source.IDropSource_iface,
                                DROPEFFECT_COPY, &effect);
        PostMessageW(window, WM_CLOSE, (WPARAM)hr, 0);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        TextOutW(dc, 30, 50, L"Press and drag virtual files", 28);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(void)
{
    WNDCLASSW window_class;
    HWND window;
    MSG message;

    if (FAILED(OleInitialize(NULL))) return 1;
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.lpszClassName = L"MbddVirtualFileTest";
    RegisterClassW(&window_class);
    window = CreateWindowW(window_class.lpszClassName, L"Mailbird DND virtual-file test",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 420, 180,
                           NULL, NULL, window_class.hInstance, NULL);
    if (!window) return 1;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    OleUninitialize();
    return 0;
}
