CC = gcc

CFLAGS := -I$(CURDIR) $(shell pkg-config --cflags libpulse pulsecore glib-2.0) -DHAVE_CONFIG_H -include $(CURDIR)/config.h -fPIC
LDFLAGS += $(shell pkg-config --libs libpulse pulsecore glib-2.0) -laudio-manager

COMMON_LIB := libpulse-audio-manager-common.so
COMMON_SRCS := \
	src/audio-manager-shared.c \
	src/audio-manager-bridge.c \
	src/audio-manager-jack.c \
	src/audio-manager-call.c \
	src/audio-manager-util.c \
	src/audio-manager-sink.c \
	src/audio-manager-source.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

SINK_SRCS := src/module-audio-manager-sink.c
SINK_OBJS := $(SINK_SRCS:.c=.o)

SOURCE_SRCS := src/module-audio-manager-source.c
SOURCE_OBJS := $(SOURCE_SRCS:.c=.o)

CARD_SRCS := src/module-audio-manager-card.c
CARD_OBJS := $(CARD_SRCS:.c=.o)

MODULEDIR := `pkg-config --variable=modlibexecdir libpulse`

MODULES := module-audio-manager-sink.so module-audio-manager-source.so module-audio-manager-card.so

.PHONY: all clean install

all: $(COMMON_LIB) $(MODULES)

config.h: configure config.h.in
	CC="$(CC)" ./configure

$(COMMON_OBJS) $(SINK_OBJS) $(SOURCE_OBJS) $(CARD_OBJS): config.h

$(COMMON_LIB): $(COMMON_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

module-audio-manager-sink.so: $(SINK_OBJS) $(COMMON_LIB)
	$(CC) -shared -o $@ $(SINK_OBJS) -L. -lpulse-audio-manager-common -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

module-audio-manager-source.so: $(SOURCE_OBJS) $(COMMON_LIB)
	$(CC) -shared -o $@ $(SOURCE_OBJS) -L. -lpulse-audio-manager-common -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

module-audio-manager-card.so: $(CARD_OBJS) $(COMMON_LIB)
	$(CC) -shared -o $@ $(CARD_OBJS) -L. -lpulse-audio-manager-common -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(MODULEDIR)
	install -m 0755 $(COMMON_LIB) $(MODULES) $(DESTDIR)$(MODULEDIR)/

clean:
	rm -f $(COMMON_OBJS) $(SINK_OBJS) $(SOURCE_OBJS) $(CARD_OBJS) $(COMMON_LIB) $(MODULES) config.h
