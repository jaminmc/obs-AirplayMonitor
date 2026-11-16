#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
rm -rf /tmp/airplay-pairing-test
mkdir -p /tmp/airplay-pairing-test
export AIRPLAY_PAIRING_DIR=/tmp/airplay-pairing-test
clang -std=c11 -w -c third_party/tweetnacl/tweetnacl.c -o /tmp/tweetnacl.o
clang -std=c11 -c src/airplay/nacl_random.c -o /tmp/nacl_random.o
clang -std=c11 -O2 -c third_party/fairplay-sap/ports/c/fairplay_sapcore.c -o /tmp/fairplay_sapcore.o
clang++ -std=c++20 -O2 -DAIRPLAY_FAIRPLAY_STUB -Isrc -Ithird_party/fairplay-sap/ports \
	tests/test_protocol.cpp \
	third_party/fairplay-sap/ports/cpp/fairplay_sender.cpp \
	src/airplay/plist.cpp \
	src/airplay/features.cpp \
	src/airplay/mirror_packet.cpp \
	src/airplay/rtsp.cpp \
	src/airplay/ntp.cpp \
	src/airplay/rtp_audio.cpp \
	src/airplay/crypto.cpp \
	src/airplay/aead.cpp \
	src/airplay/pairing.cpp \
	src/airplay/fairplay.cpp \
	src/airplay/handshake.cpp \
	src/encode/bitstream.cpp \
	src/airplay/discovery.cpp \
	/tmp/tweetnacl.o \
	/tmp/nacl_random.o \
	/tmp/fairplay_sapcore.o \
	-framework CoreFoundation -framework Security \
	-o /tmp/airplay_protocol_tests
/tmp/airplay_protocol_tests
