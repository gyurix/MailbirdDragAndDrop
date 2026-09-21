# Mailbird Drag and Drop for Linux

Drag Mailbird attachments and messages directly into native Linux file managers
while running Mailbird through Wine.

Wine can receive native Xdnd drops, but it does not send them from Windows
applications to Linux applications. This project bridges Mailbird's OLE drag
data to Xdnd. Attachments become regular files; dragged conversations become
`.eml` files containing their original message source.

> [!WARNING]
> This project injects a DLL into Mailbird and replaces Mailbird's copy of
> `GongSolutions.Wpf.DragDrop.dll` with a patched build. Back up that DLL before
> installation. Mailbird updates can overwrite the patch or change internals on
> which the bridge relies.

## How it works

1. `mailbird-dnd-injector.exe` loads `mailbird-dnd-hook.dll` into `Mailbird.exe`.
2. `MailbirdDnd.Managed.dll` adds exact `.eml` source files to conversation drag
   data.
3. The hook intercepts `DoDragDrop` and materializes OLE virtual attachments in
   a private staging directory.
4. `mailbird-dnd-broker` publishes those paths as `text/uri-list` and
   `x-special/gnome-copied-files` through Xdnd.

The broker also replaces Wine's fixed drag cursor with the desktop's
click-through `dnd-copy` cursor during the operation.

## Requirements

- Linux desktop with X11 or XWayland and an Xdnd-compatible target such as
  Thunar
- 32-bit Mailbird installation in a dedicated Wine prefix
- C compiler, `make`, `pkg-config`, and development libraries for X11, Xcursor,
  Xfixes, and Xext
- 32-bit MinGW-w64 C compiler (`i686-w64-mingw32-gcc`)
- Mono C# compiler/runtime and Mono.Cecil
- Mailbird's installed `GongSolutions.Wpf.DragDrop.dll`

Mailbird binaries and resources are not distributed by this repository.

## Build and test

The default prefix is `~/.wine-mailbird`. Override it when needed:

```sh
make check WINEPREFIX="$HOME/.wine-mailbird"
```

`make check` builds all native, Windows, and managed components, runs the broker
self-check, and verifies that the hook and injector are 32-bit PE files.

Useful targets:

```sh
make                 # build everything
make integration-check
make clean
```

`integration-check` also builds a small Windows OLE virtual-file drag source for
manual end-to-end testing.

## Install the managed bridge

Set the same prefix used for the build:

```sh
export WINEPREFIX="$HOME/.wine-mailbird"
MAILBIRD_DIR="$WINEPREFIX/drive_c/Program Files/Mailbird"

cp -n "$MAILBIRD_DIR/GongSolutions.Wpf.DragDrop.dll" \
  "$MAILBIRD_DIR/GongSolutions.Wpf.DragDrop.dll.mailbird-dnd-original"
install -m 0644 build/MailbirdDnd.Managed.dll "$MAILBIRD_DIR/"
install -m 0644 build/GongSolutions.Wpf.DragDrop.patched.dll \
  "$MAILBIRD_DIR/GongSolutions.Wpf.DragDrop.dll"
```

Keep the backup. Rebuild and reinstall the patched assembly after a Mailbird
update.

## Run

Mailbird and the injector must inherit the bridge environment:

```sh
export WINEPREFIX="$HOME/.wine-mailbird"
export MAILBIRD_DND_PREFIX_UNIX="$WINEPREFIX"
STAGE_DIR="${XDG_RUNTIME_DIR:-/tmp}/mailbird-dnd-$UID"
mkdir -p "$STAGE_DIR"
export MAILBIRD_DND_STAGE_WIN="$(winepath -w "$STAGE_DIR")"

./build/mailbird-dnd-broker &
BROKER_PID=$!

wine "$WINEPREFIX/drive_c/Program Files/Mailbird/Mailbird.exe" &
HOOK_WIN="$(winepath -w "$PWD/build/mailbird-dnd-hook.dll")"
wine build/mailbird-dnd-injector.exe "$HOOK_WIN"

wait "$BROKER_PID"
```

The injector waits up to 30 seconds for `Mailbird.exe`. Broker and hook use TCP
port `45981` on loopback; set `MAILBIRD_DND_PORT` in both environments to change
it.

Runtime logs are written to the staging directory as `hook.log` and
`managed.log`.

## Restore Mailbird

Close Mailbird, then restore the original assembly:

```sh
export WINEPREFIX="$HOME/.wine-mailbird"
MAILBIRD_DIR="$WINEPREFIX/drive_c/Program Files/Mailbird"
cp "$MAILBIRD_DIR/GongSolutions.Wpf.DragDrop.dll.mailbird-dnd-original" \
  "$MAILBIRD_DIR/GongSolutions.Wpf.DragDrop.dll"
```

You can then remove `MailbirdDnd.Managed.dll` from the Mailbird directory.

## Scope and limitations

- The hook expects Mailbird's 32-bit Wine process and a compatible
  `ole32!DoDragDrop` prologue.
- Conversation export reflects Mailbird's private managed object model and may
  need adjustment after Mailbird updates.
- The current broker implements Xdnd, not a native Wayland data source.
- Drag operations are copies only.

## IPv6-disabled Wine workaround

On systems booted with `ipv6.disable=1`, Mailbird's bundled Limilabs `Mail.dll`
may fail before connecting: its default socket constructor takes the dual-mode
IPv6 path even for IPv4 endpoints. `patch-mailbird.sh` makes only that
constructor explicitly use `AddressFamily.InterNetwork` (IPv4). It leaves the
kernel, DNS, TLS, OAuth data, and Mailbird database unchanged.

Close Mailbird before patching. The command creates a reversible backup beside
the assembly and verifies the result:

```sh
./patch-mailbird.sh apply \
  "$WINEPREFIX/drive_c/Program Files/Mailbird/Mail.dll"
./patch-mailbird.sh verify \
  "$WINEPREFIX/drive_c/Program Files/Mailbird/Mail.dll"
```

Restore the original with `./patch-mailbird.sh restore /path/to/Mail.dll`.
Mailbird binaries are not distributed. The patch was tested with Mailbird
2.9.111.0 under Wine 9.0 on a kernel booted with `ipv6.disable=1`.
