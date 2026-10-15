compiler     := gcc
TARGET       := romm_client_test

CORE_DIR = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

CC      := $(compiler)
flags   += -std=gnu99

# The test includes network/romm/romm_client.c; everything below is what
# that translation unit needs to link.
SOURCES_C := \
	romm_client_test.c \
	$(CORE_DIR)/network/romm/romm_json.c \
	stubs_retroarch.c \
	$(LIBRETRO_COMM_DIR)/formats/json/rjson.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/string/rstrtod.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c

CFLAGS += -Wall $(INCDIRS)
CFLAGS += -DHAVE_NETWORKING -DHAVE_ROMM
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
	rm -f romm_client_test romm_client_test.exe

.PHONY: clean all check
