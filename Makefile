CFLAGS_WARN=-W -Wall -Wconversion
CFLAGS=-O3 $(CFLAGS_WARN)
LDFLAGS=-lm

.PHONY: all
all: dj40

.PHONY: clean
clean:
	$(RM) -f dj40 dj40-o0g j40-fuzz

dj40: dj40.c j40.h extra/stb_image_write.h
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

dj40-o0g: dj40.c j40.h extra/stb_image_write.h
	$(CC) -g -O0 -fsanitize=address,undefined -DJ40_DEBUG $(CFLAGS_WARN) $< $(LDFLAGS) -o $@

j40-fuzz: extra/j40-fuzz.c j40.h
	clang -g -O1 -fsanitize=fuzzer,address,undefined -DJ40_DEBUG $(CFLAGS_WARN) $< $(LDFLAGS) -o $@

