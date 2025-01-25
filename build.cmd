
rem Nuitka compile
py -m nuitka --standalone --onefile^
 --product-name="proj" --product-version=1.0.0 --file-description="A simple tool to manage projects easily." --copyright="Copyright © 2025 Omena0. All rights reserved."^
 --output-dir="build"^
 --deployment --python-flag="-OO" --python-flag="-S"^
 --output-filename="proj.exe"^
 proj.py

rem copy to dist
cd build
move proj.exe "../dist/proj.exe"
