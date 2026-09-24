@echo off
REM ===========================================================================
REM  bolt.cmd -- the BoltOS SDK, from cmd.exe.
REM
REM  All the work is in bolt.ps1. This exists so that `bolt build` is what you
REM  type instead of `powershell -ExecutionPolicy Bypass -File ...\bolt.ps1
REM  build`, and so the SDK still runs on a machine whose execution policy was
REM  set by somebody who does not know BoltOS exists.
REM
REM    sdk\bolt new hello         start a project
REM    sdk\bolt build             compile and link it
REM    sdk\bolt ship              serve it to a running BoltOS
REM
REM  Put sdk\ on PATH and the `sdk\` goes away.
REM ===========================================================================
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell.exe"

REM -h and /? are what people try before they try `help`.
set "ARGS=%*"
if "%~1"=="-h"     set "ARGS=help"
if "%~1"=="--help" set "ARGS=help"
if "%~1"=="/?"     set "ARGS=help"

"%PS%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0bolt.ps1" %ARGS%
exit /b %ERRORLEVEL%
