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
wwvdecode: wwvdecode.c refclock.c refclock.h service.c service.h
	$(CC) $(CFLAGS) -o $@ $< refclock.c service.c $(LDLIBS)
noalsa: wwvdecode.c refclock.c refclock.h service.c service.h
	$(CC) -O2 -Wall -Wextra -DWWV_NO_ALSA -o wwvdecode $< refclock.c service.c -lm

test/test_core: test/test_core.c wwvdecode.c refclock.c refclock.h service.c service.h
	$(CC) -O2 -Wall -Wextra -Werror -DWWV_NO_ALSA -o $@ $< refclock.c service.c -lm
test/test_refclock: test/test_refclock.c refclock.c refclock.h service.c service.h
	$(CC) -O2 -Wall -Wextra -Werror -o $@ $< refclock.c service.c -lm
test: wwvdecode test/test_core test/test_refclock
	./test/test_core
	./test/test_refclock
	$(PYTHON) -m unittest discover -s test -v

clean:
	rm -f wwvdecode test/test_core test/test_refclock
.PHONY: all noalsa test clean
