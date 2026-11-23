/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

 #include "common.h"
#include "tbprobe.h"
// Espera de 1 ms (bestmove en infinite/ponder, I08). En la compilacion monohilo
// no se incluye windows.h; basta con declarar Sleep.
#if !defined(USE_SMP)
  #ifdef _WIN32
    __declspec(dllimport) void __stdcall Sleep(unsigned long ms);
  #else
    #include <unistd.h>
  #endif
#endif

#ifdef USE_SMP
    #ifdef _WIN32
    #include <windows.h>
    #else
    #include <pthread.h>
    #include <unistd.h>
    #endif
#endif

// Nodos de la ultima busqueda completada (main + helpers). Lo consume 'bench'.
long long last_search_nodes = 0;

TTCluster* tt_table = NULL;
uint64_t tt_cluster_count = 0;
uint8_t tt_generation = 0;

// --- Continuation History ---
// Indexada por [pieza_previa][casilla_destino_previa][casilla_origen_actual][casilla_destino_actual]
// Tamaño: 12 * 64 * 64 * 64 * 4 bytes = 12 MB (aceptable)
// Nota: pieza_previa = pieza que se movió en prev_move (0-11: P,N,B,R,Q,K blancas y negras)
#define CONT_HIST_SIZE 12
typedef ATOMIC_INT ContHistEntry[64][64]; // [from][to] del movimiento actual
typedef ContHistEntry ContHistTable[64]; // [to] del movimiento previo
static ContHistTable continuation_history[CONT_HIST_SIZE]; // [pieza_previa]

// --- Estructura de datos por hilo ---
typedef struct
{
    int thread_id;
    ATOMIC_LLONG nodes;
    int sel_depth;

    // Heurísticas de Ordenación (Locales por hilo)
    Move killer_moves[MAX_PLY][2];
    int history_moves[2][64][64];
    Move counter_moves[2][64][64];
    int capture_history[12][64][6]; // [Pieza][Casilla][Victima]

    // Correction history: sesgo medio (busqueda - eval estatica) por estructura
    // de peones. int16_t y no int para que la tabla ocupe 64 KB en vez de 128 y
    // quepa mejor en cache; con CORR_LIMIT = 8192 sobra rango.
    int16_t correction_history[2][CORR_SIZE];

    // Control de extensiones
    int search_root_depth;

    // Referencia a la posición raíz (para helpers)
    Position root_pos;
} SearchData;

// Información del movimiento previo para continuation history
typedef struct
{
    int piece;  // Pieza que se movió (0-11, o -1 si no hay)
    int to;     // Casilla destino
} PrevMoveInfo;

// --- Variables estáticas para control de tiempo interno ---
// Estas variables son compartidas (escritas por main thread antes de buscar, leídas por todos)
static long long search_start_time = 0;     // lo fija preparar_busqueda() al recibir 'go'
static SHARED_LLONG search_time_soft = -1;  // compartidas: 'ponderhit' las reescribe desde el hilo UCI
static SHARED_LLONG search_time_hard = -1;
static ATOMIC_LLONG search_max_nodes = 0;   // 'go nodes N' (0 = sin límite)
static SearchLimits current_limits;
static SHARED_INT search_flag_infinite = 0;
static SHARED_INT search_flag_ponder = 0;
static int previous_search_score = 0;

// --- Variables para heurísticas de tiempo (sólo Main Thread) ---
static double prev_growth_factors[3] = {0.0, 0.0, 0.0}; // Últimos 3 growth factors
static int prev_growth_count = 0;
static Move best_move_history[3] = {MOVE_NONE, MOVE_NONE, MOVE_NONE}; // Últimos 3 best moves
static int score_history[3] = {0, 0, 0}; // Últimos 3 scores
static int stability_count = 0;                          // Contador de estabilidad continuo
static long long root_best_move_nodes = 0;               // Nodos del mejor movimiento en la raíz (Node TM)
static long long root_total_nodes = 0;                   // Nodos totales en la raíz (Node TM)

// --- Syzygy Helper ---
int probe_syzygy(const Position* pos, int* score, int ply)
{
    // Verificar límite de piezas (configuración y límite físico de las tablas cargadas)
    int pieces = count_bits(pos->all);
    if (pieces > opt_syzygy_probe_limit || (unsigned)pieces > TB_LARGEST)
        return 0;

    int castling = pos->castling_rights;
    int ep = (pos->en_passant_sq == SQ_NONE) ? 0 : pos->en_passant_sq;
    
    // Fathom probe WDL (Win/Draw/Loss)
    unsigned wdl = tb_probe_wdl(
        pos->occupied[WHITE], pos->occupied[BLACK],
        pos->pieces[WHITE][KING] | pos->pieces[BLACK][KING],
        pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN],
        pos->pieces[WHITE][ROOK] | pos->pieces[BLACK][ROOK],
        pos->pieces[WHITE][BISHOP] | pos->pieces[BLACK][BISHOP],
        pos->pieces[WHITE][KNIGHT] | pos->pieces[BLACK][KNIGHT],
        pos->pieces[WHITE][PAWN] | pos->pieces[BLACK][PAWN],
        0,
        castling, ep, pos->side_to_move == WHITE
    );

    if (wdl == TB_RESULT_FAILED)
        return 0;

    // Convertir WDL a score del motor
    // TB_WIN: Ganamos. Devolvemos un valor alto pero menor que mate inmediato.
    // TB_LOSS: Perdemos.
    // TB_DRAW: Tablas.
    
    switch (wdl)
    {
        case TB_WIN:
            *score = TB_MAX - ply; // Ganamos (casi mate)
            return 1;
        case TB_LOSS:
            *score = -TB_MAX + ply; // Perdemos
            return 1;
        case TB_DRAW:
            *score = 0; // Tablas
            return 1;
        case TB_CURSED_WIN: // Ganamos pero la regla de 50 mov lo impide (o tablas si rule50=true)
            *score = opt_syzygy_50move_rule ? 0 : (TB_MAX - ply);
            return 1;
        case TB_BLESSED_LOSS: // Perdemos pero la regla de 50 mov nos salva
            *score = opt_syzygy_50move_rule ? 0 : (-TB_MAX + ply);
            return 1;
        default:
            return 0;
    }
}

void init_tt()
{
    // 32 MB por defecto
    resize_tt(32);
}

void resize_tt(int mb)
{
    // Mínimo: 1 Mb
    // Máximo: 16 Gb
    if (mb < 1)
        mb = 1;
    if (mb > 16384)
        mb = 16384;

    // Alineada a 64 bytes: un TTCluster mide exactamente una linea de cache, y con
    // malloc (16 bytes de alineacion en Windows) la mitad de los clusters cruzarian
    // dos lineas, duplicando los fallos de cache en cada sondeo.
    //
    // Antes se liberaba la tabla vieja y, si la reserva nueva fallaba, se jugaba sin TT
    // (B17). Ahora: 1) se reserva la nueva sin soltar la vieja; 2) si no cabe, se suelta
    // la vieja y se reintenta; 3) si sigue sin caber, se prueba con la mitad.
    const int mb_pedidos = mb;
    uint64_t clusters = ((uint64_t)mb << 20) / sizeof(TTCluster);
    TTCluster* nueva = (TTCluster*)_mm_malloc(clusters * sizeof(TTCluster), 64);
    if (!nueva && tt_table)
    {
        _mm_free(tt_table);
        tt_table = NULL;
        tt_cluster_count = 0;
        nueva = (TTCluster*)_mm_malloc(clusters * sizeof(TTCluster), 64);
    }
    while (!nueva && mb > 1)
    {
        mb /= 2;
        clusters = ((uint64_t)mb << 20) / sizeof(TTCluster);
        nueva = (TTCluster*)_mm_malloc(clusters * sizeof(TTCluster), 64);
    }
    if (!nueva)
    {
        if (tt_table)
        {
            printf("info string Error: No se pudo asignar la tabla hash (%d MB); se mantiene la anterior\n", mb_pedidos);
            return;
        }
        printf("info string Error: No se pudo asignar ninguna tabla hash: se juega SIN TT\n");
        tt_cluster_count = 0;
        return;
    }
    if (mb != mb_pedidos)
        printf("info string Aviso: no caben %d MB de hash; se usan %d MB\n", mb_pedidos, mb);

    if (tt_table)
        _mm_free(tt_table);
    tt_table = nueva;
    tt_cluster_count = clusters;

    clear_tt();
    printf("info string Hash inicializada: %d MB (%llu clusters)\n", mb, (unsigned long long)tt_cluster_count);
}

void clear_tt()
{
    if (tt_table && tt_cluster_count > 0)
    {
        memset(tt_table, 0, tt_cluster_count * sizeof(TTCluster));
        tt_generation = 0;
    }
}

// Incrementar generación al inicio de cada búsqueda (para aging)
void tt_new_search()
{
    tt_generation++;
    // Wraparound cada 256 búsquedas (no hay problema, el aging sigue funcionando)
}

// Indice de cluster sin division.
//
// 'hash %% tt_cluster_count' es una division entera de 64 bits: 20-40 ciclos en
// CADA sondeo. El truco de Lemire (multiplicar y quedarse con la parte alta)
// reparte igual de bien y cuesta una multiplicacion.
static inline uint64_t tt_indice(uint64_t hash)
{
    return (uint64_t)(((__uint128_t)hash * (__uint128_t)tt_cluster_count) >> 64);
}

// --- Hash sin bloqueo (lockless) ---
//
// Sólo hash_key es atómica; el resto de la entrada no lo es. En SMP un lector
// puede ver una entrada a medio escribir (score de una posición con la profundidad
// de otra) y cortar con un valor falso, o devolver un best_move ilegal.
//
// Solución estándar: guardar la clave XOR un resumen del resto de los campos. Si
// la entrada se lee desgarrada, el resumen no cuadra y se descarta. En monohilo
// el valor da la vuelta exacta, así que no cambia nada.
static inline uint32_t tt_resumen(Move best_move, int16_t score, int16_t eval,
                                  uint8_t depth, uint8_t flag)
{
    return (uint32_t)best_move
         ^ ((uint32_t)(uint16_t)score << 3)
         ^ ((uint32_t)(uint16_t)eval  << 7)
         ^ ((uint32_t)depth << 19)
         ^ ((uint32_t)flag  << 27);
}

static inline uint32_t tt_resumen_de(const TTEntry* e)
{
    return tt_resumen(e->best_move, e->score, e->eval, e->depth, e->flag);
}

// Índice de cluster y prefetch. Se llama nada más generar el hash del hijo para
// que la línea de cache esté en camino cuando el nodo hijo sondee la TT.
void tt_prefetch(uint64_t hash)
{
    if (tt_table && tt_cluster_count)
        __builtin_prefetch(&tt_table[tt_indice(hash)]);
}

// Ajustar puntuación de mate o de tablebase para guardar en TT.
// El score que llega es relativo al nodo actual ("mate en N desde aquí"); en la
// TT hay que dejarlo relativo a la raíz, porque la misma entrada se leerá desde
// plies distintos.
// OJO: el umbral es TB_BOUND, no MATE_BOUND. Los scores de Syzygy valen
// ±(TB_MAX - ply) y también dependen del ply; con MATE_BOUND se guardaban en
// crudo y se corrompían al releerse desde otra profundidad.
static inline int16_t score_to_tt(int score, int ply)
{
    if (score >= TB_BOUND)
        return (int16_t)(score + ply);
    if (score <= -TB_BOUND)
        return (int16_t)(score - ply);
    return (int16_t)score;
}

// Recuperar puntuación de mate o de tablebase desde TT (inversa de score_to_tt)
static inline int score_from_tt(int16_t score, int ply)
{
    // Convertir a int para comparación correcta con TB_BOUND
    int s = (int)score;
    if (s >= TB_BOUND)
        return s - ply;
    if (s <= -TB_BOUND)
        return s + ply;
    return s;
}

static void score_a_texto(int score, char* out, size_t tam)
{
    if (abs(score) > MATE_BOUND)
    {
        int moves = (MATE_SCORE - abs(score) + 1) / 2;
        snprintf(out, tam, "mate %d", score > 0 ? moves : -moves);
    }
    else
    {
        snprintf(out, tam, "cp %d", score);
    }
}

// PV terminada en MOVE_NONE, separada por espacios
static void pv_a_texto(const Move* pv, char* out, size_t tam)
{
    size_t n = 0;
    out[0] = 0;
    for (int j = 0; j < MAX_PLY && pv[j] != MOVE_NONE; j++)
    {
        char m[8];
        move_to_str(pv[j], m);
        const int w = snprintf(out + n, tam - n, "%s%s", j ? " " : "", m);
        if (w < 0 || (size_t)w >= tam - n)
            break;
        n += (size_t)w;
    }
}

/*
 *
 * read_tt (retorna: 1 si hace cutoff, 0 si no; siempre intenta devolver best_move si encuentra la posición)
 * 
 */
// tt_eval: eval estática pura guardada en TT (salida NNUE de una búsqueda anterior).
//          Se inicializa a EVAL_NONE; se rellena si hay hit, independientemente de si hay cutoff.
//          Permite evitar llamadas a nnue_evaluate() en visitas repetidas a la misma posición.
int read_tt(uint64_t hash, int depth, int ply, int alpha, int beta, int* score, Move* best_move, int* tt_depth, int* tt_flag, int* tt_eval)
{
    if (!tt_table || tt_cluster_count == 0)
        return 0;

    uint64_t cluster_idx = tt_indice(hash);
    TTCluster* cluster = &tt_table[cluster_idx];
    // Firma con los 32 bits BAJOS: tt_indice() usa los ALTOS (Lemire), y si la firma
    // tambien los usara, los bits del indice no aportarian identidad (I04).
    uint32_t hash_key = (uint32_t)hash;

    // Inicialización
    *best_move = MOVE_NONE;
    if (tt_depth)
        *tt_depth = 0;
    if (tt_flag)
        *tt_flag = 0;
    if (tt_eval)
        *tt_eval = EVAL_NONE;  // Centinela: sin eval válida hasta que encontremos la entrada

    // Buscar en las 4 ranuras
    TTEntry* found_entry = NULL;
    for (int i = 0; i < TT_CLUSTER_SIZE; i++)
    {
        TTEntry* entry = &cluster->entries[i];

        // Lectura de la clave (atómica con acquire en SMP, directa en single-thread).
        // La clave guardada lleva XOR con el resumen del resto de la entrada: si se
        // leyó desgarrada, no cuadra y la descartamos.
        // depth == 0: ranura nunca escrita (toda entrada real tiene depth >= 1). Sin la
        // guarda, una clave con los 32 bits bajos a 0 casaba con una ranura vacia (B26).
        if (entry->depth != 0 && (ATOMIC_LOAD_ACQ(entry->hash_key) ^ tt_resumen_de(entry)) == hash_key)
        {
            // Priorizar la entrada más profunda si hay múltiples
            if (!found_entry || entry->depth > found_entry->depth)
                found_entry = entry;
        }
    }

    if (!found_entry)
        return 0; // No encontrada

    // Siempre devolver el movimiento para ordenación (incluso si no hacemos cutoff)
    *best_move = found_entry->best_move;
    if (tt_depth)
        *tt_depth = found_entry->depth;
    if (tt_flag)
        *tt_flag = found_entry->flag;

    // Devolver la eval estática guardada (para evitar llamada a NNUE).
    // OJO: cuando el nodo estaba en jaque no hay eval estática y se guardó EVAL_NONE.
    // El llamante lo distingue comparando contra EVAL_NONE.
    if (tt_eval && found_entry->depth > 0)
        *tt_eval = (int)found_entry->eval;

    // Solo hacer cutoff si la profundidad es suficiente
    int tt_val = score_from_tt(found_entry->score, ply);
    if (score)
        *score = tt_val;

    // Movimiento útil, pero no suficiente profundidad
    if (found_entry->depth < depth)
        return 0;

    // Verificar si podemos hacer cutoff según el tipo de bound.
    //
    // OJO: *score se queda con tt_val en todos los casos. Antes se sobreescribia con
    // alpha o beta (fail-hard), lo que tiraba informacion en un motor que por lo demas
    // es fail-soft, y en la raiz llegaba a meter los limites de la ventana de
    // aspiracion dentro de static_eval.
    if (found_entry->flag == TT_EXACT)
        return 1;
    else if (found_entry->flag == TT_ALPHA && tt_val <= alpha)
        return 1;
    else if (found_entry->flag == TT_BETA && tt_val >= beta)
        return 1;

    // No cutoff pero movimiento disponible
    return 0;
}

