#define J40_CONFIRM_THAT_THIS_IS_EXPERIMENTAL_AND_POTENTIALLY_UNSAFE
#define J40_IMPLEMENTATION
#include "../j40.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	j40_image image;
	int ok;
	j40_from_memory(&image, (void*) data, size, NULL);
	j40_output_format(&image, J40_RGBA, J40_U8X4);
	if (j40_next_frame(&image)) j40_current_frame(&image);
	ok = !j40_error(&image);
	j40_free(&image);
	return ok ? 0 : -1;
}

