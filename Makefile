IDF_ACTIVATE_PATH ?= $(HOME)/.espressif/tools/activate_idf_v6.1.sh
TTY_PORT ?= /dev/ttyACM0
IDF := . $(IDF_ACTIVATE_PATH) >/dev/null && idf.py

.PHONY: build flash monitor flash-monitor menuconfig clean fullclean

build:
	$(IDF) build

flash:
	$(IDF) -p $(TTY_PORT) flash

monitor:
	$(IDF) -p $(TTY_PORT) monitor

flash-monitor:
	$(IDF) -p $(TTY_PORT) flash monitor

menuconfig:
	$(IDF) menuconfig

clean:
	$(IDF) clean

fullclean:
	$(IDF) fullclean