/*
 *
 * write_tt
 * 
 */
void write_tt(uint64_t hash, int depth, int ply, int score, int flag, Move best_move, int static_eval)
{
    if (!tt_table || tt_cluster_count == 0)
        return;

    uint64_t cluster_idx = tt_indice(hash);
    TTCluster* cluster = &tt_table[cluster_idx];
    // Firma con los 32 bits BAJOS: tt_indice() usa los ALTOS (Lemire), y si la firma
    // tambien los usara, los bits del indice no aportarian identidad (I04).
    uint32_t hash_key = (uint32_t)hash;

    TTEntry* replace_entry = NULL;
    int replace_idx = -1;
    int worst_score = -1000000;

    //
    // Buscar si ya existe esta posición o una ranura vacía.
    //
    // OJO: hay que barrer TODAS las ranuras buscando la misma posición ANTES de
    // quedarse con una vacía. Antes se cortaba en la primera vacía, así que si la
    // ranura 0 estaba libre y la 1 tenía esta misma posición, se guardaba dos veces.
    //
    TTEntry* ranura_vacia = NULL;

    for (int i = 0; i < TT_CLUSTER_SIZE; i++)
    {
        TTEntry* entry = &cluster->entries[i];

        // Ranura vacía (nunca usada): la recordamos, pero seguimos buscando
        if (ATOMIC_LOAD(entry->hash_key) == 0 && entry->depth == 0)
        {
            if (!ranura_vacia)
                ranura_vacia = entry;
            continue;
        }

        // Misma posición - siempre reemplazar si es más profunda o misma generación
        if ((ATOMIC_LOAD(entry->hash_key) ^ tt_resumen_de(entry)) == hash_key)
        {
            // Always replace si:
            // - Es la misma generación (búsqueda actual)
            // - Profundidad mayor o igual
            // - Tipo EXACT (más valioso)
            if (entry->generation == tt_generation
                || depth >= entry->depth
                || flag == TT_EXACT)
            {
                replace_entry = entry;
                replace_idx = i;
                break;
            }
            else
            {
                // Misma posición pero menos profunda y generación vieja - no reemplazar
                return;
            }
        }
    }

    //
    // Si no está ya guardada, usar la ranura vacía si la hubo
    //
    if (replace_entry == NULL && ranura_vacia != NULL)
    {
        replace_entry = ranura_vacia;
        replace_idx = 0;
    }

    //
    // Si no encontramos ranura libre ni misma posición, elegir víctima
    //
    if (replace_entry == NULL)
    {
        // Esquema de puntuación para elegir víctima:
        // - Penalizar entradas viejas (generación diferente)
        // - Penalizar profundidades bajas
        // - Bonificar EXACT bounds (son más valiosos)
        //
        // Fórmula: score = depth * 8 - age * 256 + (is_exact ? 128 : 0)
        // Reemplazamos la entrada con menor score

        for (int i = 0; i < TT_CLUSTER_SIZE; i++)
        {
            TTEntry* entry = &cluster->entries[i];

            // Calcular edad (wraparound safe)
            int age = (tt_generation - entry->generation) & 0xFF;

            // Calcular score de reemplazo
            int entry_score = (int)entry->depth * 8 - age * 256;
            if (entry->flag == TT_EXACT)
                entry_score += 128;

            // ¿Es la peor candidata hasta ahora?
            if (entry_score < worst_score || replace_idx == -1)
            {
                worst_score = entry_score;
                replace_entry = entry;
                replace_idx = i;
            }
        }
    }

    // 
    // Escribir en la ranura elegida
    // 
    if (replace_entry != NULL)
    {
        // Escribir datos primero
        replace_entry->best_move = best_move;
        replace_entry->score = score_to_tt(score, ply);
        replace_entry->eval = (int16_t)static_eval;
        replace_entry->depth = (uint8_t)depth;
        replace_entry->generation = tt_generation;
        replace_entry->flag = (uint8_t)flag;
        // Escribir la clave al final, con XOR del resumen (ver tt_resumen).
        // En SMP el release publica los datos anteriores de forma segura.
        ATOMIC_STORE_REL(replace_entry->hash_key,
                         hash_key ^ tt_resumen(best_move, score_to_tt(score, ply),
                                               (int16_t)static_eval, (uint8_t)depth, (uint8_t)flag));
    }
}

// --- Helpers de Búsqueda ---

// Valores de piezas para MVV-LVA [P, N, B, R, Q, K]
// Valores estándar: P=100, N=320, B=330, R=500, Q=900, K=2000
static const int piece_value[6] = { 100, 320, 330, 500, 900, 2000 };

// Obtener el índice de pieza (0-11) para continuation history
// Retorna -1 si no se puede determinar
static inline int get_piece_index(const Position* pos, int sq, int side)
{
    for (int pt = PAWN; pt <= KING; pt++)
    {
        if (GET_BIT(pos->pieces[side][pt], sq))
            return side * 6 + pt; // 0-5 blancas, 6-11 negras
    }
    return -1;
}

// Obtener info del movimiento previo para continuation history
static inline PrevMoveInfo get_prev_move_info(const Position* pos, Move prev_move)
{
    PrevMoveInfo info = { -1, 0 };
    if (prev_move == MOVE_NONE)
        return info;

    int prev_to = GET_MOVE_TARGET(prev_move);
    int prev_side = pos->side_to_move ^ 1; // El que movió antes es el oponente

    // Buscar qué pieza está en la casilla destino del movimiento anterior
    info.piece = get_piece_index(pos, prev_to, prev_side);
    info.to = prev_to;
    return info;
}

// Limpiar continuation history (llamar al inicio de cada partida)
static void clear_continuation_history(void)
{
    memset(continuation_history, 0, sizeof(continuation_history));
}

// Datos por hilo persistentes entre busquedas: la posicion 0 es el hilo principal y
// las siguientes los helpers. Antes se pedian con malloc en cada 'go' y se liberaban
// al acabar, de modo que history, capturas, contrajugadas, killers y corrhist nacian
// vacias en cada jugada. Ahora viven toda la partida y las borra clear_search_heuristics.
static SearchData* sd_pool   = NULL;
static int         sd_pool_n = 0;

// Garantiza al menos n entradas; las nuevas nacen a cero. NULL si no hay memoria.
static SearchData* obtener_sd_pool(int n)
{
    if (n > sd_pool_n)
    {
        SearchData* nuevo = (SearchData*)realloc(sd_pool, (size_t)n * sizeof(SearchData));
        if (!nuevo)
            return NULL;
        memset(nuevo + sd_pool_n, 0, (size_t)(n - sd_pool_n) * sizeof(SearchData));
        sd_pool   = nuevo;
        sd_pool_n = n;
    }
    return sd_pool;
}

// Al empezar cada busqueda: borra o conserva cada tabla segun CONSERVA_*.
static void preparar_heuristicas(SearchData* sd)
{
#if (CONSERVA_KILLERS)
    memmove(&sd->killer_moves[0], &sd->killer_moves[2], (MAX_PLY - 2) * sizeof(sd->killer_moves[0]));
    memset(&sd->killer_moves[MAX_PLY - 2], 0, 2 * sizeof(sd->killer_moves[0]));
#else
    memset(sd->killer_moves, 0, sizeof(sd->killer_moves));
#endif
#if (!CONSERVA_HISTORIA)
    memset(sd->history_moves, 0, sizeof(sd->history_moves));
#endif
#if (!CONSERVA_CONTRAJUGADAS)
    memset(sd->counter_moves, 0, sizeof(sd->counter_moves));
#endif
#if (!CONSERVA_CAPTURAS)
    memset(sd->capture_history, 0, sizeof(sd->capture_history));
#endif
#if (!CONSERVA_CORRHIST)
    memset(sd->correction_history, 0, sizeof(sd->correction_history));
#endif
}

// Función pública para limpiar todas las heurísticas de búsqueda
void clear_search_heuristics(void)
{
    clear_continuation_history();
    if (sd_pool)
        memset(sd_pool, 0, (size_t)sd_pool_n * sizeof(SearchData));
    previous_search_score = 0;
}

// Helper para obtener el tipo de pieza en una casilla (para capture history)
static int get_piece_type_at(const Position* pos, int sq, int side)
{
    if (GET_BIT(pos->pieces[side][PAWN], sq))
        return PAWN;
    if (GET_BIT(pos->pieces[side][KNIGHT], sq))
        return KNIGHT;
    if (GET_BIT(pos->pieces[side][BISHOP], sq))
        return BISHOP;
    if (GET_BIT(pos->pieces[side][ROOK], sq))
        return ROOK;
    if (GET_BIT(pos->pieces[side][QUEEN], sq))
        return QUEEN;
    if (GET_BIT(pos->pieces[side][KING], sq))
        return KING;
    return -1;
}

// Actualizar valor de historia con gravity (fórmula SF)
// El valor converge naturalmente: cuanto más alto, más se frena el crecimiento
static inline void update_history_value(int* value, int bonus)
{
    *value += bonus - (*value * abs(bonus)) / 16384;
}

// Versión atómica para continuation history (compartida entre hilos)
static inline void update_history_value_atomic(ATOMIC_INT* value, int bonus)
{
    int old_val = ATOMIC_LOAD(*value);
    int new_val = old_val + bonus - (old_val * abs(bonus)) / 16384;
    ATOMIC_STORE(*value, new_val);
}

/*
 *
 * Asigna valores de ordenación a las jugadas
 *
 */
int score_move(Move move, Move tt_move, int ply, Move prev_move, const Position* pos, SearchData* sd)
{
    // 1. El movimiento de la Tabla de Transposición es el más importante
    if (move == tt_move)
        return 70000;

    // 2. Capturas y Promociones (MVV-LVA)
    if (GET_MOVE_CAPTURE(move) || GET_MOVE_PROMOTED(move))
    {
        int score = 40000; 

        int source = GET_MOVE_SOURCE(move);
        int target = GET_MOVE_TARGET(move);
        int attacker = PAWN;
        
        int side = pos->side_to_move;
        
        // Identificar atacante
        if (GET_BIT(pos->pieces[side][PAWN], source))
            attacker = PAWN;
        else if (GET_BIT(pos->pieces[side][KNIGHT], source))
            attacker = KNIGHT;
        else if (GET_BIT(pos->pieces[side][BISHOP], source))
            attacker = BISHOP;
        else if (GET_BIT(pos->pieces[side][ROOK], source))
            attacker = ROOK;
        else if (GET_BIT(pos->pieces[side][QUEEN], source))
            attacker = QUEEN;
        else if (GET_BIT(pos->pieces[side][KING], source))
            attacker = KING;

        // Si es captura, sumar valor de la víctima
        if (GET_MOVE_CAPTURE(move))
        {
            int victim = PAWN;
            int enemy = side ^ 1;
            
            if (GET_MOVE_ENPASSANT(move))
                victim = PAWN;
            else
            {
                if (GET_BIT(pos->pieces[enemy][PAWN], target))
                    victim = PAWN;
                else if (GET_BIT(pos->pieces[enemy][KNIGHT], target))
                    victim = KNIGHT;
                else if (GET_BIT(pos->pieces[enemy][BISHOP], target))
                    victim = BISHOP;
                else if (GET_BIT(pos->pieces[enemy][ROOK], target))
                    victim = ROOK;
                else if (GET_BIT(pos->pieces[enemy][QUEEN], target))
                    victim = QUEEN;
                else if (GET_BIT(pos->pieces[enemy][KING], target))
                    victim = KING;
            }
            score += piece_value[victim] * 10;

            // Capture History: Ajuste dinámico basado en el éxito histórico de esta captura
            int piece_idx = side * 6 + attacker;
            score += sd->capture_history[piece_idx][target][victim]; // Sumar historial (puede ser negativo)
        }

        // Restar valor del atacante (MVV-LVA)
        score -= piece_value[attacker];

        // Bonus grande por promoción (incluso si no es captura)
        if (GET_MOVE_PROMOTED(move))
            score += piece_value[GET_MOVE_PROMOTED(move)] * 10;

        // SEE (Static Exchange Evaluation): Penalizar capturas que pierden material
        if (!see(pos, move, 0))
            score -= 25000; // Bajar prioridad por debajo de Killers (30000)

        return score;
    }

    // 3. Killer Moves (30,000 y 29,000)
    if (move == sd->killer_moves[ply][0])
        return 30000;
    if (move == sd->killer_moves[ply][1])
        return 29000;

    // 4. Counter Move Heuristic (28,000)
    if (prev_move != MOVE_NONE && move == sd->counter_moves[pos->side_to_move][GET_MOVE_SOURCE(prev_move)][GET_MOVE_TARGET(prev_move)])
        return 28000;

    // 5. History + Continuation History combinados
    int source = GET_MOVE_SOURCE(move);
    int target = GET_MOVE_TARGET(move);
    int score = sd->history_moves[pos->side_to_move][source][target];

    // Añadir bonus de continuation history si hay movimiento previo válido
    if (prev_move != MOVE_NONE)
    {
        PrevMoveInfo prev_info = get_prev_move_info(pos, prev_move);
        if (prev_info.piece >= 0)
        {
            score += ATOMIC_LOAD(continuation_history[prev_info.piece][prev_info.to][source][target]) / 2;
        }
    }

    return score;
}

/*
 *
 * Ordenar movimientos (Selection Sort simple para listas pequeñas)
 *
 */
// Puntua sin ordenar. El bucle de jugadas escoge con pick_move(): hace en el
// momento el mismo intercambio que haria la ordenacion completa, asi que el orden
// es identico y no se paga por ordenar jugadas que un corte nunca llega a ver (I47).
static inline void score_moves(const MoveList* list, int* scores, Move tt_move, int ply, Move prev_move, const Position* pos, SearchData* sd)
{
    for (int i = 0; i < list->count; i++)
        scores[i] = score_move(list->moves[i], tt_move, ply, prev_move, pos, sd);
}

static inline void pick_move(MoveList* list, int* scores, int i)
{
    int best_idx = i;
    for (int j = i + 1; j < list->count; j++)
    {
        if (scores[j] > scores[best_idx])
            best_idx = j;
    }
    if (best_idx != i)
    {
        Move temp_move = list->moves[i];
        list->moves[i] = list->moves[best_idx];
        list->moves[best_idx] = temp_move;

        int temp_score = scores[i];
        scores[i] = scores[best_idx];
        scores[best_idx] = temp_score;
    }
}

