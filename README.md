# J40: Independent, self-contained JPEG XL decoder

**J40** (Jay-forty) is a decoder for ISO/IEC 18181 [JPEG XL] image format.
It intends to be a fully compatible reimplementation to the reference implementation, [libjxl],
and also serves as a verification that the specification allows for
an independent implementation besides from libjxl.

J40 is a [single-file C99 library with zero dependencies][1file],
making it trivial to insert to your project and dependencies.
It is also a public domain software and can be used for absolutely every purpose.

[JPEG XL]: https://en.wikipedia.org/wiki/JPEG_XL
[libjxl]: https://github.com/libjxl/libjxl/
[1file]: https://github.com/nothings/single_file_libs

_**As of version 2270 (2022-09), J40 is a highly experimental software.**
Expect breakage, bug and [incomplete format support](#format-support).
In order to discourage accidental uses, you need to define an additional macro for now._

## Quick Start

The following is a simple but complete converter from JPEG XL to [Portable Arbitrary Map][pam] format,
and covers all the currently available API functions.

[pam]: http://netpbm.sourceforge.net/doc/pam.html

```c
#define J40_IMPLEMENTATION // only a SINGLE file should have this
#include "j40.h" // you also need to define a macro for experimental versions; follow the error.
#include <stdio.h>
#include <stdarg.h> // for va_*

static int oops(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) return oops("Usage: %s input.jxl output.pam\n", argv[0]);

    FILE *out = fopen(argv[2], "wb");
    if (!out) return oops("Error: Cannot open an output file.\n");

    j40_image image;
    j40_from_file(&image, argv[1]); // or: j40_from_memory(&image, buf, bufsize, freefunc);
    j40_output_format(&image, J40_RGBA, J40_U8X4);

    // JPEG XL supports animation, so `j40_next_frame` calls can be called multiple times
    if (j40_next_frame(&image)) {
        j40_frame frame = j40_current_frame(&image);
        j40_pixels_u8x4 pixels = j40_frame_pixels_u8x4(&frame, J40_RGBA);
        fprintf(out,
            "P7\n"
            "WIDTH %d\n"
            "HEIGHT %d\n"
            "DEPTH 4\n"
            "MAXVAL 255\n"
            "TUPLTYPE RGB_ALPHA\n"
            "ENDHDR\n",
            pixels.width, pixels.height);
        for (int y = 0; y < height; ++y) {
            fwrite(j40_row_u8x4(pixels, y), 4, pixels.width, out);
        }
    }

    // J40 stops once the first error is encountered; its error can be checked at the very end
    if (j40_error(&image)) return oops("Error: %s\n", j40_error_string(&image));
    if (ferror(out)) return oops("Error: Cannot fully write to the output file.\n");

    j40_free(&image); // also frees all memory associated to j40_frame etc.
    fclose(out);
    return 0;
}
```

Alternatively, you can use a `dj40` executable to convert a JPEG XL file into a PNG file,
which can be built as follows:

```console
# if your system has make:
$ make

# otherwise, do the equivalent of:
$ cc -O3 dj40.c -o dj40
```

## Format Support

As of version 2270, J40 can decode:

* Any image encoded with [fjxl], or
* Most images encoded with [cjxl][libjxl] containing...
    * No animations or previews
    * No image features (`--dots` and `--patches`), which implies:
        * Efforts (`-e`) up to 6
        * For lossy compression, target distance (`-d`) less than 3.0
    * No progressive encoding (`-p`)
    * No lossless JPEG transcoding in use (can be disabled with `-j`)
    * No floating point samples

[fjxl]: https://github.com/libjxl/libjxl/tree/main/experimental/fast_lossless

There are some known cases where J40 and libjxl can significantly diverge:

* Restoration filters (`--epf`, `--gaborish`) are currently ignored.
* Crop retangles are currently ignored and can reveal a frame larger than the actual image.
* ICC profiles are only decoded to the point that the actual image can be decoded.

