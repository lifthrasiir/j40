# Extra stuffs for J40

* `stb_image_write.h` (1.16): A single-file public domain image writer library from [stb](https://github.com/nothings/stb/).

* `j40-fuzz.c`: Fuzzer entry point.

## Subdirectory `build`

This directory complements `make.cmd` at the repository root which emulates Make in Windows.

* `ninja.exe` (1.11.1): The [Ninja](https://ninja-build.org/) build system.
* `vswhere.exe` (3.0.3): The [Visual Studio Locator](https://github.com/microsoft/vswhere).
* `in-vsenv.cmd`: Runs arguments as a command line after running `VsDevCmd.bat`.
* `windows_common.ninja`: Common build script shared by all other `windows-*.ninja`.
* `windows-$CC.ninja`: The build script used with given `$CC`, which can be either an environment variable or given as `CC=gcc` like the traditional Make. The default value for `$CC` is `msvc`.

