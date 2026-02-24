@echo off
cd src

rem Nuitka compile Python tools
py -m nuitka --standalone --onefile^
 --product-name="proj" --product-version=1.0.0 --file-description="A simple tool to manage projects easily." --copyright="Copyright © 2025 Omena0. All rights reserved."^
 --output-dir="build"^
 --deployment --python-flag="-OO" --python-flag="-S"^
 --output-filename="proj.exe"^
 proj.py

py -m nuitka --standalone --onefile^
 --product-name="proj" --product-version=1.0.0 --file-description="Count lines of code in a project." --copyright="Copyright © 2025 Omena0. All rights reserved."^
 --output-dir="build"^
 --deployment --python-flag="-OO" --python-flag="-S"^
 --output-filename="countlines.exe"^
 countlines.py

rem Compile C tools
cl /O2 /Fe:..\build\cp.exe cp.c
cl /O2 /Fe:..\build\mv.exe mv.c

rem copy to dist
cd ..\build
if not exist "..\dist" mkdir "..\dist"
move proj.exe "..\dist\proj.exe"
move countlines.exe "..\dist\countlines.exe"
move cp.exe "..\dist\cp.exe"
move mv.exe "..\dist\mv.exe"
