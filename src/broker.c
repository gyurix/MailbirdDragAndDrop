#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 45981
#define MAX_FILES 4096U
#define MAX_PATH_BYTES (1024U * 1024U)
#define XDND_VERSION 5L

struct atoms {
    Atom xdnd_aware;
    Atom xdnd_proxy;
    Atom xdnd_enter;
    Atom xdnd_position;
    Atom xdnd_status;
    Atom xdnd_leave;
    Atom xdnd_drop;
    Atom xdnd_finished;
    Atom xdnd_selection;
    Atom xdnd_type_list;
    Atom xdnd_action_copy;
    Atom targets;
    Atom timestamp;
    Atom text_uri_list;
    Atom gnome_copied_files;
};

struct payload {
    char **paths;
    uint32_t count;
    char *uri_list;
    size_t uri_list_len;
    char *gnome_list;
    size_t gnome_list_len;
};

struct cursor_overlay {
    Window window;
    int xhot;
    int yhot;
    int hidden;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int read_full(int fd, void *buffer, size_t size)
{
    unsigned char *cursor = buffer;

    while (size) {
        ssize_t got = read(fd, cursor, size);
        if (got == 0) return 0;
        if (got < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        cursor += got;
        size -= (size_t)got;
    }
    return 1;
}

static uint32_t decode_u32(const unsigned char bytes[4])
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void encode_u32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

static int write_full(int fd, const void *buffer, size_t size)
{
    const unsigned char *cursor = buffer;

    while (size) {
        ssize_t sent = write(fd, cursor, size);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        cursor += sent;
        size -= (size_t)sent;
    }
    return 1;
}

static int uri_safe(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static char *path_to_uri(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t input_len = strlen(path);
    size_t capacity = 7 + input_len * 3 + 1;
    char *uri = malloc(capacity);
    char *out;
    const unsigned char *in;

    if (!uri || path[0] != '/') {
        free(uri);
        return NULL;
    }
    memcpy(uri, "file://", 7);
    out = uri + 7;
    for (in = (const unsigned char *)path; *in; ++in) {
        if (uri_safe(*in)) {
            *out++ = (char)*in;
        } else {
            *out++ = '%';
            *out++ = hex[*in >> 4];
            *out++ = hex[*in & 15];
        }
    }
    *out = 0;
    return uri;
}

static void free_payload(struct payload *payload)
{
    uint32_t i;

    for (i = 0; i < payload->count; ++i) free(payload->paths[i]);
    free(payload->paths);
    free(payload->uri_list);
    free(payload->gnome_list);
    memset(payload, 0, sizeof(*payload));
}

static int build_lists(struct payload *payload)
{
    char **uris = calloc(payload->count, sizeof(*uris));
    size_t uri_size = 1;
    size_t gnome_size = 6;
    uint32_t i;
    char *uri_cursor;
    char *gnome_cursor;

    if (!uris) return 0;
    for (i = 0; i < payload->count; ++i) {
        uris[i] = path_to_uri(payload->paths[i]);
        if (!uris[i]) goto fail;
        uri_size += strlen(uris[i]) + 2;
        gnome_size += strlen(uris[i]) + 1;
    }
    payload->uri_list = malloc(uri_size);
    payload->gnome_list = malloc(gnome_size);
    if (!payload->uri_list || !payload->gnome_list) goto fail;

    uri_cursor = payload->uri_list;
    gnome_cursor = payload->gnome_list;
    memcpy(gnome_cursor, "copy\n", 5);
    gnome_cursor += 5;
    for (i = 0; i < payload->count; ++i) {
        size_t len = strlen(uris[i]);
        memcpy(uri_cursor, uris[i], len);
        uri_cursor += len;
        memcpy(uri_cursor, "\r\n", 2);
        uri_cursor += 2;
        memcpy(gnome_cursor, uris[i], len);
        gnome_cursor += len;
        *gnome_cursor++ = '\n';
    }
    *uri_cursor = 0;
    *gnome_cursor = 0;
    payload->uri_list_len = (size_t)(uri_cursor - payload->uri_list);
    payload->gnome_list_len = (size_t)(gnome_cursor - payload->gnome_list);
    for (i = 0; i < payload->count; ++i) free(uris[i]);
    free(uris);
    return 1;

fail:
    for (i = 0; i < payload->count; ++i) free(uris[i]);
    free(uris);
    return 0;
}

static int receive_payload(int fd, struct payload *payload)
{
    unsigned char header[8];
    uint32_t i;

    memset(payload, 0, sizeof(*payload));
    if (!read_full(fd, header, sizeof(header)) || memcmp(header, "MBDD", 4)) return 0;
    payload->count = decode_u32(header + 4);
    if (!payload->count || payload->count > MAX_FILES) return 0;
    payload->paths = calloc(payload->count, sizeof(*payload->paths));
    if (!payload->paths) return 0;

    for (i = 0; i < payload->count; ++i) {
        unsigned char encoded_len[4];
        uint32_t len;
        if (!read_full(fd, encoded_len, sizeof(encoded_len))) return 0;
        len = decode_u32(encoded_len);
        if (!len || len > MAX_PATH_BYTES) return 0;
        payload->paths[i] = malloc((size_t)len + 1);
        if (!payload->paths[i] || !read_full(fd, payload->paths[i], len)) return 0;
        payload->paths[i][len] = 0;
        if (payload->paths[i][0] != '/') return 0;
    }
    return build_lists(payload);
}

static Atom intern(Display *display, const char *name)
{
    return XInternAtom(display, name, False);
}

static void init_atoms(Display *display, struct atoms *atoms)
{
    atoms->xdnd_aware = intern(display, "XdndAware");
    atoms->xdnd_proxy = intern(display, "XdndProxy");
    atoms->xdnd_enter = intern(display, "XdndEnter");
    atoms->xdnd_position = intern(display, "XdndPosition");
    atoms->xdnd_status = intern(display, "XdndStatus");
    atoms->xdnd_leave = intern(display, "XdndLeave");
    atoms->xdnd_drop = intern(display, "XdndDrop");
    atoms->xdnd_finished = intern(display, "XdndFinished");
    atoms->xdnd_selection = intern(display, "XdndSelection");
    atoms->xdnd_type_list = intern(display, "XdndTypeList");
    atoms->xdnd_action_copy = intern(display, "XdndActionCopy");
    atoms->targets = intern(display, "TARGETS");
    atoms->timestamp = intern(display, "TIMESTAMP");
    atoms->text_uri_list = intern(display, "text/uri-list");
    atoms->gnome_copied_files = intern(display, "x-special/gnome-copied-files");
}

static Window child_at_pointer(Display *display, Window root, int *root_x, int *root_y,
                               unsigned int *mask)
{
    Window current = root;
    Window root_return;
    Window child;
    int win_x;
    int win_y;

    if (!XQueryPointer(display, root, &root_return, &child, root_x, root_y,
                       &win_x, &win_y, mask)) return None;
    while (child != None) {
        current = child;
        if (!XQueryPointer(display, current, &root_return, &child, root_x, root_y,
                           &win_x, &win_y, mask)) break;
    }
    return current == root ? None : current;
}

static Window parent_window(Display *display, Window window)
{
    Window root;
    Window parent;
    Window *children = NULL;
    unsigned int count;

    if (!window || !XQueryTree(display, window, &root, &parent, &children, &count)) return None;
    if (children) XFree(children);
    return parent;
}

static int property_window(Display *display, Window window, Atom property, Window *value)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long remaining;
    unsigned char *data = NULL;
    int ok = 0;

    if (XGetWindowProperty(display, window, property, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &count, &remaining, &data) == Success &&
        actual_type == XA_WINDOW && actual_format == 32 && count == 1) {
        *value = *(Window *)data;
        ok = 1;
    }
    if (data) XFree(data);
    return ok;
}

static int has_aware(Display *display, Window window, Atom aware)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long remaining;
    unsigned char *data = NULL;
    int ok;

    ok = XGetWindowProperty(display, window, aware, 0, 1, False, XA_ATOM,
                            &actual_type, &actual_format, &count, &remaining, &data) == Success &&
         actual_type != None && count >= 1;
    if (data) XFree(data);
    return ok;
}

static Window find_target(Display *display, Window deepest, const struct atoms *atoms)
{
    Window window = deepest;
    Window proxy;

    while (window != None) {
        if (has_aware(display, window, atoms->xdnd_aware)) {
            if (property_window(display, window, atoms->xdnd_proxy, &proxy) &&
                property_window(display, proxy, atoms->xdnd_proxy, &window) && window == proxy)
                return proxy;
            return window;
        }
        window = parent_window(display, window);
    }
    return None;
}

static void send_client(Display *display, Window destination, Window source, Atom type,
                        long d1, long d2, long d3, long d4)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = destination;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)source;
    event.xclient.data.l[1] = d1;
    event.xclient.data.l[2] = d2;
    event.xclient.data.l[3] = d3;
    event.xclient.data.l[4] = d4;
    XSendEvent(display, destination, False, NoEventMask, &event);
    XFlush(display);
}

