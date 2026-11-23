@echo off
rem Ejecutable de distribucion: x86-64 generico, enlazado estatico y con las redes
rem dentro. Al arrancar detecta la CPU y elige AVX-512, AVX2 o generico (nnue.c).
rem -muse-unaligned-vector-move: gcc no alinea a 32/64 bytes la pila en Windows (bug
rem 54412); con esto un acceso AVX a la pila nunca puede fallar por alineacion.
echo Compilando Gilipol (distribucion, cualquier CPU x86-64)
gcc -Wall -Wextra -O3 -march=x86-64 -mtune=generic -flto -DNDEBUG -static -Wa,-muse-unaligned-vector-move -o Gilipol_3.20.exe main.c position.c bitboards.c movegen.c search.c nnue.c tbprobe.c
