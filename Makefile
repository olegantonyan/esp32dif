IDF_ACTIVATE_PATH ?= $(HOME)/.espressif/tools/activate_idf_v6.1.sh
TTY_PORT ?= /dev/ttyACM0
ESP_HOST ?= esp32dif.local
ESP_PASSWORD ?= esp32dif-admin
IDF := . $(IDF_ACTIVATE_PATH) >/dev/null && idf.py

.PHONY: build flash ota monitor flash-monitor menuconfig clean fullclean

build:
	$(IDF) build

flash:
	$(IDF) -p $(TTY_PORT) flash

ota: build
	curl -fsS -u 'admin:$(ESP_PASSWORD)' -H 'Content-Type: application/octet-stream' --data-binary @build/esp32dif.bin http://$(ESP_HOST)/api/update
	@echo

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