void sort_moves(MoveList* list, Move tt_move, int ply, Move prev_move, const Position* pos, SearchData* sd)
{
    int scores[256];
    for (int i = 0; i < list->count; i++)
        scores[i] = score_move(list->moves[i], tt_move, ply, prev_move, pos, sd);
    
    for (int i = 0; i < list->count - 1; i++)
    {
        int best_idx = i;
        for (int j = i + 1; j < list->count; j++)
        {
            if (scores[j] > scores[best_idx])
                best_idx = j;
        }
        // Swap move y score
        if (best_idx != i)
        {
            Move temp_move = list->moves[i];
            list->moves[i] = list->moves[best_idx];
            list->moves[best_idx] = temp_move;
            
            int temp_score = scores[i];
            scores[i] = scores[best_idx];
            scores[best_idx] = temp_score;
        }
    }
}

// --- Correction history ---

// Sesgo aprendido para esta estructura de peones, en centipeones.
static inline int correccion_eval(const Position* pos, const SearchData* sd)
{
#if (USA_CORRHIST)
    const int cv = sd->correction_history[pos->side_to_move][pos->pawn_hash & (CORR_SIZE - 1)];
    return cv / CORR_GRAIN;
#else
    (void)pos; (void)sd;
    return 0;
#endif
}

// Media movil ponderada por profundidad: cuanto mas honda la busqueda que produjo
// la diferencia, mas peso se le da.
static inline void actualizar_correccion(const Position* pos, SearchData* sd, int diff, int depth)
{
#if (USA_CORRHIST)
    int16_t* entrada = &sd->correction_history[pos->side_to_move][pos->pawn_hash & (CORR_SIZE - 1)];
    const int peso = min(depth + 1, CORR_PESO);
    const int v = ((int)(*entrada) * (256 - peso) + diff * CORR_GRAIN * peso) / 256;
    *entrada = (int16_t)clamp_int(v, -CORR_LIMIT, CORR_LIMIT);
#else
    (void)pos; (void)sd; (void)diff; (void)depth;
#endif
}

/* *
 * quiescence
 * 
 */
// nodo_nuevo = false cuando negamax llama a qsearch para el MISMO nodo (profundidad
// agotada, IIR, razoring): negamax ya lo ha contado y ya ha mirado el limite de nodos y
// el reloj. Antes se contaba dos veces: nodos y nps salian inflados, y 'go nodes N'
// visitaba menos posiciones de las N pedidas (B27).
int quiescence(Position* pos, int alpha, int beta, int ply, SearchData* sd, bool nodo_nuevo)
{
    //
    // Salida inmediata si se ha detenido la búsqueda
    //
    if (ATOMIC_LOAD(stop_search))
        return 0;

    if (nodo_nuevo)
    {
        //
        // Profundidad selectiva y nodos
        //
        if (ply > sd->sel_depth)
            sd->sel_depth = ply;
        ATOMIC_FETCH_ADD(sd->nodes, 1);

        // Límite de nodos ('go nodes N'). Se comprueba en cada nodo, no cada 4096 como
        // el tiempo: el sentido de este límite es ser exacto y reproducible entre
        // máquinas. Con el límite a 0 es una comparación contra una constante caliente.
        const long long tope_nodos = ATOMIC_LOAD(search_max_nodes);
        if (tope_nodos > 0 && ply > 0 && ATOMIC_LOAD(sd->nodes) >= tope_nodos)
        {
            ATOMIC_STORE(stop_search, 1);
            return 0;
        }

        //
        // Comprobación de tiempo cada 4096 nodos (pero NO interrumpir depth 1)
        //
        if (sd->thread_id == 0 && (ATOMIC_LOAD(sd->nodes) & 4095) == 0)
        {
            long long hard_limit = ATOMIC_LOAD(search_time_hard);
            // Comprobar si se acabó el tiempo asignado
            if (hard_limit > 0 && ply > 0 && (get_time_ms() - search_start_time >= hard_limit))
            {
                ATOMIC_STORE(stop_search, 1);
                return 0;
            }
        }
    }

    //
	// Poda de distancia a mate
	//
    if (ply > 0)
    {
	    alpha = max(-MATE_SCORE  + ply, alpha);
	    beta = min(MATE_SCORE  - ply - 1, beta);
	    if (alpha >= beta)
		    return(alpha);
    }

	//
	// Consultar tabla hash
	//
    int tt_score;
    Move tt_move = MOVE_NONE;
    int tt_depth = 0;
    int tt_flag = 0;
    int tt_eval = EVAL_NONE;   // eval estática guardada en TT (EVAL_NONE = sin dato válido)
    if (read_tt(pos->hash, 0, ply, alpha, beta, &tt_score, &tt_move, &tt_depth, &tt_flag, &tt_eval)
        && pos->half_move_clock < 90)   // ver 1.7c: el hash no incluye el contador de 50
        return tt_score;
    const bool bHayScoreTT = (tt_depth > 0); // Usamos depth > 0 para detectar entrada válida, incluso sin movimiento (común en TT_ALPHA)

    //
    // Determinar si estamos en jaque
    //
    int in_check = is_square_attacked(pos, get_lsb_index(pos->pieces[pos->side_to_move][KING]), pos->side_to_move ^ 1);

    //
    // Límite de profundidad (ply) - seguridad contra desbordamiento de arrays
    //
    if (ply >= MAX_PLY)
        return (in_check ? 0 : nnue_evaluate(pos));

    //
    // Eval estática (sólo si no estamos en jaque)
    //
    // Usamos tt_eval (guardada en TT) si está disponible para evitar la llamada a nnue_evaluate().
    // tt_eval es la salida pura de NNUE guardada en una búsqueda anterior → igual de válida.
    //
    int stand_pat = -INFINITO;
    const bool bRecuperarEvalTT = (bHayScoreTT && tt_eval != EVAL_NONE);
    if (!in_check)
    {
        // Usar tt_eval si está disponible; aplicar ajuste de 50 jugadas sobre raw_eval.
        int qs_raw = bRecuperarEvalTT ? tt_eval : nnue_evaluate_tc(pos, QS_USA_TC);
        stand_pat = qs_raw * (200 - pos->half_move_clock) / 200;
        stand_pat += correccion_eval(pos, sd);
        stand_pat = clamp_int(stand_pat, -EVAL_MAX, EVAL_MAX);   // la correccion no la mete en zona TB

        // Si tenemos un lower bound de la TT (TT_BETA) que es mayor que la evaluación estática,
        // lo usamos para mejorar la precisión de stand_pat.
        // Con hmc >= 90 no: el hash no incluye el contador de 50 (ver 1.7c), y este ajuste
        // daba el mismo corte que la guarda de arriba acababa de negar (B6).
        if (bHayScoreTT && (tt_flag == TT_BETA) && tt_score > stand_pat && pos->half_move_clock < 90)
            stand_pat = tt_score;

        if (stand_pat >= beta)
            return beta;
        if (alpha < stand_pat)
            alpha = stand_pat;
    }

    //
    // Clasificador tactico (WillImprove / WillWorsen / IsStable)
    //
    bool bWillImprove = false;
    bool bWillWorsen  = false;
    bool bIsStable    = false;
    if (USA_TC && QS_USA_TC && tc_initialized && !in_check && !bRecuperarEvalTT)
    {
        TacticalClass tc = nnue_get_tc();
        int thr = TC_THRESHOLD;
        // El clasificador usa perspectiva de blancas (igual que h4[]).
        // Si juegan negras, mejora para blancas = empeora para el jugador.
        bool tc_mejora  = ((int)(tc.p_mejora  * 100) >= thr);
        bool tc_empeora = ((int)(tc.p_empeora * 100) >= thr);
        bIsStable       = ((int)(tc.p_estable * 100) >= thr);
        if (pos->side_to_move == WHITE)
        {
            bWillImprove = tc_mejora;
            bWillWorsen  = tc_empeora;
        }
        else
        {
            bWillImprove = tc_empeora;
            bWillWorsen  = tc_mejora;
        }
    }
    // Poda muy bestia por clasificador táctico
    #if(0)
    if (bWillWorsen)
        return(alpha);  // No me puedo comer un mate en contra porque si estuviese en jaque las tres variables del TC serían false
    #endif

    //
    // Poda delta mejorada
    //
    if (!in_check)
    {
        int enemy = pos->side_to_move ^ 1;
        int max_val = piece_value[PAWN];
        if (pos->pieces[enemy][QUEEN])
            max_val = piece_value[QUEEN];
        else if (pos->pieces[enemy][ROOK])
            max_val = piece_value[ROOK];
        else if (pos->pieces[enemy][BISHOP] | pos->pieces[enemy][KNIGHT])
            max_val = piece_value[BISHOP];
        int margin = max_val + QS_PODA_DELTA + bWillImprove * QS_IMPROVE + bIsStable * QS_STABLE + bWillWorsen * QS_WORSEN;
        if (stand_pat + margin < alpha)
            return(alpha);
    }

    //
    // Bucle principal: sólo capturas
    //
    MoveList moves;
    generate_captures(pos, &moves);
    int qs_scores[256];
    score_moves(&moves, qs_scores, tt_move, 0, MOVE_NONE, pos, sd);
    
    int legal_moves = 0;
    Position child;
    for (int i = 0; i < moves.count; i++)
    {
        pick_move(&moves, qs_scores, i);
        Move move = moves.moves[i];

        //
        // Podas
        //
        if (!in_check)
        {
            // LMP
            #if (0)
            if (legal_moves >= QS_LMP)
                break;
            #endif

            // Poda SEE: SEE < QS_SEE
            // Poda futility: SEE < alpha - stand_pat - QS_FUT_MARGIN
            int threshold = max(QS_SEE, alpha - stand_pat - QS_FUT_MARGIN);
            if (!see(pos, move, threshold))
                continue;
        }

        copy_position(&child, pos);
        if (!make_move(&child, move))
            continue;

        tt_prefetch(child.hash);
        
        legal_moves++;
        child.prev = pos;   // la cadena prev no debe saltarse los plies de qsearch (I31)
        int score = -quiescence(&child, -beta, -alpha, ply + 1, sd, true);
        if (ATOMIC_LOAD(stop_search))
            return 0;

        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }

    //
    // Si estamos en jaque y no hay movimientos legales, es mate (porque hemos generado evasiones)
    //
    if (in_check && legal_moves == 0)
        return -MATE_SCORE + ply;

    return alpha;
}

// Hay al menos una jugada legal? (solo se usa con la regla de 50 y en jaque)
static __attribute__((noinline, cold)) bool hay_jugada_legal(const Position* pos)
{
    MoveList lista;
    generate_moves(pos, &lista, GEN_ALL);
    Position hijo;
    for (int i = 0; i < lista.count; i++)
    {
        copy_position(&hijo, pos);
        if (make_move(&hijo, lista.moves[i]))
            return true;
    }
    return false;
}

// Detectar material insuficiente para dar mate
static int is_insufficient_material(const Position* pos)
{
    // Si hay peones, torres o damas, hay material suficiente
    if (pos->pieces[WHITE][PAWN] | pos->pieces[BLACK][PAWN] | pos->pieces[WHITE][ROOK] | pos->pieces[BLACK][ROOK] | pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN])
        return 0;

    // Solo posiciones MUERTAS (sin ninguna secuencia legal que acabe en mate):
    // K-K, K+menor contra K, y K+A contra K+A con alfiles del mismo color.
    // Menor contra menor en general NO es muerta: hay mates legales (I03).
    const int w_minors = count_bits(pos->pieces[WHITE][KNIGHT] | pos->pieces[WHITE][BISHOP]);
    const int b_minors = count_bits(pos->pieces[BLACK][KNIGHT] | pos->pieces[BLACK][BISHOP]);

    if (w_minors + b_minors <= 1)
        return 1;

    if (w_minors == 1 && b_minors == 1
        && !(pos->pieces[WHITE][KNIGHT] | pos->pieces[BLACK][KNIGHT]))
    {
        const uint64_t CASILLAS_CLARAS = 0x55AA55AA55AA55AAULL;
        const bool w_claro = (pos->pieces[WHITE][BISHOP] & CASILLAS_CLARAS) != 0;
        const bool b_claro = (pos->pieces[BLACK][BISHOP] & CASILLAS_CLARAS) != 0;
        if (w_claro == b_claro)
            return 1;
    }

    return 0;
}

static bool MargenesNoMate(int alpha, int beta)
{
    return((alpha > -MATE_BOUND) && (beta < MATE_BOUND));
}

static int razor_margin[6];

void init_razor_margin()
{
    razor_margin[0] = 0;
    razor_margin[1] = BUS_RAZOR_MARGIN1;
    razor_margin[2] = BUS_RAZOR_MARGIN2;
    razor_margin[3] = BUS_RAZOR_MARGIN3;
    razor_margin[4] = BUS_RAZOR_MARGIN4;
    razor_margin[5] = BUS_RAZOR_MARGIN5;
}

/*
 *
 * negamax
 *
 */