static void send_enter(Display *display, Window target, Window source, const struct atoms *atoms)
{
    send_client(display, target, source, atoms->xdnd_enter,
                (XDND_VERSION << 24), (long)atoms->text_uri_list,
                (long)atoms->gnome_copied_files, 0);
}

static void send_position(Display *display, Window target, Window source, const struct atoms *atoms,
                          int root_x, int root_y)
{
    long packed = ((long)(root_x & 0xffff) << 16) | (long)(root_y & 0xffff);
    send_client(display, target, source, atoms->xdnd_position, 0, packed,
                (long)CurrentTime, (long)atoms->xdnd_action_copy);
}

static void selection_reply(Display *display, const XSelectionRequestEvent *request,
                            const struct atoms *atoms, const struct payload *payload)
{
    XEvent response;
    Atom property = request->property == None ? request->target : request->property;
    Atom response_property = property;

    if (request->target == atoms->targets) {
        Atom offered[] = {atoms->targets, atoms->timestamp, atoms->text_uri_list,
                          atoms->gnome_copied_files};
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)offered,
                        (int)(sizeof(offered) / sizeof(offered[0])));
    } else if (request->target == atoms->text_uri_list) {
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (unsigned char *)payload->uri_list,
                        (int)payload->uri_list_len);
    } else if (request->target == atoms->gnome_copied_files) {
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (unsigned char *)payload->gnome_list,
                        (int)payload->gnome_list_len);
    } else if (request->target == atoms->timestamp) {
        Time now = CurrentTime;
        XChangeProperty(display, request->requestor, property, XA_INTEGER, 32,
                        PropModeReplace, (unsigned char *)&now, 1);
    } else {
        response_property = None;
    }

    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = response_property;
    response.xselection.time = request->time;
    XSendEvent(display, request->requestor, False, NoEventMask, &response);
    XFlush(display);
}

