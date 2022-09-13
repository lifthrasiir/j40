CFLAGS=-O3 -W -Wall -Wconversion
LDFLAGS=-lm

.PHONY: all
all: dj40

.PHONY: clean
clean:
	$(RM) -f dj40

dj40: dj40.c j40.h extra/stb_image_write.h
	$(CC) $(CFLAGS) dj40.c $(LDFLAGS) -o $@

