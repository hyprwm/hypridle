PREFIX = /usr

release:
	cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr/local -S . -B ./build
	cmake --build ./build --config Release --target all -j`nproc 2>/dev/null || getconf NPROCESSORS_CONF`

clear:
	rm -rf build

all:
	$(MAKE) clear
	$(MAKE) release

install:
	cmake --install ./build

uninstall:
	xargs rm < ./build/install_manifest.txt