static void sleep_millis(long millis)
{
    struct timespec duration = {millis / 1000, (millis % 1000) * 1000000L};
    while (nanosleep(&duration, &duration) < 0 && errno == EINTR) {}
}

static unsigned long cursor_component(unsigned int component, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long maximum;

    if (!mask) return 0;
    while (!(mask & 1UL)) {
        mask >>= 1;
        ++shift;
    }
    maximum = mask;
    return (((unsigned long)component * maximum + 127UL) / 255UL) << shift;
}

static int create_cursor_overlay(Display *display, Window root, struct cursor_overlay *overlay)
{
    XcursorImage *cursor_image;
    XSetWindowAttributes attributes;
    XImage *window_image;
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    int depth = DefaultDepth(display, DefaultScreen(display));
    Pixmap bounding_mask = None;
    Pixmap input_mask = None;
    GC mask_gc = NULL;
    unsigned int x;
    unsigned int y;
    int event_base;
    int error_base;

    memset(overlay, 0, sizeof(*overlay));
    if (!XFixesQueryExtension(display, &event_base, &error_base) ||
        !XShapeQueryExtension(display, &event_base, &error_base)) return 0;
    cursor_image = XcursorLibraryLoadImage("dnd-copy", NULL, 32);
    if (!cursor_image) cursor_image = XcursorLibraryLoadImage("copy", NULL, 32);
    if (!cursor_image) return 0;

    attributes.override_redirect = True;
    attributes.background_pixel = 0;
    overlay->window = XCreateWindow(display, root, -100, -100,
                                    cursor_image->width, cursor_image->height, 0,
                                    depth, InputOutput, visual,
                                    CWOverrideRedirect | CWBackPixel, &attributes);
    if (!overlay->window) goto fail;
    window_image = XCreateImage(display, visual, (unsigned int)depth, ZPixmap, 0, NULL,
                                cursor_image->width, cursor_image->height, 32, 0);
    if (!window_image) goto fail;
    window_image->data = calloc((size_t)window_image->bytes_per_line, cursor_image->height);
    if (!window_image->data) {
        XDestroyImage(window_image);
        goto fail;
    }
    bounding_mask = XCreatePixmap(display, root, cursor_image->width, cursor_image->height, 1);
    input_mask = XCreatePixmap(display, root, cursor_image->width, cursor_image->height, 1);
    if (bounding_mask == None || input_mask == None) {
        XDestroyImage(window_image);
        goto fail;
    }
    mask_gc = XCreateGC(display, bounding_mask, 0, NULL);
    if (!mask_gc) {
        XDestroyImage(window_image);
        goto fail;
    }
    XSetForeground(display, mask_gc, 0);
    XFillRectangle(display, bounding_mask, mask_gc, 0, 0,
                   cursor_image->width, cursor_image->height);
    XFillRectangle(display, input_mask, mask_gc, 0, 0,
                   cursor_image->width, cursor_image->height);
    XSetForeground(display, mask_gc, 1);
    for (y = 0; y < cursor_image->height; ++y) {
        for (x = 0; x < cursor_image->width; ++x) {
            XcursorPixel pixel = cursor_image->pixels[y * cursor_image->width + x];
            unsigned int alpha = pixel >> 24;
            unsigned int red = (pixel >> 16) & 0xffU;
            unsigned int green = (pixel >> 8) & 0xffU;
            unsigned int blue = pixel & 0xffU;
            unsigned long output = cursor_component(red, visual->red_mask) |
                                   cursor_component(green, visual->green_mask) |
                                   cursor_component(blue, visual->blue_mask);
            XPutPixel(window_image, (int)x, (int)y, output);
            if (alpha >= 128U) XDrawPoint(display, bounding_mask, mask_gc, (int)x, (int)y);
        }
    }
    XShapeCombineMask(display, overlay->window, ShapeBounding, 0, 0, bounding_mask, ShapeSet);
    XShapeCombineMask(display, overlay->window, ShapeInput, 0, 0, input_mask, ShapeSet);
    overlay->xhot = (int)cursor_image->xhot;
    overlay->yhot = (int)cursor_image->yhot;
    XMapRaised(display, overlay->window);
    XPutImage(display, overlay->window, DefaultGC(display, DefaultScreen(display)),
              window_image, 0, 0, 0, 0, cursor_image->width, cursor_image->height);
    XFixesHideCursor(display, root);
    overlay->hidden = 1;
    XFlush(display);

    XFreeGC(display, mask_gc);
    XFreePixmap(display, bounding_mask);
    XFreePixmap(display, input_mask);
    XDestroyImage(window_image);
    XcursorImageDestroy(cursor_image);
    return 1;

fail:
    if (mask_gc) XFreeGC(display, mask_gc);
    if (bounding_mask != None) XFreePixmap(display, bounding_mask);
    if (input_mask != None) XFreePixmap(display, input_mask);
    if (overlay->window) XDestroyWindow(display, overlay->window);
    XcursorImageDestroy(cursor_image);
    memset(overlay, 0, sizeof(*overlay));
    return 0;
}

