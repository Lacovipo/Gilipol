# Gilipol

<p align="center">
  <img src="Gilipol.png" alt="Logo de Gilipol" width="400">
</p>

**Gilipol es un programa de ajedrez compatible con el protocolo UCI.**

Este repositorio contiene el código fuente del motor Gilipol.  

* Lazy SMP (multiprocesador). Requiere compilar con -DUSE_SMP.
* Archivo de red net4.bin (arquitectura original y rara).
* Algunas secciones del código están deshabilitadas con #if (0) porque están implementadas pero sin probar. Las iré incorporando si las pruebas indican que son productivas.
* Gilipol es un poco más fuerte que su antecesor Anubis, pero Anubis era un proyecto agotado, tremendamente difícil de mejorar, y Gilipol lo tiene todo por mejorar.
* Compilación BMI2/AVX2: gcc -Wall -Wextra -O3 -march=haswell -flto -DNDEBUG -o Gilipol_AVX2.exe main.c position.c bitboards.c movegen.c search.c nnue.c tbprobe.c
* Compilación AVX512: gcc -Wall -Wextra -O3 -march=native -flto -DNDEBUG -o Gilipol.exe main.c position.c bitboards.c movegen.c search.c nnue.c tbprobe.c
