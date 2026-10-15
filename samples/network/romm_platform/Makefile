compiler     := gcc
TARGET       := romm_platform_test

CORE_DIR = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

CC      := $(compiler)
flags   += -std=gnu99

# network/romm/romm_platform.c, the Windows and Linux side of the RomM
# frontend, and what it needs to link.
SOURCES_C := \
	romm_platform_test.c \
	$(CORE_DIR)/network/romm/romm_platform.c \
	$(LIBRETRO_COMM_DIR)/crypto/crypto.c \
	$(LIBRETRO_COMM_DIR)/crypto/kdf.c \
	$(LIBRETRO_COMM_DIR)/hash/lrc_hash.c \
	$(LIBRETRO_COMM_DIR)/utils/md5.c \
	$(LIBRETRO_COMM_DIR)/features/features_cpu.c \
	$(LIBRETRO_COMM_DIR)/streams/file_stream.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
	$(LIBRETRO_COMM_DIR)/file/file_path.c \
	$(LIBRETRO_COMM_DIR)/file/file_path_io.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/string/rstrtod.c \
	$(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c

CFLAGS += -Wall $(INCDIRS)
CFLAGS += -DHAVE_CRYPTO

ifneq ($(SAN),)
   SANITIZER := $(SAN)
endif
ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif
ifneq ($(findstring mingw,$(compiler)),)
   TARGET  := $(TARGET).exe
endif

OBJS := $(SOURCES_C:.c=.o)

all: $(TARGET)

%.o: %.c
	$(CC) $(INCDIRS) $(CFLAGS) $(flags) -c -o$@ $<

$(TARGET): $(OBJS)
	$(CC) -o$@ $(OBJS) $(LDFLAGS)

check: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS)
	rm -f romm_platform_test romm_platform_test.exe
	rm -rf romm_platform_test_bin

.PHONY: clean all check
