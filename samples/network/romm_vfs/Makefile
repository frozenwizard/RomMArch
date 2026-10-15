compiler     := gcc
TARGET       := romm_vfs_test

CORE_DIR = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

CC      := $(compiler)
flags   += -std=gnu99

# The romm:// files are reached the way RetroArch reaches them: through
# file_stream.c and file_path_io.c into vfs_implementation.c.
SOURCES_C := \
	romm_vfs_test.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation_romm.c \
	$(LIBRETRO_COMM_DIR)/streams/file_stream.c \
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
	$(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c

CFLAGS += -Wall $(INCDIRS)
CFLAGS += -DHAVE_ROMM -DHAVE_THREADS
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
	rm -f romm_vfs_test romm_vfs_test.exe

.PHONY: clean all check
