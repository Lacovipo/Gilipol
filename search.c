/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

 #include "common.h"
#include "tbprobe.h"

#ifdef USE_SMP
    #ifdef _WIN32
    #include <windows.h>
    #else
    #include <pthread.h>
    #include <unistd.h>
    #endif
#endif

#define MAX_PLY 128

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
static long long search_start_time = 0;
static ATOMIC_LLONG search_time_soft = -1;
static ATOMIC_LLONG search_time_hard = -1;
static SearchLimits current_limits;
static ATOMIC_BOOL search_flag_infinite = 0;
static ATOMIC_BOOL search_flag_ponder = 0;
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
        opt_syzygy_50move_rule ? pos->half_move_clock : 0,
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

    if (tt_table)
        free(tt_table);

    // Calcular clusters: cada cluster = 64 bytes
    long long bytes = (long long)mb * 1024LL * 1024LL;
    tt_cluster_count = bytes / sizeof(TTCluster);

    tt_table = (TTCluster*)malloc(tt_cluster_count * sizeof(TTCluster));

    if (!tt_table)
    {
        printf("info string Error: No se pudo asignar %d MB para la tabla hash\n", mb);
        tt_cluster_count = 0;
        return;
    }

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

// Ajustar puntuación de mate para guardar en TT
static inline int16_t score_to_tt(int score, int ply)
{
    if (score >= MATE_BOUND)
        return (int16_t)(score + ply);
    if (score <= -MATE_BOUND)
        return (int16_t)(score - ply);
    return (int16_t)score;
}

// Recuperar puntuación de mate desde TT
static inline int score_from_tt(int16_t score, int ply)
{
    // Convertir a int para comparación correcta con MATE_BOUND
    int s = (int)score;
    if (s >= MATE_BOUND)
        return s - ply;
    if (s <= -MATE_BOUND)
        return s + ply;
    return s;
}

static void print_score(int score)
{
    if (abs(score) > MATE_BOUND)
    {
        int moves = (MATE_SCORE - abs(score) + 1) / 2;
        printf("mate %d", score > 0 ? moves : -moves);
    }
    else
    {
        printf("cp %d", score);
    }
}

/*
 *
 * read_tt (retorna: 1 si hace cutoff, 0 si no; siempre intenta devolver best_move si encuentra la posición)
 * 
 */