static void move_cursor_overlay(Display *display, const struct cursor_overlay *overlay,
                                int root_x, int root_y)
{
    if (overlay->window)
    {
        XMoveWindow(display, overlay->window,
                    root_x - overlay->xhot, root_y - overlay->yhot);
        XRaiseWindow(display, overlay->window);
        XFlush(display);
    }
}

static void destroy_cursor_overlay(Display *display, Window root,
                                   struct cursor_overlay *overlay)
{
    if (overlay->hidden) XFixesShowCursor(display, root);
    if (overlay->window) XDestroyWindow(display, overlay->window);
    XFlush(display);
    memset(overlay, 0, sizeof(*overlay));
}

static int perform_drag(Display *display, Window root, Window source, const struct atoms *atoms,
                        const struct payload *payload)
{
    struct cursor_overlay cursor_overlay;
    Window target = None;
    int accepted = 0;
    int button_seen = 0;
    int dropped = 0;
    int finished = 0;
    int success = 0;
    int iterations = 0;

    XSetSelectionOwner(display, atoms->xdnd_selection, source, CurrentTime);
    if (XGetSelectionOwner(display, atoms->xdnd_selection) != source) return 0;
    if (!create_cursor_overlay(display, root, &cursor_overlay))
        fprintf(stderr, "mailbird-dnd: copy cursor overlay unavailable\n");

    while (running && !finished) {
        int root_x = 0;
        int root_y = 0;
        unsigned int mask = 0;
        Window deepest;
        Window new_target;

        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionRequest) {
                selection_reply(display, &event.xselectionrequest, atoms, payload);
            } else if (event.type == ClientMessage &&
                       event.xclient.message_type == atoms->xdnd_status &&
                       (Window)event.xclient.data.l[0] == target) {
                accepted = (event.xclient.data.l[1] & 1L) != 0;
            } else if (event.type == ClientMessage &&
                       event.xclient.message_type == atoms->xdnd_finished && dropped) {
                success = (event.xclient.data.l[1] & 1L) != 0;
                finished = 1;
            }
        }
        if (finished) break;

        deepest = child_at_pointer(display, root, &root_x, &root_y, &mask);
        move_cursor_overlay(display, &cursor_overlay, root_x, root_y);
        new_target = find_target(display, deepest, atoms);
        if (new_target == source) new_target = None;

        if (!dropped && new_target != target) {
            if (target != None)
                send_client(display, target, source, atoms->xdnd_leave, 0, 0, 0, 0);
            target = new_target;
            accepted = 0;
            if (target != None) send_enter(display, target, source, atoms);
        }
        if (!dropped && target != None)
            send_position(display, target, source, atoms, root_x, root_y);

        if (mask & (Button1Mask | Button2Mask | Button3Mask)) button_seen = 1;
        if (!dropped && button_seen && !(mask & (Button1Mask | Button2Mask | Button3Mask))) {
            if (target != None && accepted) {
                send_client(display, target, source, atoms->xdnd_drop, 0,
                            (long)CurrentTime, 0, 0);
                dropped = 1;
                iterations = 0;
            } else {
                if (target != None)
                    send_client(display, target, source, atoms->xdnd_leave, 0, 0, 0, 0);
                finished = 1;
            }
        }
        if (!button_seen && ++iterations > 100) finished = 1;
        if (dropped && ++iterations > 500) finished = 1;
        sleep_millis(20);
    }

    destroy_cursor_overlay(display, root, &cursor_overlay);
    XSetSelectionOwner(display, atoms->xdnd_selection, None, CurrentTime);
    XFlush(display);
    return success;
}

