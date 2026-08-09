@echo off
REM ---------------------------------------------------------------------------
REM Convenience wrapper around scripts\coredump.py - see docs\coredump.md.
REM
REM Connection settings live here; the MQTT password deliberately does NOT.
REM This repo is public (github.com/igrowing/HA-Irrigator), so the password is
REM resolved at run time from, in order:
REM   1. an IRRIGATOR_MQTT_PASSWORD environment variable, if already set
REM   2. scripts\mqtt_password.txt  - one line, no quotes; .gitignore'd
REM   3. an interactive prompt
REM
REM Passed as an environment variable rather than a --password flag so it never
REM shows up in the process list.
REM
REM Any extra arguments go straight through to coredump.py, e.g.:
REM   scripts\coredump.bat --summary-only
REM   scripts\coredump.bat --no-decode
REM   scripts\coredump.bat --elf builds\1.1.7\firmware.elf
REM ---------------------------------------------------------------------------
setlocal

set "IRRIGATOR_MQTT_HOST=192.168.178.45"
set "IRRIGATOR_MQTT_USER=mqtt_user"
set "IRRIGATOR_TOPIC_PREFIX=irrigatorF4"
REM Must be the exact build that was running when it crashed - a dump decoded
REM against any other ELF yields confidently wrong function names. See the
REM "You need the exact firmware.elf that crashed" section in docs\coredump.md.
set "IRRIGATOR_ELF=%~dp0..\firmware1.1.8.elf"

if not defined IRRIGATOR_MQTT_PASSWORD if exist "%~dp0mqtt_password.txt" set /p IRRIGATOR_MQTT_PASSWORD=<"%~dp0mqtt_password.txt"
if not defined IRRIGATOR_MQTT_PASSWORD set /p "IRRIGATOR_MQTT_PASSWORD=MQTT password: "

python "%~dp0coredump.py" %*
set "EXITCODE=%ERRORLEVEL%"

if not "%EXITCODE%"=="0" (
    echo.
    echo [coredump.bat] coredump.py exited with code %EXITCODE% - see the output above.
)
endlocal & exit /b %EXITCODE%
