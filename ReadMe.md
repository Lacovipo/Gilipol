# ♟️ Gilipol

<p align="center">
  <img src="Gilipol.png" alt="Logo de Gilipol" width="400">
</p>

> **Gilipol** es un motor de ajedrez desarrollado desde cero y compatible con el protocolo UCI.

Este repositorio contiene el código fuente del motor Gilipol. Gilipol es un poco más fuerte que su antecesor **Anubis**, pero mientras que Anubis era un proyecto agotado y tremendamente difícil de mejorar, Gilipol lo tiene todo por delante.

## ✨ Características Destacadas

* **Lazy SMP (multiprocesador)**: Requiere compilar con el flag `-DUSE_SMP`.
* **Red Neuronal Propia**: Incluye el archivo de red `net5.bin` (con una arquitectura original y peculiar).
* **Código en Evolución**: Algunas secciones del código están deshabilitadas con `#if (0)` porque están implementadas pero en fase de pruebas. Se irán incorporando si demuestran ser productivas.
* **Aritmética Float**: La red utiliza aritmética `float` (en lugar de cuantización), ya que las pruebas iniciales con cuantización no aportaron ganancias de velocidad y complicaron el desarrollo.

## 🚀 Compilación

Puedes compilar Gilipol optimizado para tu arquitectura utilizando `gcc`.

**Para procesadores con soporte BMI2/AVX2:**
```bash
gcc -Wall -Wextra -O3 -march=haswell -flto -DNDEBUG -o Gilipol_AVX2.exe main.c position.c bitboards.c movegen.c search.c nnue.c tbprobe.c
```

**Para procesadores con soporte AVX-512:**
```bash
gcc -Wall -Wextra -O3 -march=native -flto -DNDEBUG -o Gilipol.exe main.c position.c bitboards.c movegen.c search.c nnue.c tbprobe.c
```

## 🧠 Arquitectura y Evaluación (NNUE)

Gilipol implementa una red neuronal con una arquitectura de:  
`786 -> 512 -> 128 -> 64 -> 32 -> 1`

* **Capa de entrada (786):** Contiene la posición de las piezas, el turno, los derechos de enroque y algunas características *ad-hoc* diseñadas para ayudar a la red a comprender la dinámica de ciertas situaciones.
* **Capas ocultas:** Van reduciendo su tamaño con el objetivo de abstraer y generalizar el conocimiento. *Me gusta fantasear con la idea de que la red imita al ajedrecista humano, de modo que en las últimas 32 neuronas logra abstraer conceptos complejos (estructura de peones, seguridad del rey, actividad de las piezas) para construir una evaluación "humana".*

### Origen de los Datos y Generación de Etiquetas

Las posiciones provienen de millones de partidas de alto nivel (principalmente entre motores, pero también humanas). Se almacenan en una base de datos PostgreSQL y se someten a un complejo proceso de etiquetado:

1. **Conversión a Centipeones (cp):** Para cada posición, se recopilan los resultados (**W/D/L**) y se calcula un equivalente en `cp` mediante la fórmula estándar:
   ```sql
   resul := ROUND(400 * log10((b1 + 0.5 * x1) / (n1 + 0.5 * x1)));
   ```
   *(Donde `b` son victorias blancas, `x` son tablas y `n` son victorias negras)*.

2. **Múltiples Evaluaciones:** Cada posición se nutre de varias fuentes para obtener perspectivas distintas:
   * **Estática:** Utilizo **Stockfish 17** como base sólida y de referencia absoluta.
   * **Leela Chess Zero:** Calculada con `go nodes 1`, aporta un elemento "loco", ya que Leela tiende a exagerar ciertas valoraciones posicionales de manera brillante.
   * **Dinámica:** Mi propio motor (Anubis) a profundidad 10 aporta el "espíritu de la casa" y resuelve la táctica superficial.

3. **Compresión y Ponderación Final:**
   Las evaluaciones se escalan contra un factor de compresión basado en el balance y total de material:
   ```sql
   evstst := ROUND(compresion * tanh(evalstst / compresion));
   ```
   Luego se ponderan las tres evaluaciones (con pesos que ajusto experimentalmente):
   ```sql
   evfinal := (evstle * PESO_LEELA + evdyn * PESO_DYN + evstst * (100 - PESO_LEELA - PESO_DYN)) / 100;
   ```

4. **Amortiguación de Resultados (W/D/L):**
   Para que los resultados reales no dominen posiciones extremadamente jugadas, comprimo el total de partidas (`tot`):
   ```sql
   IF tot > 100 THEN
       tot := 200 - 10000 / tot;
   END IF;
   ```
   *(Esto asegura un impacto lineal para los primeros 100 resultados y asintótico para el resto).*

5. **Etiqueta Definitiva:**
   Finalmente, calculo un `peso` según la cantidad de piezas en el tablero y lo fusiono todo:
   ```sql
   peso := public.interpola(public.fen_contar_piezas(fen));
   resultado := (resul * tot + evfinal * peso) / (tot + peso);
   ```

> **Nota:** El resultado de todo esto es una etiqueta bastante disparatada que, muy probablemente, vuelve locos los gradientes en todas direcciones y hace que mi red converja a cualquier sitio raro del espacio de soluciones. *Pero al menos es original.*

## 🏋️ Entrenamiento del Modelo

El script de entrenamiento en Python tiene un enfoque más convencional, basado en un sistema de rotación adaptativa sobre vistas materializadas de PostgreSQL (`V1`...`V5`):

* **Ciclo 1:** Recorre las 5 vistas con un `Learning Rate (LR)` de `0.0003` y `Patience` de 8. El set de validación (unos 20 millones de posiciones aleatorias) se mantiene fijo. En cada época calculo: *Val Loss (Huber loss)*, *Spearman*, *MAE*, *Pair Accuracy* y un *Pair Accuracy limitado a ±100cp* (crítico para saber si la red acierta en posiciones equilibradas).
* **Ciclo 2:** Tras alcanzar un plateau (early stopping), se divide el LR por 2 y se recorren 4 vistas (`V1`...`V4`).
* **Ciclo 3:** Se divide el LR por 2 y se recorren `V5`, `V1`, `V2`.
* **Ciclo 4:** Se divide el LR por 2 y se recorren `V3`, `V4`.
* **Ciclo 5:** Con el LR en su punto más bajo, se entrena únicamente contra `V5`.

De este modo, todas las vistas contribuyen de manera equilibrada al entrenamiento final. 

---

### 👨‍💻 Sobre el proyecto

Sé que he tomado algunas decisiones contrarias al conocimiento general de programación de ajedrez y redes neuronales, y probablemente resulten bastante absurdas a ojos expertos. Soy un completo principiante en cuestiones de IA y necesito pedir explicaciones continuamente a Claude y Gemini. Sin ellos, todavía seguiría intentando entender cómo funciona una única neurona. 
