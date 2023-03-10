all:
	meson build
	meson compile -C build

clean:
	rm -rf build

reconfigure:
	meson --reconfigure build
	meson compile -C build

test-install:
	umount -l /usr || true
	cp -f build/libgs_plugin_vanilla_meta.so /usr/lib/x86_64-linux-gnu/gnome-software/plugins-19/.

install:
	cp build/libgs_plugin_vanilla_meta.so /tmp/libgs_plugin_vanilla_meta.so
	abroot exec cp /tmp/libgs_plugin_vanilla_meta.so /usr/lib/x86_64-linux-gnu/gnome-software/plugins-19/libgs_plugin_vanilla_meta.so