static int create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;
    struct sockaddr_in address;

    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_test_drag(int port, const char *path)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    unsigned char header[8];
    unsigned char encoded_len[4];
    unsigned char response = 0;
    size_t len = strlen(path);

    if (fd < 0 || path[0] != '/' || len > UINT32_MAX) return 2;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) goto fail;
    memcpy(header, "MBDD", 4);
    encode_u32(header + 4, 1);
    encode_u32(encoded_len, (uint32_t)len);
    if (!write_full(fd, header, sizeof(header)) ||
        !write_full(fd, encoded_len, sizeof(encoded_len)) ||
        !write_full(fd, path, len) || !read_full(fd, &response, 1)) goto fail;
    close(fd);
    return response == 1 ? 0 : 1;

fail:
    close(fd);
    return 2;
}

static int self_check(void)
{
    char *uri = path_to_uri("/tmp/a b/#mail.eml");
    int ok = uri && !strcmp(uri, "file:///tmp/a%20b/%23mail.eml");
    free(uri);
    puts(ok ? "broker check passed" : "broker check failed");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    Display *display;
    int screen;
    Window root;
    Window source;
    struct atoms atoms;
    int listener;
    int port = DEFAULT_PORT;

    if (getenv("MAILBIRD_DND_PORT")) port = atoi(getenv("MAILBIRD_DND_PORT"));
    if (port < 1 || port > 65535) return 2;
    if (argc == 2 && !strcmp(argv[1], "--check")) return self_check();
    if (argc == 3 && !strcmp(argv[1], "--send")) return send_test_drag(port, argv[2]);

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    signal(SIGPIPE, SIG_IGN);
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "mailbird-dnd: cannot open X display\n");
        return 1;
    }
    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    source = XCreateSimpleWindow(display, root, -10, -10, 1, 1, 0, 0, 0);
    init_atoms(display, &atoms);
    {
        Atom types[] = {atoms.text_uri_list, atoms.gnome_copied_files};
        XChangeProperty(display, source, atoms.xdnd_type_list, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)types, 2);
    }
    listener = create_listener(port);
    if (listener < 0) {
        fprintf(stderr, "mailbird-dnd: cannot listen on 127.0.0.1:%d: %s\n",
                port, strerror(errno));
        XDestroyWindow(display, source);
        XCloseDisplay(display);
        return 1;
    }

    while (running) {
        struct pollfd poll_fd = {listener, POLLIN, 0};
        int result = poll(&poll_fd, 1, 1000);
        if (result > 0 && (poll_fd.revents & POLLIN)) {
            int client = accept(listener, NULL, NULL);
            if (client >= 0) {
                struct payload payload;
                unsigned char response = 0;
                if (receive_payload(client, &payload)) {
                    response = (unsigned char)perform_drag(display, root, source, &atoms, &payload);
                    free_payload(&payload);
                }
                if (write(client, &response, 1) < 0 && errno != EPIPE)
                    fprintf(stderr, "mailbird-dnd: response write failed: %s\n", strerror(errno));
                close(client);
            }
        }
    }

    close(listener);
    XDestroyWindow(display, source);
    XCloseDisplay(display);
    return 0;
}
