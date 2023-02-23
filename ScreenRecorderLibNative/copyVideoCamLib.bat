@echo off

set "buildType=%1"
if not defined buildType (
set "buildType=Debug"
REM set "buildType=Release"
)

set "dir=%~dp0\..\..\"
if not exist %dir%\DirectShowDemo\ (
    echo %buildType% VideoCamLib will not be copied -- DirectShowDemo project must exist on the same level as yuja-presenter and lecture-capture-cpp-hub to copy the dll, lib and .h
    goto :end
) else (
    echo %buildType% VideoCamLib will be copied
    echo on
)

set "videoCamLibDir=%dir%\DirectShowDemo\ViedeoCamLib"
set "videoCamLibBinDir=%videoCamLibDir%\bin"
set "yscScreenRecorderLibNativeDir=%dir%\yuja-presenter\ScreenRecorderLibNative"

Copy "%videoCamLibBinDir%\%buildType%\VideoCamLib.dll" %yscScreenRecorderLibNativeDir%
Copy "%videoCamLibBinDir%\%buildType%\VideoCamLib.lib" %yscScreenRecorderLibNativeDir%
Copy "%videoCamLibDir%\VideoCamLib.h" %yscScreenRecorderLibNativeDir%

:end

REM pause