int read_tt(uint64_t hash, int depth, int ply, int alpha, int beta, int* score, Move* best_move, int* tt_depth, int* tt_flag)
{
    if (!tt_table || tt_cluster_count == 0)
        return 0;

    uint64_t cluster_idx = hash % tt_cluster_count;
    TTCluster* cluster = &tt_table[cluster_idx];
    uint32_t hash_key = (uint32_t)(hash >> 32);

    // Inicialización
    *best_move = MOVE_NONE;
    if (tt_depth)
        *tt_depth = 0;
    if (tt_flag)
        *tt_flag = 0;

    // Buscar en las 4 ranuras
    TTEntry* found_entry = NULL;
    for (int i = 0; i < TT_CLUSTER_SIZE; i++)
    {
        TTEntry* entry = &cluster->entries[i];

        // Lectura de la clave (atómica con acquire en SMP, directa en single-thread)
        if (ATOMIC_LOAD_ACQ(entry->hash_key) == hash_key)
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

    // Solo hacer cutoff si la profundidad es suficiente
    int tt_val = score_from_tt(found_entry->score, ply);
    if (score)
        *score = tt_val;

    // Movimiento útil, pero no suficiente profundidad
    if (found_entry->depth < depth)
        return 0;

    // Verificar si podemos hacer cutoff según el tipo de bound
    if (found_entry->flag == TT_EXACT)
        return 1;
    else if (found_entry->flag == TT_ALPHA && tt_val <= alpha)
    {
        if (score)
            *score = alpha;
        return 1;
    }
    else if (found_entry->flag == TT_BETA && tt_val >= beta)
    {
        if (score)
            *score = beta;
        return 1;
    }

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

    uint64_t cluster_idx = hash % tt_cluster_count;
    TTCluster* cluster = &tt_table[cluster_idx];
    uint32_t hash_key = (uint32_t)(hash >> 32);

    TTEntry* replace_entry = NULL;
    int replace_idx = -1;
    int worst_score = -1000000;

    // 
    // Buscar si ya existe esta posición o una ranura vacía
    // 
    for (int i = 0; i < TT_CLUSTER_SIZE; i++)
    {
        TTEntry* entry = &cluster->entries[i];

        // Ranura vacía (nunca usada)
        if (ATOMIC_LOAD(entry->hash_key) == 0)
        {
            replace_entry = entry;
            replace_idx = i;
            break;
        }

        // Misma posición - siempre reemplazar si es más profunda o misma generación
        if (ATOMIC_LOAD(entry->hash_key) == hash_key)
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
        // Escribir clave al final (con release en SMP para publicar datos de forma segura)
        ATOMIC_STORE_REL(replace_entry->hash_key, hash_key);
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

// Función pública para limpiar todas las heurísticas de búsqueda
void clear_search_heuristics(void)
{
    clear_continuation_history();
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

/*
 *
 * quiescence
 * 
 */
int quiescence(Position* pos, int alpha, int beta, int ply, SearchData* sd)
{
    //
    // Salida inmediata si se ha detenido la búsqueda
    //
    if (ATOMIC_LOAD(stop_search))
        return 0;

    //
    // Profundidad selectiva y nodos
    //
    if (ply > sd->sel_depth)
        sd->sel_depth = ply;
    ATOMIC_FETCH_ADD(sd->nodes, 1);

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
    if (read_tt(pos->hash, 0, ply, alpha, beta, &tt_score, &tt_move, &tt_depth, &tt_flag))
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
    int stand_pat = -INFINITO;
    if (!in_check)
    {
        stand_pat = nnue_evaluate(pos);

        // Si tenemos un lower bound de la TT (TT_BETA) que es mayor que la evaluación estática,
        // lo usamos para mejorar la precisión de stand_pat.
        if (bHayScoreTT && (tt_flag == TT_BETA) && tt_score > stand_pat)
            stand_pat = tt_score;

        if (stand_pat >= beta)
            return beta;
        if (alpha < stand_pat)
            alpha = stand_pat;
    }

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
        if (stand_pat + max_val + QS_PODA_DELTA < alpha)
            return(alpha);
    }

    //
    // Bucle principal: sólo capturas
    //
    MoveList moves;
    generate_captures(pos, &moves);
    sort_moves(&moves, tt_move, 0, MOVE_NONE, pos, sd);
    
    int legal_moves = 0;
    Position child;
    for (int i = 0; i < moves.count; i++)
    {
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
        
        legal_moves++;
        int score = -quiescence(&child, -beta, -alpha, ply + 1, sd);
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

// Detectar material insuficiente para dar mate
static int is_insufficient_material(const Position* pos)
{
    // Si hay peones, torres o damas, hay material suficiente
    if (pos->pieces[WHITE][PAWN] | pos->pieces[BLACK][PAWN] | pos->pieces[WHITE][ROOK] | pos->pieces[BLACK][ROOK] | pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN])
        return 0;

    // Contar piezas menores
    int w_minors = count_bits(pos->pieces[WHITE][KNIGHT] | pos->pieces[WHITE][BISHOP]);
    int b_minors = count_bits(pos->pieces[BLACK][KNIGHT] | pos->pieces[BLACK][BISHOP]);

    // Tablas si ambos bandos tienen 1 o menos piezas menores (K, K+N, K+B vs K, K+N, K+B)
    if (w_minors <= 1 && b_minors <= 1)
        return 1;

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
    if (ply > 0 && (pos->half_move_clock >= 100 || is_insufficient_material(pos) || is_repetition(pos)))
        return 0;

    //
    // Syzygy Probing (WDL)
    //
    if (ply > 0
        && !ATOMIC_LOAD(stop_search)
        && excluded_move == MOVE_NONE
        && depth >= opt_syzygy_probe_depth
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

    pv[0] = MOVE_NONE;

    //
    // Salida por profundidad agotada
    //
    if (depth <= 0)
        return quiescence(pos, alpha, beta, ply, sd);

	//
	// Consultar tabla hash
	//
    int tt_score;
    Move tt_move = MOVE_NONE;
    int tt_depth = 0;
    int tt_flag = 0;
    if (excluded_move == MOVE_NONE)
    {
        if (ply == 0) // en la raíz, consultamos pero no cortamos
            read_tt(pos->hash, depth, ply, alpha, beta, &tt_score, &tt_move, &tt_depth, &tt_flag);
        else if (read_tt(pos->hash, depth, ply, alpha, beta, &tt_score, &tt_move, &tt_depth, &tt_flag))
            // OJO: para podar, añadir la condición de que el contador de 50 no esté muy cerca de las tablas (< 90 o algo así)
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
    int static_eval = -INFINITO;
    if (!in_check)
    {
        if (bHayScoreTT)
        {
            if (tt_flag == TT_EXACT
                || (tt_flag == TT_ALPHA && tt_score < alpha)
                || (tt_flag == TT_BETA && tt_score > beta))
            {
                static_eval = tt_score;
            }
            else
                static_eval = nnue_evaluate(pos);
        }
        else
            static_eval = nnue_evaluate(pos);
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

                // Si hace 4 también estábamos en jaque, O si la eval actual es mejor que hace 4
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
	// IID
	//
    if (!bHayMovTT
        && depth >= BUS_IID_DEPTH_MIN
        && static_eval > alpha
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
            depth -= BUS_IIR_PVRED;
        else if (bIsCutNode)
            depth -= BUS_IIR_CUTRED;
        else
            depth -= BUS_IIR_ALLRED;

        if (depth <= 0)
            return quiescence(pos, alpha, beta, ply, sd);
    }

    const bool bPodable = !bEsPV && !in_check && ply > 0 && excluded_move == MOVE_NONE && MargenesNoMate(alpha, beta);

    //
    // Full razoring (por llamarlo de alguna forma)
    // Idea vista en SF y algún otro (Halogen, Reckless...)
    //
    #if (0)
    if (bPodable
        && static_eval < alpha - BUS_FULLRAZ_BASE - BUS_FULLRAZ_MULT * depth * depth)
    {
        return quiescence(pos, alpha, beta, ply, sd);
    }
    #endif

	//
	// Razoring contra alfa (si estamos tan lejos de alfa que únicamente podemos recuperar con capturas, intentamos llegar a alfa con quiesce)
	//
    if (bPodable
        && depth <= BUS_RAZOR_DEPTH
        && static_eval < alpha - razor_margin[depth < 6 ? depth : 5])
    {
        const int val = quiescence(pos, alpha, beta, ply, sd);
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
            && static_eval > beta + BUS_PODAJC_MARGIN * (depth - bMejorando)
            && static_eval > BUS_PODAJC_CERO)
        {
            return((static_eval + beta) / 2); // OJO: devolver eval - margen (como Alexandria 9)
        }

        //
        // Null move
        //
        if (prev_move != MOVE_NONE
            && (BUS_NM_CUTYALL || bIsCutNode)) // El parámetro a 1 acepta los nodos ALL
        {
            Position child;
            copy_position(&child, pos);
            child.prev = (Position*)pos;
            make_null_move(&child);
            
            int R = BUS_NM_R + depth / 3 + BUS_NM_IMP * bMejorando;
            if (static_eval - beta >= BUS_NM_DIV)
                R += min((static_eval - beta) / BUS_NM_DIV, 3);
            Move null_pv[MAX_PLY]; // Buffer para PV (no se usa aquí)

            // Búsqueda con ventana nula
            int score = -negamax(&child, -beta, -beta + 1, depth - R, ply + 1, MOVE_NONE, MOVE_NONE, null_pv, false, sd);

            if (score >= beta)
            {
                if (score >= MATE_BOUND)
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
        const int pbeta = beta + PROBCUT_MARGIN;

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
                int pc_score = -quiescence(&pc_child, -pbeta, -pbeta + 1, ply + 1, sd);

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
    sort_moves(&moves, tt_move, ply, prev_move, pos, sd);

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
        if (moves.moves[i] == excluded_move)
            continue;

        const bool bEsCaptura = GET_MOVE_CAPTURE(moves.moves[i]);
        const bool bEsPromo   = GET_MOVE_PROMOTED(moves.moves[i]);

        //
        // Podas estáticas contra el movimiento
        //
        if (legal_moves > 0 && !bEsPromo && !in_check && !bEsPV && abs(static_eval) < MATE_BOUND && bTieneAlgunaPieza && !gives_check(pos, moves.moves[i]))
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

        copy_position(&child, pos);
        child.prev = (Position*)pos; // Enlazar para detección de repeticiones
        
        // Si el movimiento es ilegal, lo saltamos
        if (!make_move(&child, moves.moves[i])) ////////////////////////////////////////////////////////////////////////////////
            continue;
        
        legal_moves++;

        // Registrar movimientos probados (para history gravity)
        if (!bEsCaptura && !bEsPromo && quiets_tried_count < 64)
            quiets_tried[quiets_tried_count++] = moves.moves[i];
        else if (bEsCaptura && captures_tried_count < 32)
            captures_tried[captures_tried_count++] = moves.moves[i];

        if (sd->thread_id == 0 && ply == 0 && ATOMIC_LOAD(search_flag_infinite) && depth > 15)
        {
            printf("info currmove ");
            print_move(moves.moves[i]);
            printf(" currmovenumber %d\n", legal_moves);
            fflush(stdout);
        }
        
        // Detectar si el movimiento da jaque (jaque después del movimiento)
        int gives_check = is_square_attacked(&child, get_lsb_index(child.pieces[child.side_to_move][KING]), child.side_to_move ^ 1);

        //
        // LMP (Late Move Pruning)
        // OJO: ahora tengo gives_check antes de mover, así que creo que podría pasar LMP a antes de mover
        if (bPodable
            && !gives_check
            && depth <= LMP_DEPTH
            && legal_moves > (LMP_BASE + depth * depth) / (bMejorando ? 1 : 2)
            && !bEsCaptura
            && !bEsPromo
            && bTieneAlgunaPieza)
        {
            continue;
        }

        // Extensión de jaque (acumulando la singular, si la hay)
        int extension = gives_check;
        if (moves.moves[i] == tt_move)
            extension += singular_extension;

        //
        // Control de extensiones
        //
        int extensions_used = (ply + depth) - sd->search_root_depth;
        int max_extensions = (int)(sd->search_root_depth * EXT_LIMIT_MULTIPLIER);

        if (extensions_used >= max_extensions)
            extension = 0;                                  // Si ya superamos el límite, no permitir más extensiones
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
                if (gives_check)
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
            while (child_pv[j] != MOVE_NONE && (j + 1) < MAX_PLY)
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
            if (excluded_move == MOVE_NONE)
                write_tt(pos->hash, depth, ply, beta, TT_BETA, moves.moves[i], static_eval);

            // Killer Heuristic: Guardar movimientos quietos que causan un corte
            if (!bEsCaptura && ply < MAX_PLY)
            {
                sd->killer_moves[ply][1] = sd->killer_moves[ply][0];
                sd->killer_moves[ply][0] = moves.moves[i];

                // History Heuristic: Premiar jugadas que causan corte
                int bonus = depth * depth;
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

    // Guardar en TT
    if (excluded_move == MOVE_NONE)
    {
        tt_flag = (best_score <= original_alpha) ? TT_ALPHA : (best_score >= beta ? TT_BETA : TT_EXACT);
        write_tt(pos->hash, depth, ply, best_score, tt_flag, best_move_this_node, static_eval);
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
    else if (limits.wtime > 0 || limits.btime > 0) // Modo partida
    {
        int time_left = (pos->side_to_move == WHITE) ? limits.wtime : limits.btime;
        int inc = (pos->side_to_move == WHITE) ? limits.winc : limits.binc;
        int movestogo = (limits.movestogo > 0) ? limits.movestogo : MOVESTOGO;

        // Margen de seguridad por lag (50ms)
        time_left = max(0, time_left - 50);

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

        ATOMIC_STORE(search_time_soft, soft);
        ATOMIC_STORE(search_time_hard, hard);
    }
}

// Variable estática para saber el bando (necesaria para ponderhit)
static int search_side;

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
    memset(sd->killer_moves, 0, sizeof(sd->killer_moves));
    memset(sd->history_moves, 0, sizeof(sd->history_moves));
    memset(sd->counter_moves, 0, sizeof(sd->counter_moves));
    memset(sd->capture_history, 0, sizeof(sd->capture_history));

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
    ATOMIC_STORE(stop_search, 0);
    current_limits = limits; // Guardar copia global
    ATOMIC_STORE(search_flag_infinite, limits.infinite ? 1 : 0);
    ATOMIC_STORE(search_flag_ponder, limits.ponder ? 1 : 0);
    search_side = pos->side_to_move;

    // Incrementar generación de TT (aging automático)
    tt_new_search();

    // --- Syzygy Root Probe (Solución a Ceguera de WDL / Shuffling) ---
    int pieces = count_bits(pos->all);
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

                printf("info depth 1 score ");
                print_score(score);
                printf(" time 0 nodes 0 pv ");
                print_move(tb_move);
                printf("\n");
                
                printf("bestmove ");
                print_move(tb_move);
                printf("\n");
                fflush(stdout);

                // Retornar inmediatamente evita que inicie toda la búsqueda Iterativa 
                // y los hilos en Lazy SMP de forma innecesaria.
                return;
            }
        }
    }

    // Configurar datos del hilo principal (ID 0)
    SearchData main_sd;
    main_sd.thread_id = 0;
    ATOMIC_STORE(main_sd.nodes, 0);
    main_sd.sel_depth = 0;
    memset(main_sd.killer_moves, 0, sizeof(main_sd.killer_moves));
    memset(main_sd.history_moves, 0, sizeof(main_sd.history_moves));
    memset(main_sd.counter_moves, 0, sizeof(main_sd.counter_moves));
    memset(main_sd.capture_history, 0, sizeof(main_sd.capture_history));
    copy_position(&main_sd.root_pos, pos);

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
        helper_data = (SearchData*)malloc(num_helpers * sizeof(SearchData));
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
            free(helper_data);
            num_helpers = 0;
        }

        for (int i = 0; i < num_helpers; i++)
        {
            helper_data[i].thread_id = i + 1;
            copy_position(&helper_data[i].root_pos, pos);

#ifdef _WIN32
            helper_threads[i] = CreateThread(NULL, 0, search_worker, &helper_data[i], 0, NULL);
            if (helper_threads[i] == NULL)
            {
                printf("info string Error: Failed to create thread %d\n", i+1);
                // Stop creating threads, but keep the ones created
                num_helpers = i;
                break;
            }
#else
            if (pthread_create(&helper_threads[i], NULL, search_worker, &helper_data[i]) != 0)
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
    search_start_time = get_time_ms();
    ATOMIC_STORE(search_time_soft, -1);
    ATOMIC_STORE(search_time_hard, -1);
    
    if (!limits.ponder && !limits.infinite)
        calculate_time_allocation(pos, limits);

    int depth = limits.depth;
    if (depth == 0 || limits.infinite || limits.ponder)
        depth = MAX_PLY - 1; // Si es infinite, ponemos un límite alto (respetando buffer PV)

    // Si hay tiempo definido, limitamos la profundidad máxima para evitar bucles infinitos accidentales
    if (ATOMIC_LOAD(search_time_hard) > 0 && limits.depth == 0)
        depth = MAX_PLY - 1;

    Move best_move = MOVE_NONE;
    Move ponder_move = MOVE_NONE;
    int prev_score = 0;
    long long last_iter_time = 0;
    long long last_iter_nodes = 0;

    //
    // Iterative Deepening con Aspiración
    //
    for (int current_depth = 1; current_depth <= depth; current_depth++)
    {
        long long iter_start_time = get_time_ms();
        long long iter_start_nodes = ATOMIC_LOAD(main_sd.nodes);

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
            main_sd.search_root_depth = search_depth;

            score = negamax(pos, alpha, beta, search_depth, 0, MOVE_NONE, MOVE_NONE, root_pv, false, &main_sd);

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
                if (ATOMIC_LOAD(main_sd.nodes) >= 100000 || limits.infinite)
                {
                    long long current_time = get_time_ms();
                    long long total_nodes_fl = ATOMIC_LOAD(main_sd.nodes);
#ifdef USE_SMP
                    if (helper_data)
                    {
                        for (int i = 0; i < num_helpers; i++)
                            total_nodes_fl += ATOMIC_LOAD(helper_data[i].nodes);
                    }
#endif
                    printf("info depth %d score ", current_depth);
                    print_score(score);
                    printf(" upperbound nodes %lld time %lld pv ", total_nodes_fl, current_time - search_start_time);
                    for (int j = 0; root_pv[j] != MOVE_NONE; j++)
                    {
                        print_move(root_pv[j]);
                        printf(" ");
                    }
                    printf("\n");
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
                    best_move = root_pv[0];

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
                if (ATOMIC_LOAD(main_sd.nodes) >= 100000 || limits.infinite)
                {
                    long long current_time = get_time_ms();
                    long long total_nodes_fh = ATOMIC_LOAD(main_sd.nodes);
#ifdef USE_SMP
                    if (helper_data) {
                        for (int i = 0; i < num_helpers; i++)
                            total_nodes_fh += ATOMIC_LOAD(helper_data[i].nodes);
                    }
#endif
                    printf("info depth %d score ", current_depth);
                    print_score(score);
                    printf(" lowerbound nodes %lld time %lld pv ", total_nodes_fh, current_time - search_start_time);
                    for (int j = 0; root_pv[j] != MOVE_NONE; j++)
                    {
                        print_move(root_pv[j]);
                        printf(" ");
                    }
                    printf("\n");
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
        if (!limits.infinite && !limits.ponder && current_depth >= 10 && score >= MATE_BOUND && score > previous_search_score)
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

            if (ATOMIC_LOAD(main_sd.nodes) >= 100000 || limits.infinite)
            {
                long long current_time = get_time_ms();
                long long elapsed = current_time - search_start_time;

                // Calcular nodos totales (Main + Helpers)
                long long total_nodes = ATOMIC_LOAD(main_sd.nodes);
#ifdef USE_SMP
                if (helper_data)
                {
                    for (int i = 0; i < num_helpers; i++)
                        total_nodes += ATOMIC_LOAD(helper_data[i].nodes);
                }
#endif

                printf("info depth %d seldepth %d score ", current_depth, main_sd.sel_depth);
                print_score(score);
                printf(" nodes %lld time %lld pv ", total_nodes, elapsed);

                // Imprimir línea PV completa
                for (int j = 0; root_pv[j] != MOVE_NONE; j++)
                {
                    print_move(root_pv[j]);
                    printf(" ");
                }
                printf("\n");
                fflush(stdout);
            }
        }

        long long iter_end_time = get_time_ms();
        long long current_iter_time = iter_end_time - iter_start_time;
        
        // Prevenir tiempos nulos debido a la baja resolución de GetTickCount64 en Windows (~15.6ms).
        // Un tiempo de 0 arruina la predicción de la siguiente iteración y dispara el aborto prematuro.
        if (current_iter_time <= 0)
            current_iter_time = 1;
            
        long long current_iter_nodes = ATOMIC_LOAD(main_sd.nodes) - iter_start_nodes;

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
                    // Aumentar tiempo si hay dudas (<50%), reducir solo si es muy obvia (>80%)
                    double node_factor = 1.5 - best_fraction;
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

                // Reducciones de tiempo (solo si no es movetime fijo)
                if (limits.movetime == 0)
                {
                    if (fail_lows + fail_highs >= MAX_ASPIRATION_FAILS)
                        stability_factor *= 0.8;

                    // Si es mate inminente o final resuelto por tablebases, no gastar tiempo
                    if (abs(score) >= TB_MAX - MAX_PLY)
                        stability_factor *= 0.5;

                    // Ajuste por evaluación
                    if (current_depth >= 8 && abs(score) < MATE_BOUND)
                    {
                        double score_factor = 1.0;
                        // Solo reducir tiempo si la ventaja es clara y sólida
                        if (score > 200)
                            score_factor = 1.0 - (double)(score - 200) / 1000.0;
                        // Si estamos perdiendo o en apuros (< -0.50), usar más tiempo para defender
                        else if (score < -50)
                            score_factor = 1.0 - (double)(score + 50) / 400.0;
                            
                        // Acotar a márgenes de seguridad para evitar locuras con scores extremos
                        score_factor = clamp_double(score_factor, 0.70, 1.50);
                        
                        stability_factor *= score_factor;
                    }

                    // Estabilidad continua: cuanto más tiempo lleva el best move sin cambiar, más reducimos.
                    if (current_depth >= 10 && stability_count >= 3 && best_move != MOVE_NONE)
                    {
                        int capped = stability_count < 10 ? stability_count : 10;
                        stability_factor *= 1.0 - 0.015 * capped;
                    }
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
                    if (time_used + predicted_next > current_soft)
                    {
                        ATOMIC_STORE(stop_search, 1);
                        break;
                    }
                    prediction_success = true;
                }

                // Predicción de Branching Factor (fallback simple)
                // Solo usamos el fallback si no tenemos datos fiables para la predicción
                if (!prediction_success && 3 * time_used / 2 > current_soft)
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

    // Detener helpers
    ATOMIC_STORE(stop_search, 1);
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
        free(helper_data);
        free(helper_threads);
    }
#endif // USE_SMP

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
            read_tt(next_pos.hash, 0, 0, -INFINITO, INFINITO, &tt_score, &tt_move, &tt_depth, &tt_flag);
            if (tt_move != MOVE_NONE)
                ponder_move = tt_move;
        }
    }

    // Guardar el score final de la búsqueda para comparar en la siguiente jugada
    previous_search_score = score_history[0];

    printf("bestmove ");
    if (best_move != MOVE_NONE)
    {
        print_move(best_move);
        if (opt_ponder && ponder_move != MOVE_NONE)
        {
            printf(" ponder ");
            print_move(ponder_move);
        }
        printf("\n");
    }
    else
    {
        printf("0000\n"); // No move (mate o ahogado - posición terminal)
    }
    fflush(stdout);
}

void engine_ponderhit()
{
    if (ATOMIC_LOAD(search_flag_ponder))
    {
        printf("Info: Ponderhit recibido, cambiando a modo normal.\n");
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
