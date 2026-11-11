@echo off
cd /d "%~dp0"
set "EDEN_DKB_FRAME_TRACE=1"
set "EDEN_NX2_GPU_PROFILE=1"
set "EDEN_NX2_STUTTER_TRACE=1"
set "EDEN_NX2_STUTTER_DETAIL_MS=25"
start "" /wait "%~dp0lichii.exe"
pause
