@echo off
setlocal

for /f "usebackq tokens=*" %%i in (`%~p0\vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
	set InstallDir=%%i
)

if exist "%InstallDir%\Common7\Tools\vsdevcmd.bat" (
	call "%InstallDir%\Common7\Tools\vsdevcmd.bat"
	%*
	endlocal
) else (
	echo Error: Couldn't locate VsDevCmd.bat.
	endlocal
	exit /b 1
)
