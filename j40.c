#define J40_IMPLEMENTATION
#include "j40.h"

int main(int argc, char **argv) {
	if (argc < 2) return 1;
	if (argc > 2) dumppath = argv[2];

	j40_err ret = j40_from_file(argv[1]);
	if (ret) {
		fprintf(stderr, "error: %c%c%c%c\n", ret >> 24 & 0xff, ret >> 16 & 0xff, ret >> 8 & 0xff, ret & 0xff);
		return 1;
	}
	fprintf(stderr, "ok\n");
	return 0;
}