int negamax(Position* pos, int alpha, int beta, int depth, int ply, Move prev_move, Move excluded_move, Move* pv, bool bIsCutNode, SearchData* sd)
{
    const bool bEsPV = (beta > alpha + 1);
    const int original_alpha = alpha;

    // La PV se vacia LO PRIMERO, antes de cualquier salida anticipada.
    //
    // Antes se hacia mas abajo, despues de ocho returns: parada por tiempo o por
    // nodos, limite de ply, poda por distancia a mate, tablas por repeticion o por
    // regla de 50, y los tres de Syzygy. Al salir por cualquiera de ellos el buffer
    // del hijo se quedaba con la PV del hermano anterior, y el padre la copiaba tal
    // cual: de ahi las 22.338 quejas de "Illegal PV move" en un match de 5000
    // partidas. No afectaba a la jugada elegida (pv[0] siempre es legal), pero si
    // a la linea mostrada y a la jugada de ponder.
    pv[0] = MOVE_NONE;

    //
    // Salida inmediata si se ha detenido la búsqueda
    //
    if (ATOMIC_LOAD(stop_search))
        return 0;

    //
    // Límite de profundidad (ply)
    //
    if (ply >= MAX_PLY)
    {
        const int in_check = is_square_attacked(pos, get_lsb_index(pos->pieces[pos->side_to_move][KING]), pos->side_to_move ^ 1);
        return in_check ? 0 : nnue_evaluate(pos);
    }

    //
    // Profundidad selectiva y nodos
    //
    if (ply > sd->sel_depth)
        sd->sel_depth = ply;
    ATOMIC_FETCH_ADD(sd->nodes, 1);

    // Límite de nodos ('go nodes N'). Se comprueba en cada nodo, no cada 4096 como
    // el tiempo: el sentido de este límite es ser exacto y reproducible entre
    // máquinas. Con el límite a 0 es una comparación contra una constante caliente.
    {
        const long long tope_nodos = ATOMIC_LOAD(search_max_nodes);
        if (tope_nodos > 0 && ply > 0 && ATOMIC_LOAD(sd->nodes) >= tope_nodos)
        {
            ATOMIC_STORE(stop_search, 1);
            return 0;
        }
    }

    //
    // Comprobación de tiempo cada 4095 nodos
    //
    if (sd->thread_id == 0 && (ATOMIC_LOAD(sd->nodes) & 4095) == 0)
    {
        long long hard_limit = ATOMIC_LOAD(search_time_hard);
        // Comprobar si se acabó el tiempo asignado (chequeo intra-búsqueda)
        if (hard_limit > 0 && ply > 0 && (get_time_ms() - search_start_time >= hard_limit))
        {
            ATOMIC_STORE(stop_search, 1);
            return 0;
        }
    }

    //
	// Poda de distancia a mate
	//
    if (ply > 0)
    {
	    alpha = max(-MATE_SCORE  + ply, alpha);
	    beta = min(MATE_SCORE  - ply - 1, beta);
	    if (alpha >= beta)
		    return(alpha);
    }

    //
    // Comprobar tablas: 50 movimientos, material insuficiente o repetición
    //
    if (ply > 0 && (is_insufficient_material(pos) || is_repetition(pos)))
        return 0;

    // Regla de 50: el mate tiene prioridad (FIDE 9.3). Solo hace falta mirarlo si el
    // bando al turno esta en jaque; si no, son tablas sin mas (I02).
    if (ply > 0 && pos->half_move_clock >= 100)
    {
        if (!is_square_attacked(pos, get_lsb_index(pos->pieces[pos->side_to_move][KING]), pos->side_to_move ^ 1)
            || hay_jugada_legal(pos))
            return 0;
        return -MATE_SCORE + ply;
    }

    //
    // Syzygy Probing (WDL)
    //
    if (ply > 0
        && !ATOMIC_LOAD(stop_search)
        && excluded_move == MOVE_NONE
        && depth >= opt_syzygy_probe_depth
        && pos->half_move_clock == 0        // la WDL solo es exacta tras una jugada que pone el
        && pos->castling_rights == 0        // contador a 0, y sin enroques (como Stockfish; B9)
        && count_bits(pos->all) <= opt_syzygy_probe_limit)
    {
        int tb_score;
        if (probe_syzygy(pos, &tb_score, ply))
        {
            if (tb_score >= beta)
                return beta;
            if (tb_score <= alpha)
                return alpha;
            return tb_score;
        }
    }


    //
    // Salida por profundidad agotada
    //
    if (depth <= 0)
        return quiescence(pos, alpha, beta, ply, sd, false);

	//
	// Consultar tabla hash
	//
    int tt_score;
    Move tt_move = MOVE_NONE;
    int tt_depth = 0;
    int tt_flag = 0;
    int tt_eval = EVAL_NONE;   // eval estática guardada en TT (EVAL_NONE = sin dato válido)
    if (excluded_move == MOVE_NONE)
    {
        const int tt_permite_corte = read_tt(pos->hash, depth, ply, alpha, beta,
                                             &tt_score, &tt_move, &tt_depth, &tt_flag, &tt_eval);

        // Consultamos siempre (necesitamos tt_move y tt_eval), pero NO cortamos:
        //
        //  - en la raíz: necesitamos recorrer las jugadas para tener PV y bestmove.
        //  - con el contador de 50 alto.
        if (tt_permite_corte && ply > 0 && (TT_CORTE_EN_PV || !bEsPV) && pos->half_move_clock < 90)
            return tt_score;
    }
    bool bHayMovTT = (tt_move != MOVE_NONE); // No es constante porque puede cambiar tras IID
    const bool bHayScoreTT = (tt_depth > 0); // Usamos depth > 0 para detectar entrada válida, incluso sin movimiento (común en TT_ALPHA)

    //
    // Podas hash
    //
    #if (0)
    if (bHayScoreTT && ply > 0 && !bEsPV && abs(tt_score) < MATE_BOUND && excluded_move == MOVE_NONE)
    {
        //
        // Poda hash beta
        // 
        if (bIsCutNode
            && tt_depth >= depth - PODA_HASH_BETA_PROF_DIF
            && (tt_flag == TT_BETA || tt_flag == TT_EXACT)
            && abs(beta) < MATE_BOUND            
            && tt_score >= beta
            && tt_score > beta
                        + PODA_HASH_BETA_BASE
                        + PODA_HASH_BETA_MULT_DIF * max(depth - tt_depth, 1)
                        + PODA_HASH_BETA_MULT_PROF * depth)
        {
            return(beta);
        }

        //
        // Poda hash alfa
        //
		if (!bIsCutNode
            && tt_depth >= depth - PODA_HASH_ALFA_PROF_DIF
			&& (tt_flag == TT_EXACT || tt_flag == TT_ALPHA)
			&& abs(alpha) < MATE_BOUND
			&& tt_score <= alpha
			&& tt_score < alpha
                        - PODA_HASH_ALFA_BASE
                        - PODA_HASH_ALFA_MULT_DIF * max(depth - tt_depth, 1)
                        - PODA_HASH_ALFA_MULT_PROF * depth)
		{
			return(alpha);
		}
    }
    #endif
    
    //
    // Determinar si estamos en jaque (necesario para NM y para detección de mate)
    //
    const int king_sq = get_lsb_index(pos->pieces[pos->side_to_move][KING]);
    const int in_check = is_square_attacked(pos, king_sq, pos->side_to_move ^ 1);

    //
    // Eval estática (sólo si no estamos en jaque)
    //
    // raw_eval   : salida pura de nnue_evaluate() o tt_eval guardada.
    //              Se escribe en TT al final del nodo. NUNCA es tt_score.
    // static_eval: raw_eval, opcionalmente mejorada con tt_score para decisiones de poda.
    //              Puede ser tt_score; no se escribe en TT.
    //
    int static_eval = -INFINITO;
    int eval_base   = -INFINITO;   // eval con la regla de 50, sin correccion ni tt_score: contra ella aprende la corrhist
    int raw_eval    = EVAL_NONE;   // en jaque no hay eval estática: esto es lo que va a la TT
    const bool bRecuperarEvalTT = (bHayScoreTT && tt_eval != EVAL_NONE);
    if (!in_check)
    {
        // Usar tt_eval si está disponible para evitar la llamada a nnue_evaluate()
        raw_eval = bRecuperarEvalTT ? tt_eval : nnue_evaluate(pos);

        // Aplicar ajuste por regla de 50 jugadas
        static_eval = raw_eval * (200 - pos->half_move_clock) / 200;
        eval_base   = static_eval;

        // Correccion por estructura de peones. Va sobre static_eval, NO sobre
        // raw_eval: raw_eval es lo que se guarda en la TT y debe quedar en crudo.
        static_eval += correccion_eval(pos, sd);
        static_eval = clamp_int(static_eval, -EVAL_MAX, EVAL_MAX);   // la correccion no la mete en zona TB

        // Mejorar static_eval con el tt_score si es un bound fiable en la dirección correcta
        // (no con hmc >= 90: el hash no incluye el contador de 50, ver 1.7c; B6)
        if (bHayScoreTT && pos->half_move_clock < 90)
        {
            if (tt_flag == TT_EXACT
                || (tt_flag == TT_ALPHA && tt_score < static_eval)
                || (tt_flag == TT_BETA  && tt_score > static_eval))
            {
                static_eval = tt_score;
            }
        }
    }
    
    //
    // Mejorando
    //
    pos->eval = static_eval;
    bool bMejorando = false;
    if (!in_check && pos->prev && pos->prev->prev)
    {
        Position* prev_pos = pos->prev->prev;
        
        const bool prev_in_check = (prev_pos->eval == -INFINITO);

        if (prev_in_check)
        {
            // Si hace 2 estábamos en jaque, miramos hace 4
            if (prev_pos->prev && prev_pos->prev->prev)
            {
                Position* prev4_pos = prev_pos->prev->prev;
                const bool prev4_in_check = (prev4_pos->eval == -INFINITO);

                // Si hace 4 también estábamos en jaque, o si la eval actual es mejor que hace 4
                if (prev4_in_check || static_eval > prev4_pos->eval)
                    bMejorando = true;
            }
        }
        else
        {
            // Si hace 2 no estábamos en jaque, comparamos eval
            if (static_eval > prev_pos->eval)
                bMejorando = true;
        }
    }

    //
    // Clasificador tactico (WillImprove / WillWorsen / IsStable)
    //
    bool bWillImprove = false;
    bool bWillWorsen  = false;
    bool bIsStable    = false;
    if (USA_TC && tc_initialized && !in_check && !bRecuperarEvalTT)
    {
        TacticalClass tc = nnue_get_tc();
        int thr = TC_THRESHOLD;
        // El clasificador usa perspectiva de blancas (igual que h4[]).
        // Si juegan negras, mejora para blancas = empeora para el jugador.
        bool tc_mejora  = ((int)(tc.p_mejora  * 100) >= thr);
        bool tc_empeora = ((int)(tc.p_empeora * 100) >= thr);
        bIsStable       = ((int)(tc.p_estable * 100) >= thr);
        if (pos->side_to_move == WHITE)
        {
            bWillImprove = tc_mejora;
            bWillWorsen  = tc_empeora;
        }
        else
        {
            bWillImprove = tc_empeora;
            bWillWorsen  = tc_mejora;
        }

        // La direccion del clasificador esta invertida cerca de las hojas: solo
        // se hace caso a partir de TC_DIR_MIN_DEPTH. Ver el comentario en common.h.
        if (depth < TC_DIR_MIN_DEPTH)
        {
            bWillImprove = false;
            bWillWorsen  = false;
        }
    }

	//
	// IID
	//
    if (!bHayMovTT
        && depth >= BUS_IID_DEPTH_MIN
        && static_eval > alpha + bWillImprove * BUS_IID_IMPROVE + bWillWorsen * BUS_IID_WORSEN
        && excluded_move == MOVE_NONE)
    {
        Move iid_pv[MAX_PLY];
        negamax(pos, alpha, beta, depth / 2, ply, prev_move, MOVE_NONE, iid_pv, bIsCutNode, sd);
        if (iid_pv[0] != MOVE_NONE)
        {
            tt_move = iid_pv[0];
            bHayMovTT = true;
        }
    }

    //
    // IIR
    //
    if (!bHayMovTT
        && depth >= BUS_IIR_DEPTH
        && excluded_move == MOVE_NONE)
    {
        if (bEsPV)
            depth -= (BUS_IIR_PVRED + bIsStable * BUS_IIR_PV_STABLE);
        else if (bIsCutNode)
            depth -= (BUS_IIR_CUTRED + bWillImprove * BUS_IIR_CUT_IMPROVE);
        else
            depth -= (BUS_IIR_ALLRED + bWillWorsen * BUS_IIR_ALL_WORSEN);

        if (depth <= 0)
            return quiescence(pos, alpha, beta, ply, sd, false);
    }

    const bool bPodable = !bEsPV && !in_check && ply > 0 && excluded_move == MOVE_NONE && MargenesNoMate(alpha, beta);

    //
    // Full razoring (por llamarlo de alguna forma)
    // Idea vista en SF y algún otro (Halogen, Reckless...)
    // OJO: Probar otra forma de aplicar TC: no hacer razoring si bWillImprove
    //
    if (BUS_FULLRAZ_USE
        && bPodable
        && static_eval < alpha - BUS_FULLRAZ_BASE - BUS_FULLRAZ_MULT * depth * depth + bWillImprove * BUS_FULLRAZ_IMPROVE + bWillWorsen * BUS_FULLRAZ_WORSEN)
    {
        return quiescence(pos, alpha, beta, ply, sd, false);
    }

	//
	// Razoring contra alfa (si estamos tan lejos de alfa que únicamente podemos recuperar con capturas, intentamos llegar a alfa con quiesce)
    // OJO: Probar otra forma de aplicar TC: no hacer razoring si bWillImprove
	//
    if (bPodable
        && depth <= BUS_RAZOR_DEPTH
        && static_eval < alpha - razor_margin[depth < 6 ? depth : 5])   // Idea: + bWillImprove * cte- + bWillWorsen * cte+
    {
        const int val = quiescence(pos, alpha, beta, ply, sd, false);
        if (val <= alpha)
            return(val);
    }

    const bool bTieneAlgunaPieza = pos->occupied[pos->side_to_move] ^ pos->pieces[pos->side_to_move][PAWN] ^ pos->pieces[pos->side_to_move][KING];

    //
    // Podas contra beta
    //
    if (bPodable
        && static_eval >= beta
        && bTieneAlgunaPieza)
    {
        //
        // Poda JC (~ reverse futility)
        //
        if (depth <= BUS_PODAJC_DEPTH
            && static_eval < TB_BOUND       // puede ser un mate de la TT: (mate + beta) / 2 no es nada (B8)
            && (BUS_PODAJC_IFWORSEN || !bWillWorsen)
            && static_eval > beta + BUS_PODAJC_MARGIN * (depth - bMejorando) // + bWillImprove * BUS_PODAJC_WILLIMPROVE + bWillWorsen * BUS_PODAJC_WILLWORSEN
            && static_eval > BUS_PODAJC_CERO)
        {
            return((static_eval + beta) / 2); // OJO: devolver eval - margen (como Alexandria 9)
        }

        //
        // Null move
        // OJO: bWillImprove puede tener el efecto contrario del esperado, porque en realidad dice "tenemos un movimiento que mejora", y en null move no estamos haciendo ese movimiento...
        //
        if (prev_move != MOVE_NONE
            && (BUS_NM_IFWORSEN || !bWillWorsen)
            && static_eval >= beta + bWillImprove * BUS_NM_WILLIMPROVE + bIsStable * BUS_NM_ISSTABLE + bWillWorsen * BUS_NM_WILLWORSEN
            && (BUS_NM_CUTYALL || bIsCutNode)) // El parámetro a 1 acepta los nodos ALL
        {
            Position child;
            copy_position(&child, pos);
            child.prev = (Position*)pos;
            make_null_move(&child);
            
            int R = BUS_NM_R + depth / 3 + BUS_NM_IMP * (bMejorando || bWillImprove);
            if (static_eval - beta >= BUS_NM_DIV)
                R += min((static_eval - beta) / BUS_NM_DIV, 3);
            Move null_pv[MAX_PLY]; // Buffer para PV (no se usa aquí)

            // Búsqueda con ventana nula
            int score = -negamax(&child, -beta, -beta + 1, depth - R, ply + 1, MOVE_NONE, MOVE_NONE, null_pv, false, sd);

            if (score >= beta)
            {
                if (score >= TB_BOUND)      // ni un mate ni una victoria de TB salen de un pase (B7)
                    score = beta;
                // Verificación
                if (depth >= BUS_NM_VERIF)
                {
                    const int verif_depth = max((depth - R) / 2, 1);
                    int scorev = negamax(pos, beta - 1, beta, verif_depth, ply, MOVE_NONE, MOVE_NONE, null_pv, true, sd);
                    if (scorev >= beta)
                        return(score); // El resultado de la primera búsqueda (null move)
                }
                else
                    return score;
            }
        }
    }

    //
    // Probcut: si una búsqueda superficial de capturas supera beta + margen,
    // es muy probable que la búsqueda completa también lo haga → podar
    //
    if (bPodable
        && depth >= PROBCUT_DEPTH)
    {
        const int pbeta = beta + PROBCUT_MARGIN + bWillImprove * PROBCUT_WILLIMPROVE + bIsStable * PROBCUT_ISSTABLE + bWillWorsen * PROBCUT_WILLWORSEN;

        // Solo buscar si la eval estática sugiere que hay posibilidades
        if (static_eval >= pbeta)
        {
            MoveList pc_moves;
            generate_moves(pos, &pc_moves, GEN_CAPTURES);
            sort_moves(&pc_moves, MOVE_NONE, 0, MOVE_NONE, pos, sd);

            for (int i = 0; i < pc_moves.count; i++)
            {
                // Solo considerar capturas con SEE >= 0 (no perder material)
                if (!see(pos, pc_moves.moves[i], 0))
                    continue;

                Position pc_child;
                copy_position(&pc_child, pos);
                pc_child.prev = (Position*)pos;

                if (!make_move(&pc_child, pc_moves.moves[i]))
                    continue;

                Move pc_pv[MAX_PLY];

                // Primero quiescence para verificar rápido
                int pc_score = -quiescence(&pc_child, -pbeta, -pbeta + 1, ply + 1, sd, true);

                // Si quiescence ya supera pbeta, verificar con búsqueda más profunda
                if (pc_score >= pbeta && depth - PROBCUT_REDUCTION > 0)
                    pc_score = -negamax(&pc_child, -pbeta, -pbeta + 1, depth - PROBCUT_REDUCTION, ply + 1, pc_moves.moves[i], MOVE_NONE, pc_pv, !bIsCutNode, sd);

                if (ATOMIC_LOAD(stop_search))
                    return 0;

                if (pc_score >= pbeta)
                    return pc_score;
            }
        }
    }

    //
    // Extensión singular
    //
    int singular_extension = 0;
    if (!ATOMIC_LOAD(stop_search)
        && depth >= SE_MIN_DEPTH
        && ply > 0
        && bHayScoreTT
        && bHayMovTT
        && excluded_move == MOVE_NONE
        && (tt_flag == TT_BETA || tt_flag == TT_EXACT)
        && MargenesNoMate(alpha, beta)
        && tt_depth >= depth - SE_TT_DEPTH_MARGIN)
    {
        // Margen dinámico
        int singular_margin = SE_MARGIN_BASE + SE_MARGIN_SCALE * depth / 3;
        int singular_beta = tt_score - singular_margin;

        // Profundidad reducida
        int singular_depth = depth / 2;

        Move singular_pv[MAX_PLY];

        // Búsqueda reducida excluyendo tt_move
        int se_score = negamax(pos, singular_beta - 1, singular_beta, singular_depth, ply, prev_move, tt_move, singular_pv, false, sd);

        if (ATOMIC_LOAD(stop_search))
            return 0;

        if (se_score < singular_beta)
        {
            singular_extension = 1;

            if (se_score < singular_beta - SE_DOUBLE_MARGIN)
            {
                singular_extension = 2;

                if (se_score < singular_beta - SE_TRIPLE_MARGIN)
                    singular_extension = 3;
            }
        }
        // Multicut
        else if (singular_beta >= beta)
            return singular_beta;
        else if (se_score >= beta && abs(se_score) < MATE_BOUND)
                return se_score;
        // Extensiones negativas
        else if (tt_score >= beta)
            singular_extension = -3 + bEsPV;
        else if (bIsCutNode)
            singular_extension = -2;
        else if (tt_score <= alpha)
            singular_extension = -1;
    }

    //
    // Generar movimientos
    //
    MoveList moves;
    generate_moves(pos, &moves, GEN_ALL);
    int move_scores[256];
    score_moves(&moves, move_scores, tt_move, ply, prev_move, pos, sd);

    int legal_moves = 0;
    int best_score = -MATE_SCORE;
    Move best_move_this_node = MOVE_NONE;

    Move child_pv[MAX_PLY];
    Position child; // Copia para hacer movimientos

    // Movimientos probados (para history gravity)
    Move quiets_tried[64];
    int quiets_tried_count = 0;
    Move captures_tried[32];
    int captures_tried_count = 0;

    // Para LMR: calcular info del movimiento previo una sola vez fuera del bucle
    PrevMoveInfo prev_info_lmr = { -1, 0 };
    if (prev_move != MOVE_NONE)
        prev_info_lmr = get_prev_move_info(pos, prev_move);

    // Node TM: Resetear contadores de nodos por movimiento raíz
    if (ply == 0 && sd->thread_id == 0)
    {
        root_best_move_nodes = 0;
        root_total_nodes = 0;
    }

    //
    // Bucle principal
    //
    for (int i = 0; i < moves.count; i++)
    {
        pick_move(&moves, move_scores, i);
        if (moves.moves[i] == excluded_move)
            continue;

        const bool bEsCaptura = GET_MOVE_CAPTURE(moves.moves[i]);
        const bool bEsPromo   = GET_MOVE_PROMOTED(moves.moves[i]);

        // Si la jugada da jaque. Se calcula ANTES de mover y una sola vez: lo usan
        // las podas estaticas, LMP y la extension de jaque. Antes se calculaba dos
        // veces (aqui con gives_check() y despues de mover con is_square_attacked()).
        // Verificado equivalente sobre 11 millones de jugadas, 0 discrepancias.
        const int bDaJaque = gives_check(pos, moves.moves[i]);

        //
        // Podas estáticas contra el movimiento
        //
        if (legal_moves > 0 && !bEsPromo && !in_check && !bEsPV && abs(static_eval) < MATE_BOUND && bTieneAlgunaPieza && !bDaJaque)
        {
            // OJO: en capturas utilizamos depth, mientras que en no capturas utilizamos lmrDepth
            if (bEsCaptura)
            {
                //
                // Capturas
                //

                int threshold = -INFINITO;
                bool check_see = false;

                // Capture Futility Pruning (CFP)
                #if (0)
                if (depth <= CFP_DEPTH)
                {
                    int cfp_threshold = alpha - static_eval - CFP_MARGIN - depth * CFP_MULT;
                    threshold = max(threshold, cfp_threshold);
                    check_see = true;
                }
                #endif

                // SEE Pruning (SEEP) para capturas
                if (depth <= BUS_SEEP_NOISY_DEPTH)
                {
                    int source = GET_MOVE_SOURCE(moves.moves[i]);
                    int target = GET_MOVE_TARGET(moves.moves[i]);
                    int side = pos->side_to_move;
                    int enemy = side ^ 1;
                    int attacker = get_piece_type_at(pos, source, side);
                    int victim = get_piece_type_at(pos, target, enemy);
                    if (GET_MOVE_ENPASSANT(moves.moves[i]))
                        victim = PAWN;

                    if (attacker != -1 && victim != -1)
                    {
                        int hist = sd->capture_history[side * 6 + attacker][target][victim];
                        int d = depth + (static_eval <= alpha);
                        int seep_threshold = -BUS_SEEP_NOISY_MARGIN * d * d - hist / BUS_SEEP_NOISY_HIST_DIV;
                        
                        threshold = max(threshold, seep_threshold);
                        check_see = true;
                    }
                }
                
                // SEE Pruning (SEEP) para capturas ** otra implementación
                if (depth <= BUS_SEEP_NOISY_DEPTH2)
                {
                    int seep2_threshold = -BUS_SEEP_NOISY_MARGIN2 * depth;
                    threshold = max(threshold, seep2_threshold);
                    check_see = true;
                }
                
                if (check_see && !see(pos, moves.moves[i], threshold))
                    continue;
            }
            else
            {
                //
                // No capturas
                //

                const int lmrDepth = max(depth - (lmr_red(legal_moves) + lmr_red(depth)) / 2, 1);
                const int source = GET_MOVE_SOURCE(moves.moves[i]);
                const int target = GET_MOVE_TARGET(moves.moves[i]);
                const int hist_score = sd->history_moves[pos->side_to_move][source][target];

                // Futility Pruning (FP)
                #if (0)
                if (lmrDepth <= BUS_FUT_DEPTH
                    && static_eval <= alpha - BUS_FUT_BASE - lmrDepth * BUS_FUT_MARGIN)
                {
                    continue;
                }
                #endif

                // History Pruning (HP)
                if (lmrDepth <= BUS_HP_DEPTH
                    && hist_score < -BUS_HP_MARGIN * depth)
                {
                    continue;
                }

                int threshold = -INFINITO;
                bool check_see = false;

                // SEE Pruning con historia (SEEP)
                #if (0)
                if (lmrDepth <= BUS_SEEP_DEPTH)
                {
                    int seep_threshold = -BUS_SEEP_MARGIN * lmrDepth - hist_score / BUS_SEEP_DIV;
                    //seep_threshold = min(seep_threshold, 0); // Más prudente
                    threshold = max(threshold, seep_threshold);
                    check_see = true;
                }
                #endif

                // SEE Pruning only depth (SEEPOD)
                if (lmrDepth <= BUS_SEEPOD_DEPTH)
                {
                    int seepod_threshold = -BUS_SEEPOD_MARGIN * lmrDepth * lmrDepth;
                    threshold = max(threshold, seepod_threshold);
                    check_see = true;
                }

                if (check_see && !see(pos, moves.moves[i], threshold))
                    continue;

                // Continuation History Pruning (CHP)
                // OJO: revisar de nuevo la implementación de SF 18, que es más completa que la mía
                #if (0)
                if (lmrDepth <= BUS_CHP_DEPTH
                    && prev_info_lmr.piece >= 0)
                {
                    int cont_hist = ATOMIC_LOAD(continuation_history[prev_info_lmr.piece][prev_info_lmr.to][source][target]);
                    
                    if (cont_hist < -BUS_CHP_MARGIN * depth)
                        continue;
                }
                #endif
            }
        }

        //
        // LMP (Late Move Pruning)
        //
        // Va ANTES de copy_position + make_move. Antes iba despues, y cada jugada
        // podada tiraba a la basura la actualizacion incremental del acumulador NNUE,
        // que es el 82 % del coste de make_move (0,075 de 0,092 us medidos).
        //
        // Ademas la jugada ya se habia anotado en quiets_tried[], asi que recibia
        // malus de history y de continuation history sin haber sido buscada jamas.
        // Al podar aqui, quiets_tried[] solo contiene jugadas realmente buscadas.
        //
        // OJO: legal_moves todavia NO incluye la jugada actual, de ahi el >= en vez
        // del > que habia cuando la comprobacion iba despues de legal_moves++.
        if (bPodable
            && !bDaJaque
            && depth <= LMP_DEPTH
            && legal_moves >= (LMP_BASE + depth * depth) / (bMejorando ? 1 : 2)
            && !bEsCaptura
            && !bEsPromo
            && bTieneAlgunaPieza)
        {
            continue;
        }

        copy_position(&child, pos);
        child.prev = (Position*)pos; // Enlazar para detección de repeticiones
        
        // Si el movimiento es ilegal, lo saltamos
        if (!make_move(&child, moves.moves[i]))
            continue;

        // Traer ya la linea de cache de la TT del hijo: para cuando el nodo hijo la
        // sondee, la memoria estara en camino.
        tt_prefetch(child.hash);
        
        legal_moves++;

        // Registrar movimientos probados (para history gravity)
        if (!bEsCaptura && !bEsPromo && quiets_tried_count < 64)
            quiets_tried[quiets_tried_count++] = moves.moves[i];
        else if (bEsCaptura && captures_tried_count < 32)
            captures_tried[captures_tried_count++] = moves.moves[i];

        if (sd->thread_id == 0 && ply == 0 && ATOMIC_LOAD(search_flag_infinite) && depth > 15)
        {
            char s_move[8];
            move_to_str(moves.moves[i], s_move);
            printf("info currmove %s currmovenumber %d\n", s_move, legal_moves);   // una sola escritura (B19)
            fflush(stdout);
        }
        
        // Extensión de jaque (acumulando la singular, si la hay)
        int extension = bDaJaque;
        if (moves.moves[i] == tt_move)
            extension += singular_extension;

        //
        // Control de extensiones
        //
        int extensions_used = (ply + depth) - sd->search_root_depth;
        int max_extensions = (int)(sd->search_root_depth * EXT_LIMIT_MULTIPLIER);

        if (extensions_used >= max_extensions)
        {
            // Si ya superamos el límite, no permitir más extensiones POSITIVAS.
            //
            // OJO: antes se ponía a 0 sin más, lo que también anulaba las extensiones
            // negativas de la singular. Es decir, justo cuando se había decidido que el
            // árbol ya era demasiado grande, se dejaba de recortarlo.
            extension = min(extension, 0);
        }
        else if (extensions_used + extension > max_extensions)
            extension = max_extensions - extensions_used;   // Si la extensión propuesta nos pasaría del límite, recortarla

        //
        // PVS
        //
        
        // Determinar si el nodo hijo será Cut Node o All Node
        bool child_cut_node = !bIsCutNode;
        if (bEsPV)
            child_cut_node = (legal_moves > 1);

        int score;

        // Node TM: Registrar nodos antes de buscar este movimiento
        long long root_nodes_before = 0;
        if (ply == 0 && sd->thread_id == 0)
            root_nodes_before = ATOMIC_LOAD(sd->nodes);

        if (legal_moves == 1)
        {
            // Primer movimiento (PV): Búsqueda con ventana completa
            score = -negamax(&child, -beta, -alpha, depth - 1 + extension, ply + 1, moves.moves[i], MOVE_NONE, child_pv, child_cut_node, sd);
            if (ATOMIC_LOAD(stop_search))
                return 0;
        }
        else
        {
            //
            // Reducciones (LMR)
            //
            int reduction = 0;
            if (depth >= LMR_BASE_DEPTH
                && legal_moves >= LMR_BASE_MOVE
                && (!bEsCaptura || !see(pos, moves.moves[i], 0))
                && !bEsPromo
                && moves.moves[i] != sd->killer_moves[ply][0]
                && moves.moves[i] != sd->killer_moves[ply][1])
            {
                if (bDaJaque)
                    reduction += LMR_CHECK_DEPTH;   // -
                if (bIsCutNode)
                    reduction += LMR_CUT_DEPTH;     // +
                if (bMejorando)
                    reduction += LMR_MEJO_DEPTH;    // -
                if (!in_check && static_eval > beta - LMR_GOODMOVE_MARGIN)
                    reduction += LMR_EVAL_DEPTH;    // +
                if (tt_depth >= depth)
                    reduction += LMR_TTD_DEPTH;     // -
                if (GET_MOVE_CAPTURE(tt_move))
                    reduction += LMR_TTCAPT_DEPTH;  // +

                // Ajuste por History/Continuation: reducir menos si es buen movimiento
                if (!bEsCaptura)
                {
                    int source = GET_MOVE_SOURCE(moves.moves[i]);
                    int target = GET_MOVE_TARGET(moves.moves[i]);
                    int hist_score = sd->history_moves[pos->side_to_move][source][target];

                    if (prev_info_lmr.piece >= 0) // Usar la info pre-calculada
                        hist_score += ATOMIC_LOAD(continuation_history[prev_info_lmr.piece][prev_info_lmr.to][source][target]) / 2;

                    if (hist_score >= LMR_HIST_MENOS2)
                        reduction -= 6; // 2 * 3
                    else if (hist_score >= LMR_HIST_MENOS1)
                        reduction -= 3; // 1 * 3
                    else if (hist_score <= LMR_HIST_MAS1 && legal_moves >= LMR_BASE_MOVE + 6)
                        reduction += 3; // 1 * 3
                }

                // Ajuste por Capture History: capturas con SEE < 0 que entran en LMR
                if (bEsCaptura)
                {
                    int source = GET_MOVE_SOURCE(moves.moves[i]);
                    int target = GET_MOVE_TARGET(moves.moves[i]);
                    int side = pos->side_to_move;
                    int enemy = side ^ 1;
                    int attacker = get_piece_type_at(pos, source, side);
                    int victim = get_piece_type_at(pos, target, enemy);
                    if (GET_MOVE_ENPASSANT(moves.moves[i]))
                        victim = PAWN;

                    if (attacker != -1 && victim != -1)
                    {
                        int capt_hist = sd->capture_history[side * 6 + attacker][target][victim];

                        if (capt_hist <= LMR_CHIST_MAS1)
                            reduction += 3; // 1 * 3
                        else if (capt_hist >= LMR_CHIST_MENOS1)
                            reduction -= 3; // 1 * 3
                    }
                }

                // Ajuste por clasificador tactico
                if (bWillImprove)
                {
                    reduction += LMR_TC_WILLIMPROVE;
                    reduction += LMR_TC_WLLIMPBETA * (static_eval > beta);
                }
                if (bWillWorsen)
                {
                    reduction += LMR_TC_WILLWORSEN;
                    reduction += LMR_TC_WILWORALPHA * (static_eval < alpha);
                }
                if (bIsStable)
                    reduction += LMR_TC_ISSTABLE;

                // Aplicar el factor de escala logarítmico suavizado a la suma de modificadores
                // lmr_red(depth) devuelve: 0 (d<4), 1 (d=4..7), 2 (d=8..15), 3 (d=16..31), 4 (d=32..63)
                // Dividido por 2:
                // - d < 4   -> factor 0   (modificadores ignorados cerca de las hojas)
                // - d 4-7   -> factor 0.5 (mitad de impacto)
                // - d 8-15  -> factor 1   (comportamiento estándar)
                // - d 16-31 -> factor 1.5 (peso incrementado)
                // - d 32+   -> factor 2+  (heurísticas muy agresivas contra la explosión del árbol)
                const int scale_factor = lmr_red(depth);
                
                if (LMR_MODIF_SCALE
                    && (reduction > 0 || depth > 15)) // Evito escalar reducciones negativas a bajas profundidades para preservar su función de protección
                {
                    reduction = (reduction * scale_factor) / 2;
                }

                // Las reducciones que aplicamos son tercios de ply; si reducimos 6, es una reducción de 6/3 = 2
                reduction = RedondeoSimetricoBase3(reduction);

                // Reducción base logarítmica
                reduction += (lmr_red(legal_moves) + lmr_red(depth)) / 2;

                // Acotar
                reduction = clamp_int(reduction, 0, depth - 1);
            }

            // Movimientos restantes: Búsqueda con ventana nula (Zero Window Search)
            score = -negamax(&child, -alpha - 1, -alpha, depth - 1 - reduction + extension, ply + 1, moves.moves[i], MOVE_NONE, child_pv, child_cut_node, sd);
            if (ATOMIC_LOAD(stop_search)) return 0;

            // Si LMR falla (el movimiento es mejor de lo esperado), re-buscar sin reducción
            if (reduction > 0 && score > alpha)
            {
                score = -negamax(&child, -alpha - 1, -alpha, depth - 1 + extension, ply + 1, moves.moves[i], MOVE_NONE, child_pv, child_cut_node, sd);
                if (ATOMIC_LOAD(stop_search)) return 0;
            }

            // Si falla alto (mejor que alpha), re-buscar con ventana completa
            if (score > alpha && score < beta)
            {
                score = -negamax(&child, -beta, -alpha, depth - 1 + extension, ply + 1, moves.moves[i], MOVE_NONE, child_pv, false, sd);
                if (ATOMIC_LOAD(stop_search)) return 0;
            }
        }

        // Node TM: Calcular nodos consumidos por este movimiento
        long long root_move_nodes = 0;
        if (ply == 0 && sd->thread_id == 0)
        {
            root_move_nodes = ATOMIC_LOAD(sd->nodes) - root_nodes_before;
            root_total_nodes += root_move_nodes;
        }

        //
        // Poda y actualización
        //
        if (score > best_score)
        {
            best_score = score;
            best_move_this_node = moves.moves[i];

            // Node TM: Registrar nodos del mejor movimiento
            if (ply == 0 && sd->thread_id == 0)
                root_best_move_nodes = root_move_nodes;

            // Actualizar PV: Mejor movimiento + PV del hijo
            pv[0] = moves.moves[i];
            int j = 0;
            // OJO: (j + 2) y no (j + 1): tras el bucle se escribe pv[j + 1], que con
            // la condición antigua podía valer pv[MAX_PLY] y salirse del array.
            while (child_pv[j] != MOVE_NONE && (j + 2) < MAX_PLY)
            {
                pv[j + 1] = child_pv[j];
                j++;
            }
            pv[j + 1] = MOVE_NONE;
        }
        
        if (score > alpha)
        {
            alpha = score;
        }
        
        if (alpha >= beta)
        {
            // (La entrada de TT se escribe una sola vez, al salir del bucle: escribirla
            //  aquí además era trabajo duplicado y guardaba beta en vez de best_score.)

            // Killer Heuristic: Guardar movimientos quietos que causan un corte
            if (!bEsCaptura && ply < MAX_PLY)
            {
                sd->killer_moves[ply][1] = sd->killer_moves[ply][0];
                sd->killer_moves[ply][0] = moves.moves[i];

                // History Heuristic: Premiar jugadas que causan corte
                int bonus = min(depth * depth, 16384);   // por encima del divisor de la gravedad diverge (B24)
                int side = pos->side_to_move;
                int source = GET_MOVE_SOURCE(moves.moves[i]);
                int target = GET_MOVE_TARGET(moves.moves[i]);

                update_history_value(&sd->history_moves[side][source][target], bonus);

                // Counter Move Heuristic
                if (prev_move != MOVE_NONE)
                    sd->counter_moves[side][GET_MOVE_SOURCE(prev_move)][GET_MOVE_TARGET(prev_move)] = moves.moves[i];

                // Continuation History: Premiar movimiento que causa corte en contexto del movimiento previo
                if (prev_move != MOVE_NONE)
                {
                    PrevMoveInfo prev_info = get_prev_move_info(pos, prev_move);
                    if (prev_info.piece >= 0)
                        update_history_value_atomic(&continuation_history[prev_info.piece][prev_info.to][source][target], bonus);
                }

                // History Gravity: Penalizar movimientos quietos que NO causaron corte
                PrevMoveInfo prev_info_grav;
                int has_prev_info_grav = 0;
                if (prev_move != MOVE_NONE)
                {
                    prev_info_grav = get_prev_move_info(pos, prev_move);
                    has_prev_info_grav = (prev_info_grav.piece >= 0);
                }

                for (int q = 0; q < quiets_tried_count; q++)
                {
                    if (quiets_tried[q] == moves.moves[i])
                        continue; // No penalizar el movimiento que causó corte (ya fue premiado)

                    int q_source = GET_MOVE_SOURCE(quiets_tried[q]);
                    int q_target = GET_MOVE_TARGET(quiets_tried[q]);

                    // Malus en history_moves
                    update_history_value(&sd->history_moves[side][q_source][q_target], -bonus);

                    // Malus en continuation history
                    if (has_prev_info_grav)
                        update_history_value_atomic(&continuation_history[prev_info_grav.piece][prev_info_grav.to][q_source][q_target], -bonus);
                }
            }
            // Capture History: Premiar capturas que causan corte
            else if (bEsCaptura)
            {
                int cap_bonus = min(depth * depth, 400);
                int side = pos->side_to_move;
                int enemy = side ^ 1;

                // 1. Premiar la captura que causó el corte
                int source = GET_MOVE_SOURCE(moves.moves[i]);
                int target = GET_MOVE_TARGET(moves.moves[i]);
                int attacker = get_piece_type_at(pos, source, side);
                int victim = get_piece_type_at(pos, target, enemy);

                // Si es en passant, la víctima es un peón
                if (GET_MOVE_ENPASSANT(moves.moves[i])) victim = PAWN;

                if (attacker != -1 && victim != -1)
                    update_history_value(&sd->capture_history[side * 6 + attacker][target][victim], cap_bonus);

                // 2. Penalizar capturas realmente probadas que NO causaron corte
                for (int c = 0; c < captures_tried_count; c++)
                {
                    if (captures_tried[c] == moves.moves[i])
                        continue; // No penalizar la que causó corte

                    int bad_source = GET_MOVE_SOURCE(captures_tried[c]);
                    int bad_target = GET_MOVE_TARGET(captures_tried[c]);
                    int bad_attacker = get_piece_type_at(pos, bad_source, side);
                    int bad_victim = get_piece_type_at(pos, bad_target, enemy);
                    if (GET_MOVE_ENPASSANT(captures_tried[c])) bad_victim = PAWN;

                    if (bad_attacker != -1 && bad_victim != -1)
                        update_history_value(&sd->capture_history[side * 6 + bad_attacker][bad_target][bad_victim], -cap_bonus);
                }
            }

            break; // Poda Beta (Cutoff)
        }
    }

    // 4. Detección de Mate / Ahogado
    if (legal_moves == 0)
    {
        // En búsqueda singular, si no hay movimientos devuelvo -infinito
        if (excluded_move != MOVE_NONE)
            return(-MATE_SCORE);
        // En jaque y sin jugadas legales: -mate
        if (in_check)
            return -MATE_SCORE + ply; // Jaque Mate (preferimos mates más cercanos)
        else
            return 0; // Ahogado (Tablas)
    }

    // Correction history: aprender el sesgo de esta estructura de peones.
    //
    // Version 2 (B3, B4, B5):
    //  - Se aprende contra eval_base (eval de la red con la regla de 50, sin la correccion
    //    y sin sustituir por tt_score). Antes era contra static_eval: con la correccion
    //    ya sumada la media movil convergia a la MITAD del sesgo, y con tt_score se
    //    aprendia busqueda menos busqueda (y un mate de la TT saturaba la entrada).
    //  - Solo si el score es fiable en la direccion de la correccion: hacia abajo, si es
    //    una cota superior de verdad o exacto (best_score < beta); hacia arriba, si es
    //    una cota inferior o exacto (best_score > alfa original). Antes la guarda de
    //    subida era "hay mejor jugada", que se cumple casi siempre, y los fail-low
    //    (cotas superiores) subian la correccion.
    if (!in_check
        && excluded_move == MOVE_NONE
        && eval_base != -INFINITO
        && abs(best_score) < TB_BOUND
        && !(best_move_this_node != MOVE_NONE && GET_MOVE_CAPTURE(best_move_this_node))
        && ((best_score < eval_base && best_score < beta)
            || (best_score > eval_base && best_score > original_alpha)))
    {
        actualizar_correccion(pos, sd, best_score - eval_base, depth);
    }

    // Guardar en TT
    if (excluded_move == MOVE_NONE)
    {
        tt_flag = (best_score <= original_alpha) ? TT_ALPHA : (best_score >= beta ? TT_BETA : TT_EXACT);
        write_tt(pos->hash, depth, ply, best_score, tt_flag, best_move_this_node, raw_eval);
    }

    return best_score;
}

