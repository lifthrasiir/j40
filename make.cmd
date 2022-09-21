@echo off
setlocal EnableDelayedExpansion EnableExtensions

rem batch file by default treats `=` as a separator, we replace all occurrences of
rem `=` with `#E` (and `#` with `#H` for round-trip) and reparse replaced parameters.
rem (adapted from https://stackoverflow.com/a/44661126)
rem --------------------------------------------------------------------------------
set "_rawopts=|%*|"
set "_rawopts=!_rawopts:#=#H!"
set "_opts="
set "_idx=1"
:eqloop
set "_part="
for /f "tokens=%_idx% eol= delims==" %%i in ("!_rawopts!") do set "_part=%%i"
if not "!_part!" == "" (
	set "_opts=!_opts!!_part!#E"
	set /a _idx+=1
	goto eqloop
)
set "_opts=!_opts:~1,-3!"
call :reparse !_opts!
goto :eof

:reparse

rem default variables
rem --------------------------------------------------------------------------------
set "_var_cc=!cc!"
if "!_var_cc!" == "" set "_var_cc=msvc"

rem filter VAR=VALUE arguments and set _var_VAR; remaining options go to _nopts
rem --------------------------------------------------------------------------------
set "_nopts="
:argloop
set "_opt=%1"
if not "!_opt!" == "" (
	set "_opt=|%~1|"
	set "_opt=!_opt:#E==!"
	set "_opt=!_opt:#H=#!"
	set "_value="
	for /f "tokens=1,* eol= delims==" %%a in ("!_opt!") do set "_key=%%a" & set "_value=%%b"
	if "!_value!" == "" (
		set "_opt=%1"
		set "_opt=!_opt:#E==!"
		set "_opt=!_opt:#H=#!"
		set "_nopts=!_nopts!!_opt! "
	) else (
		set "_key=!_key:~1!"
		set "_value=!_value:~0,-1!"
		set "_var_!_key!=!_value!"
	)
	shift
	goto :argloop
)

if exist "%~p0extra\build\windows-!_var_cc!.ninja" (
	%~p0extra\build\ninja -f "%~p0extra\build\windows-!_var_cc!.ninja" !_nopts!
	endlocal
) else (
	echo Error: CC option or environment variable should be either `msvc` (default^), `gcc` or `clang`.
	exit /b 1
)
goto :eof
