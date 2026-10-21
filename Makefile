CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS := -lm

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  CFLAGS += -DWWV_NO_ALSA
else
  HAVE_ALSA := $(shell pkg-config --exists alsa 2>/dev/null && echo yes)
  ifeq ($(HAVE_ALSA),yes)
    LDLIBS += -lasound
  else
    CFLAGS += -DWWV_NO_ALSA
  endif
endif

all: wwvdecode
wwvdecode: wwvdecode.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
noalsa: wwvdecode.c
	$(CC) -O2 -Wall -Wextra -DWWV_NO_ALSA -o wwvdecode $< -lm

test/test_core: test/test_core.c wwvdecode.c
	$(CC) -O2 -Wall -Wextra -Werror -DWWV_NO_ALSA -o $@ $< -lm
test: wwvdecode test/test_core
	./test/test_core
	$(PYTHON) -m unittest discover -s test -v

clean:
	rm -f wwvdecode test/test_core
.PHONY: all noalsa test clean