// Helper para calcular el tiempo asignado
static void calculate_time_allocation(const Position* pos, SearchLimits limits)
{
    static const int MOVESTOGO = 35;

    ATOMIC_STORE(search_time_soft, -1);
    ATOMIC_STORE(search_time_hard, -1);

    if (limits.infinite || limits.ponder)
        return;

    if (limits.movetime > 0)
    {
        ATOMIC_STORE(search_time_soft, limits.movetime);
        ATOMIC_STORE(search_time_hard, limits.movetime);
    }
    else if (limits.reloj) // Modo partida: llego reloj, aunque sea 0 o negativo (B12)
    {
        int time_left = (pos->side_to_move == WHITE) ? limits.wtime : limits.btime;
        int inc = (pos->side_to_move == WHITE) ? limits.winc : limits.binc;
        int movestogo = (limits.movestogo > 0) ? limits.movestogo : MOVESTOGO;

        // Margen de seguridad por lag (50 ms). Con menos de 100 ms se reserva la mitad
        // en vez de 50: restar 50 dejaba time_left = 0 y, con ello, hard = 0, que en
        // todo el motor significa 'sin limite' (I01).
        if (time_left > 100)
            time_left -= 50;
        else
            time_left = max(1, time_left / 2);

        double base_time;
        if (limits.movestogo > 0)
        {
             // Control "x jugadas en y minutos"
             // Reservar un pequeño buffer (+1) para no llegar a 0 exacto
             base_time = (double)time_left / (double)(movestogo + 1) + 3 * (double)inc / 4;
        }
        else
        {
             // Sudden death
             // Pero si hay incremento, la partida será más larga, así que podemos gastar un poco más por jugada.
             base_time = (double)time_left / MOVESTOGO + 3 * (double)inc / 4;
        }

        // Soft limit: Objetivo ideal de tiempo
        long long soft = (long long)base_time;
        
        // Hard limit: Máximo absoluto para esta jugada (permitimos extender hasta 5x si es crítico)
        long long hard = (long long)(base_time * 5.0);
        
        // Mínimos para evitar timeouts por granularidad del SO
        if (soft < 50)
            soft = 50;
        if (hard < 100)
            hard = 100;

        // Límites absolutos de seguridad (CRÍTICO: deben ir al final para no exceder time_left)
        if (hard > time_left)
            hard = time_left;
        if (soft > time_left * 0.7)
            soft = (long long)(time_left * 0.7);

        // Con movestogo hay que guardar reloj para las jugadas que faltan hasta el
        // control: con mtg <= 4, 5 * base ya era todo el reloj, y una iteracion que se
        // dispara podia dejar 50 ms para las ultimas jugadas (B11).
        if (limits.movestogo > 1)
        {
            const long long reserva = (long long)(limits.movestogo - 1) * time_left / (2LL * limits.movestogo);
            if (hard > time_left - reserva)
                hard = time_left - reserva;
            if (soft > hard)
                soft = hard;
        }

        // Con reloj finito el limite nunca puede ser 0: 0 significa 'sin limite'.
        if (hard < 1)
            hard = 1;
        if (soft < 1)
            soft = 1;

        ATOMIC_STORE(search_time_soft, soft);
        ATOMIC_STORE(search_time_hard, hard);
    }
}

