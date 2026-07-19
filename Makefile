CC ?= cc
MINGW_CC ?= i686-w64-mingw32-gcc
MCS ?= mcs
CFLAGS ?= -O2 -Wall -Wextra -Werror
WINEPREFIX ?= $(HOME)/.wine-mailbird
MAILBIRD_DIR ?= $(WINEPREFIX)/drive_c/Program Files/Mailbird
GONG_SOURCE ?= $(MAILBIRD_DIR)/GongSolutions.Wpf.DragDrop.dll
WPF_DIR ?= $(WINEPREFIX)/drive_c/windows/Microsoft.NET/Framework/v4.0.30319/WPF
CECIL ?= /usr/lib/mono/gac/Mono.Cecil/0.11.0.0__0738eb9f132ed756/Mono.Cecil.dll

.PHONY: all clean check integration-check managed

all: build/mailbird-dnd-broker build/mailbird-dnd-hook.dll build/mailbird-dnd-injector.exe managed

build:
	mkdir -p build

build/mailbird-dnd-broker: src/broker.c | build
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs x11 xcursor xfixes xext)

build/mailbird-dnd-hook.dll: src/hook.c | build
	$(MINGW_CC) $(CFLAGS) -shared -static-libgcc -municode -o $@ $< \
		-lws2_32 -lole32 -lshell32 -luuid

build/mailbird-dnd-injector.exe: src/injector.c | build
	$(MINGW_CC) $(CFLAGS) -municode -o $@ $<

build/MailbirdDnd.Managed.dll: src/managed/EmailDragBridge.cs | build
	$(MCS) -sdk:4 -target:library -optimize+ -out:$@ $< \
		-r:"$(MAILBIRD_DIR)/GongSolutions.Wpf.DragDrop.dll" \
		-r:"$(WPF_DIR)/WindowsBase.dll" \
		-r:"$(WPF_DIR)/PresentationCore.dll" \
		-r:"$(WPF_DIR)/PresentationFramework.dll"

build/PatchGong.exe: tools/PatchGong.cs | build
	$(MCS) -sdk:4 -target:exe -optimize+ -out:$@ $< -r:"$(CECIL)"

build/GongSolutions.Wpf.DragDrop.patched.dll: build/PatchGong.exe build/MailbirdDnd.Managed.dll
	mono build/PatchGong.exe "$(GONG_SOURCE)" \
		build/MailbirdDnd.Managed.dll $@ "$(WPF_DIR)"

managed: build/MailbirdDnd.Managed.dll build/GongSolutions.Wpf.DragDrop.patched.dll

build/mbdd-test-source.exe: src/test_source.c | build
	$(MINGW_CC) $(CFLAGS) -municode -o $@ $< -lole32 -luuid -lgdi32

check: all
	./build/mailbird-dnd-broker --check
	file build/mailbird-dnd-hook.dll | grep -q 'PE32 executable.*Intel 80386'
	file build/mailbird-dnd-injector.exe | grep -q 'PE32 executable.*Intel 80386'

integration-check: check build/mbdd-test-source.exe
	@echo 'Virtual-file integration test source built.'

clean:
	rm -rf build
