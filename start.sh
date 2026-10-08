#!/usr/bin/env bash
# ComUI 显示平台 - Linux (Ubuntu) 启动脚本
set -e
cd "$(dirname "$0")"
dotnet build ComUI.slnx -c Debug -v q
exec dotnet "src/ComUI.Host/bin/Debug/net8.0/ComUI.Host.dll"
