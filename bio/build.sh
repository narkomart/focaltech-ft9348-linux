#!/bin/bash
# Сборка ftbio. RTLD_LAZY -> шим libgusb не нужен (мы зовём только focal_*-алгоритм, не gusb).
set -e
cd "$(dirname "$0")"
gcc -O2 -o ftbio ftbio.c -ldl $(pkg-config --cflags --libs libusb-1.0) -lm
echo "built: $(pwd)/ftbio"
