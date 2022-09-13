#define J40_CONFIRM_THAT_THIS_IS_EXPERIMENTAL_AND_POTENTIALLY_UNSAFE
#define J40_IMPLEMENTATION
#include "j40.h"

#ifdef __GNUC__ // stb_image_write issues too many warnings
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wsign-conversion"
	#pragma GCC diagnostic ignored "-Wconversion"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h" // copy from https://github.com/nothings/stb/blob/master/stb_image_write.h
#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif

int main(int argc, char **argv) {
	if (argc < 2) return 1;

	j40_image image;
	j40_from_file(&image, argv[1]);
	j40_output_format(&image, J40_RGBA, J40_U8X4);

	if (j40_next_frame(&image)) {
		j40_frame frame = j40_current_frame(&image);
		j40_pixels_u8x4 pixels = j40_frame_pixels_u8x4(&frame, J40_RGBA);
		if (argc > 2) {
			fprintf(stderr, "%dx%d frame read.\n", pixels.width, pixels.height);
			stbi_write_png(argv[2], pixels.width, pixels.height, 4, pixels.data, pixels.stride_bytes);
		} else {
			fprintf(stderr, "%dx%d frame read and discarded.\n", pixels.width, pixels.height);
		}
	}

	if (j40_error(&image)) {
		fprintf(stderr, "Error: %s\n", j40_error_string(&image));
		return 1;
	}

	j40_free(&image);
	return 0;
}