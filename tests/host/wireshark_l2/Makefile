CC ?= cc
ROOT := ../../..
COMPONENT := $(ROOT)/firmware/components/wireshark_l2
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -pedantic
SAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test clean

all: test_wireshark_l2

test_wireshark_l2: test_wireshark_l2.c \
	$(COMPONENT)/wireshark_ble.c \
	$(COMPONENT)/wireshark_wifi.c \
	$(COMPONENT)/include/wireshark_l2.h
	$(CC) $(CFLAGS) $(SAN_FLAGS) \
		-I$(COMPONENT)/include \
		test_wireshark_l2.c \
		$(COMPONENT)/wireshark_ble.c \
		$(COMPONENT)/wireshark_wifi.c \
		-o $@

test: test_wireshark_l2
	./test_wireshark_l2

clean:
	rm -f test_wireshark_l2
