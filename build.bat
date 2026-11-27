@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build
set CXX=g++
set CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc
set LDFLAGS=-static -static-libgcc -static-libstdc++ -lwinpthread -lws2_32
set SRCS=src\util.cpp src\platform.cpp src\skiplist.cpp src\bloom.cpp src\wal.cpp src\sstable.cpp src\db.cpp
set OBJS=
for %%f in (%SRCS%) do (
    %CXX% %CXXFLAGS% -c %%f -o build\%%~nf.o
    if errorlevel 1 goto fail
    set OBJS=!OBJS! build\%%~nf.o
)
ar rcs build\libminidb.a !OBJS!
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% tests\test_persist.cpp !OBJS! -o build\test_persist.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% tests\test_bench.cpp !OBJS! -o build\test_bench.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% tests\test_concurrent.cpp !OBJS! -o build\test_concurrent.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% tests\test_levels.cpp !OBJS! -o build\test_levels.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% src\server.cpp !OBJS! -o build\minidb_server.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% src\cli.cpp !OBJS! -o build\minidb_cli.exe %LDFLAGS%
if errorlevel 1 goto fail
%CXX% %CXXFLAGS% tests\test_network.cpp !OBJS! -o build\test_network.exe %LDFLAGS%
if errorlevel 1 goto fail
echo BUILD OK
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
