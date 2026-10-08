@echo off
rem ComUI display platform launcher (Windows)
rem Keep this file ASCII-only: cmd.exe parses batch files in the ANSI codepage,
rem and UTF-8 Chinese comments break line parsing on zh-CN systems.
cd /d "%~dp0"
dotnet build ComUI.slnx -c Debug -v q
if errorlevel 1 (
  echo Build FAILED.
  pause
  exit /b 1
)
start "" "src\ComUI.Host\bin\Debug\net8.0\ComUI.Host.exe"
