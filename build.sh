#!/usr/bin/env zsh

cd src

# Nuitka compile Python tools
CC=gcc python3 -m nuitka --standalone --onefile \
 --product-name="proj" --product-version=1.0.0 --file-description="A simple tool to manage projects easily." --copyright="Copyright © 2026 Omena0. All rights reserved." \
 --output-dir="../build" \
 --deployment --python-flag="-OO" --python-flag="-S" \
 --output-filename="proj" \
 proj.py

CC=gcc python3 -m nuitka --standalone --onefile \
 --product-name="proj" --product-version=1.0.0 --file-description="Count lines of code in a project." --copyright="Copyright © 2026 Omena0. All rights reserved." \
 --output-dir="../build" \
 --deployment --python-flag="-OO" --python-flag="-S" \
 --output-filename="countlines" \
 countlines.py

# Compile C tools
gcc -O2 -march=native -mtune=native -o ../build/cp cp.c -lpthread
gcc -O2 -march=native -mtune=native -o ../build/mv mv.c -lpthread

# copy to dist
cd ../build
mkdir -p ../dist

mv proj "../dist/proj"
mv countlines "../dist/countlines"

mv cp "../dist/cp"
mv mv "../dist/mv"


cd ..
