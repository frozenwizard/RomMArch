compiler     := gcc
TARGET       := romm_webdav_auth_test

CORE_DIR = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

CC      := $(compiler)
flags   += -std=gnu99

# The test includes network/cloud_sync/webdav.c built with HAVE_ROMM and
# links the webdav_reauth harness's stubs. Built in one step: settings_t
# has RomM's fields only with HAVE_ROMM, so no object may be shared with
# a harness built without it.
SOURCES_C := \
	romm_webdav_auth_test.c \
	../webdav_reauth/stubs_retroarch.c \
	$(LIBRETRO_COMM_DIR)/lists/string_list.c \
	$(LIBRETRO_COMM_DIR)/file/file_path.c \
	$(LIBRETRO_COMM_DIR)/streams/file_stream.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation_romm.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_base64.c \
	$(LIBRETRO_COMM_DIR)/hash/lrc_hash.c \
	$(LIBRETRO_COMM_DIR)/utils/md5.c \
	$(LIBRETRO_COMM_DIR)/features/features_cpu.c \
	$(LIBRETRO_COMM_DIR)/file/file_path_io.c \
	$(LIBRETRO_COMM_DIR)/net/net_http.c \
	$(LIBRETRO_COMM_DIR)/net/net_compat.c \
	$(LIBRETRO_COMM_DIR)/net/net_socket.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/string/rstrtod.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strldup.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_posix_string.c \
	$(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c

CFLAGS += -Wall $(INCDIRS)
CFLAGS += -DHAVE_NETWORKING -DHAVE_CLOUDSYNC -DHAVE_ROMM -D_GNU_SOURCE
LDFLAGS += -lm

ifneq ($(SAN),)
   SANITIZER := $(SAN)
endif
ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif
ifneq ($(findstring mingw,$(compiler)),)
   TARGET  := $(TARGET).exe
   LDFLAGS += -lws2_32 -static
endif

all: $(TARGET)

$(TARGET): $(SOURCES_C) $(CORE_DIR)/network/cloud_sync/webdav.c
	$(CC) $(CFLAGS) $(flags) -o$@ $(SOURCES_C) $(LDFLAGS)

check: $(TARGET)
	./$(TARGET)

clean:
	rm -f romm_webdav_auth_test romm_webdav_auth_test.exe

.PHONY: clean all check