// Variable estática para saber el bando (necesaria para ponderhit)
static int search_side;

// Fija el estado de control de la busqueda: limites, banderas de ponder/infinite,
// bando y reloj. La llama quien lanza la busqueda (engine_go, en el hilo UCI) ANTES de
// crear el hilo de busqueda. Antes lo hacia el propio hilo al arrancar, y un
// 'ponderhit' llegado antes de eso se perdia: el motor ponderaba para siempre y
// perdia por tiempo (B1). De paso, el reloj arranca al recibir 'go'.
void preparar_busqueda(const Position* pos, SearchLimits limits)
{
    current_limits = limits;
    ATOMIC_STORE(search_max_nodes, limits.nodes > 0 ? limits.nodes : 0);
    ATOMIC_STORE(search_flag_infinite, limits.infinite ? 1 : 0);
    ATOMIC_STORE(search_flag_ponder, limits.ponder ? 1 : 0);
    search_side = pos->side_to_move;

    search_start_time = get_time_ms();
    ATOMIC_STORE(search_time_soft, -1);
    ATOMIC_STORE(search_time_hard, -1);
    if (!limits.ponder && !limits.infinite)
        calculate_time_allocation(pos, limits);
}

int busqueda_espera_stop(void)
{
    return ATOMIC_LOAD(search_flag_infinite) || ATOMIC_LOAD(search_flag_ponder);
}

