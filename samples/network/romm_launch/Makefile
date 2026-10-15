compiler     := gcc
TARGET       := romm_launch_test

CORE_DIR = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

CC      := $(compiler)
flags   += -std=gnu99

# The test includes network/romm/romm_launch.c; everything below is
# what that translation unit needs to link. Requests go to the RomM
# client harness's stubs, which record them for the test to answer, and
# fetched files go to the real romm:// VFS.
SOURCES_C := \
	romm_launch_test.c \
	../romm_client/stubs_retroarch.c \
	$(CORE_DIR)/network/romm/romm_client.c \
	$(CORE_DIR)/network/romm/romm_json.c \
	$(LIBRETRO_COMM_DIR)/formats/json/rjson.c \
	$(LIBRETRO_COMM_DIR)/streams/file_stream.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation_romm.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_crc32.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_deflate.c \
	$(LIBRETRO_COMM_DIR)/file/archive_file.c \
	$(LIBRETRO_COMM_DIR)/file/archive_file_zlib.c \
	$(LIBRETRO_COMM_DIR)/file/archive_file_7z.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_archive.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_lzma.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_lzma_stream.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_lzma2.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_bcj2.c \
	$(LIBRETRO_COMM_DIR)/formats/7z/r7z_filters.c \
	$(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
	$(LIBRETRO_COMM_DIR)/file/file_path.c \
	$(LIBRETRO_COMM_DIR)/file/file_path_io.c \
	$(LIBRETRO_COMM_DIR)/file/retro_dirent.c \
	$(LIBRETRO_COMM_DIR)/lists/string_list.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/string/rstrtod.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strldup.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strcasestr.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_posix_string.c \
	$(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c

CFLAGS += -Wall $(INCDIRS)
CFLAGS += -DHAVE_NETWORKING -DHAVE_ROMM -DHAVE_THREADS -DHAVE_COMPRESSION -DHAVE_7ZIP
LDFLAGS += -lm -lpthread

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

# encoding_crc32.c picks its fast path by asking cpu_features_get(). The
# stubs keep the clock, so features_cpu.c comes in with its own renamed.
CPU_FEATURES_OBJ := features_cpu.o

OBJS := $(SOURCES_C:.c=.o) $(CPU_FEATURES_OBJ)

all: $(TARGET)

%.o: %.c
	$(CC) $(INCDIRS) $(CFLAGS) $(flags) -c -o$@ $<

$(CPU_FEATURES_OBJ): $(LIBRETRO_COMM_DIR)/features/features_cpu.c
	$(CC) $(INCDIRS) $(CFLAGS) $(flags) \
		-Dcpu_features_get_time_usec=features_cpu_time_usec -c -o$@ $<

$(TARGET): $(OBJS)
	$(CC) -o$@ $(OBJS) $(LDFLAGS)

check: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS)
	rm -f romm_launch_test romm_launch_test.exe
	rm -rf romm_launch_test_system

.PHONY: clean all check