// UCI: en 'go infinite' y 'go ponder' el bestmove solo puede salir tras 'stop' o
// 'ponderhit' (I08). Se espera a la parada del GUI (stop_gui) y no a stop_search,
// porque esa tambien la pone el limite de nodos: con 'go ponder ... nodes N' el
// bestmove salia antes del ponderhit (B13).
static void esperar_stop_o_ponderhit(void)
{
    while (busqueda_espera_stop() && !ATOMIC_LOAD(stop_gui))
    {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}

// --- Lazy SMP Worker ---
#ifdef USE_SMP

#ifdef _WIN32
DWORD WINAPI search_worker(LPVOID lpParam)
#else
void* search_worker(void* lpParam)
#endif
{
    SearchData* sd = (SearchData*)lpParam;

    // Inicializar datos del hilo
    ATOMIC_STORE(sd->nodes, 0);
    sd->sel_depth = 0;
    preparar_heuristicas(sd);

    // Copia local de la posición raíz
    Position pos;
    copy_position(&pos, &sd->root_pos);

    // Bucle de búsqueda infinita (hasta que stop_search sea true)
    // Los helpers simplemente profundizan tanto como pueden
    // Diversificación simple: empezar en profundidad distinta
    int depth = 1 + (sd->thread_id % 4);
    Move root_pv[MAX_PLY];

    while (!ATOMIC_LOAD(stop_search))
    {
        sd->search_root_depth = depth;
        int alpha = -INFINITO;
        int beta = INFINITO;

        // Búsqueda
        negamax(&pos, alpha, beta, depth, 0, MOVE_NONE, MOVE_NONE, root_pv, false, sd);

        depth++;
        // Límite de seguridad
        if (depth >= MAX_PLY) break;
    }

    return 0;
}

#endif // USE_SMP

/*
 *
 * search_position
 * 
 */
void search_position(Position* pos, SearchLimits limits)
{
    // El estado de control (stop_search, limites, banderas, reloj) lo fija quien lanza
    // la busqueda, con preparar_busqueda(), ANTES de crear este hilo: si lo hiciera el
    // hilo, un 'stop' o un 'ponderhit' llegado antes de que arrancase se perdia (B1, I10).

    // Incrementar generación de TT (aging automático)
    tt_new_search();

    // --- Syzygy Root Probe (Solución a Ceguera de WDL / Shuffling) ---
    int pieces = count_bits(pos->all);
    // En infinite/ponder tambien se juega la jugada DTZ, pero el bestmove espera a
    // stop/ponderhit (I07). Antes se saltaba el atajo y tras el ponderhit decidia la
    // WDL a ciegas del arbol (B2).
    if (pieces <= opt_syzygy_probe_limit && (unsigned)pieces <= TB_LARGEST)
    {
        int castling = pos->castling_rights;
        int ep = (pos->en_passant_sq == SQ_NONE) ? 0 : pos->en_passant_sq;
        
        unsigned tb_res = tb_probe_root(
            pos->occupied[WHITE], pos->occupied[BLACK],
            pos->pieces[WHITE][KING] | pos->pieces[BLACK][KING],
            pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN],
            pos->pieces[WHITE][ROOK] | pos->pieces[BLACK][ROOK],
            pos->pieces[WHITE][BISHOP] | pos->pieces[BLACK][BISHOP],
            pos->pieces[WHITE][KNIGHT] | pos->pieces[BLACK][KNIGHT],
            pos->pieces[WHITE][PAWN] | pos->pieces[BLACK][PAWN],
            opt_syzygy_50move_rule ? pos->half_move_clock : 0,
            castling, ep, pos->side_to_move == WHITE,
            NULL
        );

        // Verificamos si las bases nos devuelven un movimiento válido.
        if (tb_res != TB_RESULT_FAILED && tb_res != TB_RESULT_CHECKMATE && tb_res != TB_RESULT_STALEMATE)
        {
            int from = TB_GET_FROM(tb_res);
            int to = TB_GET_TO(tb_res);
            int promotes_tb = TB_GET_PROMOTES(tb_res);
            
            int promoted = 0;
            switch (promotes_tb)
            {
                case TB_PROMOTES_QUEEN: promoted = QUEEN; break;
                case TB_PROMOTES_ROOK: promoted = ROOK; break;
                case TB_PROMOTES_BISHOP: promoted = BISHOP; break;
                case TB_PROMOTES_KNIGHT: promoted = KNIGHT; break;
            }

            // Validamos contra nuestro generador para asegurar legalidad absoluta en nuestra estructura
            MoveList root_moves;
            generate_moves(pos, &root_moves, GEN_ALL);
            Move tb_move = MOVE_NONE;
            for (int i = 0; i < root_moves.count; i++)
            {
                Move m = root_moves.moves[i];
                if (GET_MOVE_SOURCE(m) == from
                    && GET_MOVE_TARGET(m) == to
                    && GET_MOVE_PROMOTED(m) == promoted)
                {
                    Position test_pos;
                    copy_position(&test_pos, pos);
                    if (make_move(&test_pos, m))
                    {
                        tb_move = m;
                        break;
                    }
                }
            }

            if (tb_move != MOVE_NONE)
            {
                int wdl = TB_GET_WDL(tb_res);
                int dtz = TB_GET_DTZ(tb_res);
                
                int score = 0;
                if (wdl == TB_WIN)
                    score = TB_MAX - dtz;
                else if (wdl == TB_LOSS)
                    score = -TB_MAX + dtz;
                else if (wdl == TB_DRAW)
                    score = 0;
                else if (wdl == TB_CURSED_WIN)
                    score = opt_syzygy_50move_rule ? 0 : (TB_MAX - dtz);
                else if (wdl == TB_BLESSED_LOSS)
                    score = opt_syzygy_50move_rule ? 0 : (-TB_MAX + dtz);

                char s_score[32];
                char s_move[8];
                score_a_texto(score, s_score, sizeof(s_score));
                move_to_str(tb_move, s_move);
                printf("info depth 1 score %s time 0 nodes 0 pv %s\n", s_score, s_move);
                fflush(stdout);
                esperar_stop_o_ponderhit();
                
                printf("bestmove %s\n", s_move);
                fflush(stdout);

                // Retornar inmediatamente evita que inicie toda la búsqueda Iterativa 
                // y los hilos en Lazy SMP de forma innecesaria.
                last_search_nodes = 0;
                ATOMIC_STORE(search_flag_ponder, 0);
                ATOMIC_STORE(search_flag_infinite, 0);
                ATOMIC_STORE_REL(is_searching, 0);
                return;
            }
        }
    }

    // Configurar datos del hilo principal (ID 0)
    // SearchData ocupa ~90 KB (history, counter moves, capture history). En la pila
    // se comía casi una décima parte del hilo de búsqueda; al heap.
    SearchData* main_sd_ptr = obtener_sd_pool(1);
    if (!main_sd_ptr)
    {
        printf("info string Error: sin memoria para SearchData\n");
        printf("bestmove 0000\n");
        fflush(stdout);
        last_search_nodes = 0;
        ATOMIC_STORE(is_searching, 0);
        return;
    }
    main_sd_ptr->thread_id = 0;
    ATOMIC_STORE(main_sd_ptr->nodes, 0);
    main_sd_ptr->sel_depth = 0;
    preparar_heuristicas(main_sd_ptr);
    copy_position(&main_sd_ptr->root_pos, pos);

    // --- Lazy SMP: Lanzar helpers ---
#ifdef USE_SMP
    int num_helpers = opt_threads - 1;
    SearchData* helper_data = NULL;

#ifdef _WIN32
    HANDLE* helper_threads = NULL;
#else
    pthread_t* helper_threads = NULL;
#endif

    if (num_helpers > 0)
    {
        // obtener_sd_pool puede mover el bloque: se recalcula tambien main_sd_ptr.
        SearchData* pool = obtener_sd_pool(num_helpers + 1);
        if (pool)
        {
            main_sd_ptr = &pool[0];
            helper_data = &pool[1];
        }
        if (!helper_data)
        {
            printf("info string Error: Failed to allocate memory for helper data\n");
            num_helpers = 0;
        }
    }

    if (num_helpers > 0)
    {
#ifdef _WIN32
        helper_threads = (HANDLE*)malloc(num_helpers * sizeof(HANDLE));
#else
        helper_threads = (pthread_t*)malloc(num_helpers * sizeof(pthread_t));
#endif

        if (!helper_threads)
        {
            printf("info string Error: Failed to allocate memory for helper threads\n");
            num_helpers = 0;
        }

        for (int i = 0; i < num_helpers; i++)
        {
            helper_data[i].thread_id = i + 1;
            ATOMIC_STORE(helper_data[i].nodes, 0);   // la info de profundidad 1 los suma antes de que arranque el hilo (B29)
            helper_data[i].sel_depth = 0;
            copy_position(&helper_data[i].root_pos, pos);

#ifdef _WIN32
            // 8 MB de pila: negamax gasta 4512 bytes por nivel y puede anidar hasta
            // MAX_PLY, lo que se acerca peligrosamente al 1 MB por defecto.
            helper_threads[i] = CreateThread(NULL, 8 << 20, search_worker, &helper_data[i], 0, NULL);
            if (helper_threads[i] == NULL)
            {
                printf("info string Error: Failed to create thread %d\n", i+1);
                // Stop creating threads, but keep the ones created
                num_helpers = i;
                break;
            }
#else
            pthread_attr_t attr_helper;
            pthread_attr_init(&attr_helper);
            pthread_attr_setstacksize(&attr_helper, 8 << 20);
            if (pthread_create(&helper_threads[i], &attr_helper, search_worker, &helper_data[i]) != 0)
            {
                printf("info string Error: Failed to create thread %d\n", i+1);
                num_helpers = i;
                break;
            }
#endif
        }
    }
#endif // USE_SMP

    // Limpiar heurísticas al inicio de una nueva búsqueda
    // (Ya hecho en memset arriba para main_sd)

    // Inicializar historial de iteraciones para heurísticas de tiempo
    prev_growth_count = 0;
    stability_count = 0;
    root_best_move_nodes = 0;
    root_total_nodes = 0;
    for (int i = 0; i < 3; i++)
    {
        prev_growth_factors[i] = 0.0;
        best_move_history[i] = MOVE_NONE;
        score_history[i] = 0;
    }

    // --- Gestión de Tiempo ---
    // (El reloj y los limites ya los fijo preparar_busqueda() al recibir 'go'.)
    

    int depth = limits.depth;
    if (depth == 0 || limits.infinite || limits.ponder)
        depth = MAX_PLY - 1; // Si es infinite, ponemos un límite alto (respetando buffer PV)
    if (depth > MAX_PLY - 1)
        depth = MAX_PLY - 1; // 'go depth 250': la history divergia y desbordaba int (B24)

    // Si hay tiempo definido, limitamos la profundidad máxima para evitar bucles infinitos accidentales
    if (ATOMIC_LOAD(search_time_hard) > 0 && limits.depth == 0)
        depth = MAX_PLY - 1;

    Move best_move = MOVE_NONE;
    Move ponder_move = MOVE_NONE;
    // Datos de la jugada que se va a jugar, para la info final: la de la ultima
    // iteracion completa o la de un fail-high posterior (entonces su score es una cota
    // inferior). Antes se mezclaba la jugada del fail-high con el score viejo (B14).
    int ultima_prof = 0;
    int ultima_sel = 0;
    int score_jugada = 0;
    int cota_inferior = 0;
    int prev_score = 0;
    long long last_iter_time = 0;
    long long last_iter_nodes = 0;

    //
    // Iterative Deepening con Aspiración
    //
    for (int current_depth = 1; current_depth <= depth; current_depth++)
    {
        long long iter_start_time = get_time_ms();
        long long iter_start_nodes = ATOMIC_LOAD(main_sd_ptr->nodes);

        Move root_pv[MAX_PLY];
        root_pv[0] = MOVE_NONE;
        int score = 0;

        int alpha = -INFINITO;
        int beta = INFINITO;
        int delta = 10;  // OJO: poner como parámetro UCI
        int fail_lows = 0;
        int fail_highs = 0;
        const int MAX_ASPIRATION_FAILS = 5; // OJO: poner como parámetro UCI

        // Activar aspiración solo a partir de depth 5
        if (current_depth >= 5 && abs(prev_score) < MATE_BOUND)
        {
            alpha = max(-INFINITO, prev_score - delta);
            beta = min(INFINITO, prev_score + delta);
        }

        int search_depth = current_depth;

        // Bucle de re-búsqueda por fallos de aspiración
        while (1)
        {
            // Establecer profundidad raíz para control de extensiones
            main_sd_ptr->search_root_depth = search_depth;

            score = negamax(pos, alpha, beta, search_depth, 0, MOVE_NONE, MOVE_NONE, root_pv, false, main_sd_ptr);

            // Si se detuvo la búsqueda, salir inmediatamente
            if (ATOMIC_LOAD(stop_search))
            {
                // Protección: guardar PV parcial si tenemos algo
                if (current_depth == 1 && best_move == MOVE_NONE && root_pv[0] != MOVE_NONE)
                    best_move = root_pv[0];
                break;
            }

            // Fail low
            if (score <= alpha)
            {
                fail_lows++;

                // Después de MAX_ASPIRATION_FAILS fallos, abrir ventana completa
                if (fail_lows + fail_highs >= MAX_ASPIRATION_FAILS)
                {
                    alpha = -INFINITO;
                    beta = INFINITO;
                }
                else
                {
                    // Expandir ventana gradualmente (×1.5 en lugar de ×2)
                    delta = delta + delta / 2;
                    alpha = max(-INFINITO, score - delta); // OJO: debería beta = (alfa+beta)/2, ajustar alpha, finalmente ampliar delta
                }

                // Info para debug
                if (salida_busqueda && (ATOMIC_LOAD(main_sd_ptr->nodes) >= 100000 || limits.infinite))
                {
                    long long current_time = get_time_ms();
                    long long total_nodes_fl = ATOMIC_LOAD(main_sd_ptr->nodes);
#ifdef USE_SMP
                    if (helper_data)
                    {
                        for (int i = 0; i < num_helpers; i++)
                            total_nodes_fl += ATOMIC_LOAD(helper_data[i].nodes);
                    }
#endif
                    char s_score[32];
                    char s_pv[1024];
                    score_a_texto(score, s_score, sizeof(s_score));
                    pv_a_texto(root_pv, s_pv, sizeof(s_pv));
                    printf("info depth %d score %s upperbound nodes %lld time %lld pv %s\n",
                           current_depth, s_score, total_nodes_fl, current_time - search_start_time, s_pv);
                    fflush(stdout);
                }
                continue;
            }

            // Fail high
            if (score >= beta)
            {
                fail_highs++;

                if (search_depth > 1)
                    search_depth--;

                // Guardar movimiento provisional por si se acaba el tiempo
                if (root_pv[0] != MOVE_NONE)
                {
                    best_move = root_pv[0];
                    ponder_move = root_pv[1];   // si no, podia quedar la de otra jugada (I17)
                    ultima_prof = current_depth;
                    ultima_sel = main_sd_ptr->sel_depth;
                    score_jugada = score;
                    cota_inferior = 1;
                }

                // Después de MAX_ASPIRATION_FAILS fallos, abrir ventana completa
                if (fail_lows + fail_highs >= MAX_ASPIRATION_FAILS)
                {
                    alpha = -INFINITO;
                    beta = INFINITO;
                }
                else
                {
                    // Expandir ventana gradualmente
                    delta = delta + delta / 2;
                    beta = min(INFINITO, score + delta); // OJO: debería ajustar beta primero, después ampliar delta
                }

                // Info para debug
                if (salida_busqueda && (ATOMIC_LOAD(main_sd_ptr->nodes) >= 100000 || limits.infinite))
                {
                    long long current_time = get_time_ms();
                    long long total_nodes_fh = ATOMIC_LOAD(main_sd_ptr->nodes);
#ifdef USE_SMP
                    if (helper_data) {
                        for (int i = 0; i < num_helpers; i++)
                            total_nodes_fh += ATOMIC_LOAD(helper_data[i].nodes);
                    }
#endif
                    char s_score[32];
                    char s_pv[1024];
                    score_a_texto(score, s_score, sizeof(s_score));
                    pv_a_texto(root_pv, s_pv, sizeof(s_pv));
                    printf("info depth %d score %s lowerbound nodes %lld time %lld pv %s\n",
                           current_depth, s_score, total_nodes_fh, current_time - search_start_time, s_pv);
                    fflush(stdout);
                }
                continue;
            }

            // Éxito: la búsqueda está dentro de la ventana [alpha, beta]
            break;
        }

        // Si se detuvo la búsqueda, salir del bucle principal
        if (ATOMIC_LOAD(stop_search))
            break;

        // Si encontramos un mate a favor mejor que en la búsqueda anterior (con prof >= 10), mover rápido.
        // Solo con reloj: con 'go depth' o 'go nodes' el resultado dependia de la busqueda
        // anterior (B30). Con las banderas, y no con limits, vale tambien tras un ponderhit.
        if (ATOMIC_LOAD(search_time_hard) > 0 && !busqueda_espera_stop()
            && current_depth >= 10 && score >= MATE_BOUND && score > previous_search_score)
        {
            ATOMIC_STORE(stop_search, 1);
        }

        //
        // Iteración completada
        //
        
        // Guardar valores anteriores para heurísticas de estabilidad
        int last_score = prev_score;
        Move last_best_move = best_move;

        prev_score = score;

        if (root_pv[0] != MOVE_NONE)
        {
            best_move = root_pv[0];
            ponder_move = root_pv[1];
            ultima_prof = current_depth;
            ultima_sel = main_sd_ptr->sel_depth;
            score_jugada = score;
            cota_inferior = 0;

            if (salida_busqueda)
            {
                long long current_time = get_time_ms();
                long long elapsed = current_time - search_start_time;

                // Calcular nodos totales (Main + Helpers)
                long long total_nodes = ATOMIC_LOAD(main_sd_ptr->nodes);
#ifdef USE_SMP
                if (helper_data)
                {
                    for (int i = 0; i < num_helpers; i++)
                        total_nodes += ATOMIC_LOAD(helper_data[i].nodes);
                }
#endif

                char s_score[32];
                char s_pv[1024];
                score_a_texto(score, s_score, sizeof(s_score));
                pv_a_texto(root_pv, s_pv, sizeof(s_pv));
                printf("info depth %d seldepth %d score %s nodes %lld time %lld pv %s\n",
                       current_depth, main_sd_ptr->sel_depth, s_score, total_nodes, elapsed, s_pv);
                fflush(stdout);
            }
        }

        long long iter_end_time = get_time_ms();
        long long current_iter_time = iter_end_time - iter_start_time;
        
        // Prevenir tiempos nulos debido a la baja resolución de GetTickCount64 en Windows (~15.6ms).
        // Un tiempo de 0 arruina la predicción de la siguiente iteración y dispara el aborto prematuro.
        if (current_iter_time <= 0)
            current_iter_time = 1;
            
        long long current_iter_nodes = ATOMIC_LOAD(main_sd_ptr->nodes) - iter_start_nodes;

        // Actualizar contador de estabilidad
        if (best_move == last_best_move && best_move != MOVE_NONE)
            stability_count++;
        else
            stability_count = 0;

        // --- Gestión de Tiempo Avanzada ---
        long long hard_limit = ATOMIC_LOAD(search_time_hard);
        if (hard_limit > 0 && current_depth > 1)
        {
            long long time_used = get_time_ms() - search_start_time;

            // 1. Hard Limit: Parada obligatoria
            if (time_used >= hard_limit)
            {
                ATOMIC_STORE(stop_search, 1);
                break;
            }

            long long soft_limit = ATOMIC_LOAD(search_time_soft);
            // 2. Soft Limit y Heurísticas de parada
            if (soft_limit > 0 && !ATOMIC_LOAD(search_flag_ponder) && limits.movetime == 0)
            {
                // Factor de estabilidad
                double stability_factor = 1.0;

                // Node TM: Ajustar según fracción de nodos del mejor movimiento
                if (root_total_nodes > 0 && current_depth >= 8)
                {
                    double best_fraction = (double)root_best_move_nodes / (double)root_total_nodes;
                    // Aumentar tiempo si hay dudas (<50%), reducir solo si es muy obvia (>90%)
                    double node_factor = 1.0;
                    if (best_fraction < 0.5)
                        node_factor = 1.5 - best_fraction; // Escala de 1.0 a 1.5
                    else if (best_fraction > 0.9)
                        node_factor = 1.0 - (best_fraction - 0.9) * 2.0; // Escala de 1.0 a 0.8

                    node_factor = clamp_double(node_factor, 0.5, 1.5);
                    stability_factor *= node_factor;
                }

                // Si el mejor movimiento ha cambiado respecto a la iteración anterior, extender tiempo
                if (last_best_move != MOVE_NONE && best_move != last_best_move)
                    stability_factor *= 1.5;

                // Si el score ha caído significativamente (fail low), extender para resolver
                if (current_depth > 5 && score < last_score - 30)
                    stability_factor *= 1.5;

                // Caída acumulativa
                if (current_depth > 5 && score < score_history[2] - 50)
                    stability_factor *= 1.4;

                // Si hubo fallos de aspiración hacia abajo (posición peor de lo esperado), necesitamos más tiempo
                if (fail_lows > 0)
                    stability_factor *= 1.0 + 0.25 * fail_lows;
                
                // Si hubo fallos hacia arriba (posición mejor), no necesitamos apenas tiempo extra,
                // a menos que la jugada haya cambiado (lo cual ya se penaliza más arriba).
                if (fail_highs > 0)
                    stability_factor *= 1.0 + 0.01 * fail_highs;

                // Reducciones de tiempo
                if (fail_lows + fail_highs >= MAX_ASPIRATION_FAILS)
                    stability_factor *= 0.9;

                // Si es mate inminente o final resuelto por tablebases, no gastar tiempo
                if (abs(score) >= TB_MAX - MAX_PLY)
                    stability_factor *= 0.5;

                // Ajuste por evaluación
                if (current_depth >= 8 && abs(score) < MATE_BOUND)
                {
                    double score_factor = 1.0;
                    // Solo reducir tiempo si la ventaja es clara y sólida
                    if (score > 200 && current_depth >= 14)
                        score_factor = 1.0 - (double)(score - 200) / 1000.0;
                    // Si estamos perdiendo o en apuros (< -0.50), usar más tiempo para defender
                    else if (score < -50)
                        score_factor = 1.0 - (double)(score + 50) / 400.0;
                        
                    // Acotar a márgenes de seguridad para evitar locuras con scores extremos
                    score_factor = clamp_double(score_factor, 0.70, 1.50);
                    
                    stability_factor *= score_factor;
                }

                // Estabilidad continua: cuanto más tiempo lleva el best move sin cambiar, más reducimos.
                if (current_depth >= 14 && stability_count >= 6 && best_move != MOVE_NONE)
                {
                    int capped = stability_count < 10 ? stability_count : 10;
                    stability_factor *= 1.0 - 0.015 * capped;
                }

                // Limitar stability_factor para evitar exceder hard limit
                if (stability_factor > 3.0)
                    stability_factor = 3.0;

                long long current_soft = (long long)(soft_limit * stability_factor);
                if (current_soft > hard_limit)
                    current_soft = hard_limit;

                // Parar si excedemos el soft limit ajustado
                if (time_used >= current_soft)
                {
                    ATOMIC_STORE(stop_search, 1);
                    break;
                }

                // Predicción de coste de la siguiente iteración (basado en nodos y tiempo)
                bool prediction_success = false;
                if (last_iter_time > 0 && last_iter_nodes > 0 && current_iter_nodes > 0)
                {
                    double current_growth = (double)current_iter_nodes / (double)last_iter_nodes;

                    if (current_growth < 1.05)
                        current_growth = 1.05;

                    // Usar promedio ponderado de últimos growth factors
                    double growth = current_growth;
                    if (prev_growth_count > 0)
                    {
                        // Promedio ponderado: 60% actual, 40% histórico
                        double historical_avg = 0.0;
                        double weight_sum = 0.0;
                        for (int i = 0; i < prev_growth_count && i < 3; i++)
                        {
                            double weight = 1.0 / (i + 1.0); // Peso decreciente
                            historical_avg += prev_growth_factors[i] * weight;
                            weight_sum += weight;
                        }
                        if (weight_sum > 0.0)
                            historical_avg /= weight_sum;

                        growth = 0.6 * current_growth + 0.4 * historical_avg;
                    }

                    // Guardar growth actual en el historial (shift array)
                    for (int i = 2; i > 0; i--)
                        prev_growth_factors[i] = prev_growth_factors[i - 1];
                    prev_growth_factors[0] = current_growth;
                    if (prev_growth_count < 3)
                        prev_growth_count++;

                    long long predicted_next = (long long)(current_iter_time * growth);
                    
                    // Idea de 1/4: solo abortar si el tiempo restante no nos da
                    // ni para hacer el 25% de la siguiente iteración.
                    if (time_used + (predicted_next / 4) > current_soft)
                    {
                        ATOMIC_STORE(stop_search, 1);
                        break;
                    }
                    prediction_success = true;
                }

                // Fallback si no hay predicción fiable (asume growth empírico de ~2.0 y aplica la misma regla de 1/4)
                if (!prediction_success && time_used + (time_used / 2) > current_soft)
                {
                    ATOMIC_STORE(stop_search, 1);
                    break;
                }
            }
        }

        last_iter_time = current_iter_time;
        last_iter_nodes = current_iter_nodes;

        // Actualizar historial de best moves y scores
        for (int i = 2; i > 0; i--)
        {
            best_move_history[i] = best_move_history[i - 1];
            score_history[i] = score_history[i - 1];
        }
        best_move_history[0] = best_move;
        score_history[0] = score;
    }

    // UCI: en 'go infinite' y 'go ponder' bestmove solo puede salir tras 'stop' o
    // 'ponderhit', aunque el bucle de profundidades se haya agotado (I08, B13).
    esperar_stop_o_ponderhit();

    // Detener helpers
    ATOMIC_STORE(stop_search, 1);

    // Nodos totales de esta busqueda (los helpers se suman tras el join)
    long long nodos_busqueda = ATOMIC_LOAD(main_sd_ptr->nodes);
#ifdef USE_SMP
    if (num_helpers > 0)
    {
#ifdef _WIN32
        // WaitForMultipleObjects limit 64
        int count = num_helpers;
        int index = 0;
        while (count > 0)
        {
            int chunk = (count > 64) ? 64 : count;
            WaitForMultipleObjects(chunk, &helper_threads[index], TRUE, INFINITE);
            index += chunk;
            count -= chunk;
        }
        for (int i = 0; i < num_helpers; i++)
            CloseHandle(helper_threads[i]);
#else
        for (int i = 0; i < num_helpers; i++)
            pthread_join(helper_threads[i], NULL);
#endif
        for (int i = 0; i < num_helpers; i++)
            nodos_busqueda += ATOMIC_LOAD(helper_data[i].nodes);

        free(helper_threads);
    }
#endif // USE_SMP

    last_search_nodes = nodos_busqueda;

    // Movimiento de emergencia: si no tenemos movimiento, generar uno legal
    if (best_move == MOVE_NONE)
    {
        MoveList emergency_moves;
        generate_moves(pos, &emergency_moves, GEN_ALL);

        // Buscar el primer movimiento legal
        Position temp_pos;
        for (int i = 0; i < emergency_moves.count; i++)
        {
            copy_position(&temp_pos, pos);
            if (make_move(&temp_pos, emergency_moves.moves[i]))
            {
                best_move = emergency_moves.moves[i];
                printf("info string Warning: Emergency move used (time expired before depth 1)\n");
                break;
            }
        }
    }

    // Si vamos a ponderar pero no tenemos jugada de ponderación (PV de longitud 1),
    // intentamos obtener la respuesta esperada de la tabla hash.
    if (best_move != MOVE_NONE && ponder_move == MOVE_NONE && opt_ponder)
    {
        Position next_pos;
        copy_position(&next_pos, pos);
        // Aplicar best_move para ver la posición siguiente
        if (make_move(&next_pos, best_move))
        {
            int tt_score, tt_depth, tt_flag;
            Move tt_move = MOVE_NONE;
            // Consultar TT. Los valores de alpha/beta/depth no importan para recuperar el movimiento.
            read_tt(next_pos.hash, 0, 0, -INFINITO, INFINITO, &tt_score, &tt_move, &tt_depth, &tt_flag, NULL);
            // La TT puede devolver la jugada de otra posicion: validarla antes de usarla.
            if (tt_move != MOVE_NONE)
            {
                MoveList resp;
                generate_moves(&next_pos, &resp, GEN_ALL);
                for (int i = 0; i < resp.count; i++)
                {
                    if (resp.moves[i] == tt_move)
                    {
                        Position prueba;
                        copy_position(&prueba, &next_pos);
                        if (make_move(&prueba, tt_move))
                            ponder_move = tt_move;
                        break;
                    }
                }
            }
        }
    }

    // Guardar el score final de la búsqueda para comparar en la siguiente jugada
    previous_search_score = score_history[0];

    // Info final con los nodos REALES de la busqueda. Las de cada iteracion solo
    // salen al completarla; si 'go nodes N' o el reloj cortan a media iteracion, la
    // ultima info se quedaba muy por debajo de lo buscado (con go nodes 100000 decia
    // 42.650). El Juez y cualquier arbitro leen los nodos de esta ultima linea.
    if (salida_busqueda && best_move != MOVE_NONE && ultima_prof > 0)
    {
        // PV de una sola jugada: con la ponder detras, si best_move cierra una
        // repeticion, la siguiente quedaria 'despues de las tablas'
        char s_score[32];
        char s_move[8];
        score_a_texto(score_jugada, s_score, sizeof(s_score));
        move_to_str(best_move, s_move);
        printf("info depth %d seldepth %d score %s%s nodes %lld time %lld pv %s\n",
               ultima_prof, ultima_sel, s_score, cota_inferior ? " lowerbound" : "",
               nodos_busqueda, get_time_ms() - search_start_time, s_move);
        fflush(stdout);
    }


    ATOMIC_STORE(search_flag_ponder, 0);     // un 'ponderhit' tardio ya no debe hacer nada
    ATOMIC_STORE(search_flag_infinite, 0);

    if (!salida_busqueda)
    {
        ATOMIC_STORE_REL(is_searching, 0);
        return;
    }

    // Una sola escritura: a trozos, un 'readyok' del hilo UCI podia colarse en medio
    // y el GUI leia 'bestmove readyok' (B19)
    char linea[64];
    if (best_move != MOVE_NONE)
    {
        char s_best[8];
        move_to_str(best_move, s_best);
        if (opt_ponder && ponder_move != MOVE_NONE)
        {
            char s_ponder[8];
            move_to_str(ponder_move, s_ponder);
            snprintf(linea, sizeof(linea), "bestmove %s ponder %s\n", s_best, s_ponder);
        }
        else
        {
            snprintf(linea, sizeof(linea), "bestmove %s\n", s_best);
        }
    }
    else
    {
        snprintf(linea, sizeof(linea), "bestmove 0000\n"); // posición terminal: mate o ahogado
    }
    printf("%s", linea);
    fflush(stdout);

    ATOMIC_STORE_REL(is_searching, 0);
}

void engine_ponderhit()
{
    if (ATOMIC_LOAD(search_flag_ponder))
    {
        printf("info string Ponderhit recibido, cambiando a modo normal\n");
        ATOMIC_STORE(search_flag_ponder, 0);
        
        // IMPORTANTE: NO reseteamos search_start_time.
        // Si hemos estado ponderando durante 10s y nuestro soft_limit es 5s,
        // al no resetear, time_used será 10s > 5s y el motor moverá instantáneamente.
        
        Position dummy_pos; dummy_pos.side_to_move = search_side;
        SearchLimits limits = current_limits;
        limits.ponder = false;
        calculate_time_allocation(&dummy_pos, limits);
    }
}
