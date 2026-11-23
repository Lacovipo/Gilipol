/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <immintrin.h>

// CPU detectada al arrancar (detectar_cpu, en nnue.c). El ejecutable de distribucion
// se compila para x86-64 generico y elige en tiempo de ejecucion las variantes AVX-512,
// AVX2 o generica de la red, PEXT o magic bitboards y POPCNT o no.
enum { CPU_GENERICA = 0, CPU_AVX2 = 1, CPU_AVX512 = 2 };
extern int cpu_nivel;       // CPU_*: variante de la red y del acumulador
extern int cpu_pext;        // 1 = PEXT rapido (BMI2, y no AMD anterior a Zen 3)
extern int cpu_popcnt;      // 1 = instruccion POPCNT
void        detectar_cpu(void);
const char* nombre_cpu(void);

// Redes incluidas en el ejecutable. EvalFile/TCFile con este nombre (o vacio) usan
// la copia interna; con cualquier otro, la cargan de disco.
#define RED_POR_DEFECTO     "net5.bin"
#define TC_POR_DEFECTO      "tcnet5.bin"

#define MATE_SCORE 32000
#define MATE_BOUND 31000
#define INFINITO 32001
#define TB_MAX 30000
#define MAX_PLY 128

// Zona de puntuaciones de tablebase.
// probe_syzygy devuelve +-(TB_MAX - ply), con ply < MAX_PLY, asi que todo score
// de TB cae en [TB_BOUND, TB_MAX]. Igual que los mates, depende del ply y por
// tanto hay que ajustarlo al guardarlo/leerlo en la tabla hash.
#define TB_BOUND (TB_MAX - MAX_PLY)

// Corte por hash en nodos PV.
//
//  1 = se corta (comportamiento historico de Gilipol)
//  0 = no se corta (lo que hace Stockfish: mejor PV y raiz mas estable)
//
// Poner a 0 cuesta un 15 % de nodos y un 22 % de tiempo medidos en bench. A cambio
// deja de truncar la PV. NO esta probado con SPRT: es el candidato numero uno a test.
#define TT_CORTE_EN_PV              1

// Cota de la evaluacion estatica: nunca debe invadir la zona de TB ni la de mate,
// o score_to_tt/score_from_tt la confundirian con una distancia a tablebase.
#define EVAL_MAX (TB_BOUND - 1)

// Centinela de "esta entrada de la TT no trae eval estatica".
//
// Antes se usaba -INFINITO, que ademas es el valor que negamax deja en raw_eval
// cuando el nodo esta en jaque: o sea que "no hay dato" y "habia jaque" acababan
// siendo el mismo numero por casualidad. Funcionaba, pero se rompia solo con que
// alguien cambiase INFINITO o el tipo del campo. INT16_MIN esta fuera del rango de
// cualquier evaluacion legitima (acotada a +-EVAL_MAX) y no colisiona con nada.
#define EVAL_NONE (-32768)

// ============================================================================
// COMPILACIÓN CONDICIONAL: SMP vs SINGLE-THREAD
// ============================================================================
// Por defecto: single-thread (sin overhead de atómicos)
// Para compilar con SMP: gcc -DUSE_SMP ...
// ============================================================================

#ifdef USE_SMP
    #include <stdatomic.h>

    // Tipos atómicos
    #define ATOMIC_INT          atomic_int
    #define ATOMIC_BOOL         atomic_bool
    #define ATOMIC_LLONG        atomic_llong
    #define ATOMIC_UINT32       atomic_uint_least32_t

    // Operaciones atómicas
    #define ATOMIC_LOAD(x)              atomic_load_explicit(&(x), memory_order_relaxed)
    #define ATOMIC_LOAD_ACQ(x)          atomic_load_explicit(&(x), memory_order_acquire)
    #define ATOMIC_STORE(x, v)          atomic_store_explicit(&(x), (v), memory_order_relaxed)
    #define ATOMIC_STORE_REL(x, v)      atomic_store_explicit(&(x), (v), memory_order_release)
    #define ATOMIC_FETCH_ADD(x, v)      atomic_fetch_add_explicit(&(x), (v), memory_order_relaxed)
#else
    // Single-thread: tipos normales, sin overhead
    #define ATOMIC_INT          int
    #define ATOMIC_BOOL         int
    #define ATOMIC_LLONG        long long
    #define ATOMIC_UINT32       uint32_t

    // Operaciones directas (sin barreras de memoria)
    #define ATOMIC_LOAD(x)              (x)
    #define ATOMIC_LOAD_ACQ(x)          (x)
    #define ATOMIC_STORE(x, v)          ((x) = (v))
    #define ATOMIC_STORE_REL(x, v)      ((x) = (v))
    #define ATOMIC_FETCH_ADD(x, v)      ((x) += (v))  // En single-thread no necesitamos el valor anterior
#endif

// Banderas de control compartidas entre el hilo UCI y el de busqueda (stop, ponder,
// limites de tiempo...). Tambien en monohilo hay dos hilos, asi que son atomicas en
// las DOS compilaciones: con int simple era una carrera de datos (UB en C11) que solo
// funcionaba porque cada bucle de sondeo llamaba a una funcion opaca (B20). Se leen
// y escriben con las mismas macros ATOMIC_*: en x86 la carga es un MOV normal.
#include <stdatomic.h>
#define SHARED_INT                  atomic_int
#define SHARED_LLONG                atomic_llong

#define MAX_MOVES_BUFFER 2048

typedef uint64_t Bitboard;

enum Color { WHITE, BLACK, COLOR_NB };

enum PieceType { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, PIECE_TYPE_NB, NO_PIECE_TYPE = -1 };

enum Square
{
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE
};

enum CastlingRights
{
    NO_CASTLING = 0,
    WHITE_OO    = 1,
    WHITE_OOO   = 2,
    BLACK_OO    = 4,
    BLACK_OOO   = 8
};

// NNUE
#define INPUT_SIZE 786
#define HIDDEN1 512
#define HIDDEN2 128
#define HIDDEN3 64
#define HIDDEN4 32
#define OUTPUT_SIZE 1

// Acumulador NNUE
typedef struct
{
    float values[HIDDEN1];
} Accumulator;

// Estructura principal que representa el estado del tablero
typedef struct Position Position; // Forward declaration para el puntero prev

struct Position
{
    Bitboard pieces[COLOR_NB][PIECE_TYPE_NB]; // Tableros de piezas por color y tipo
    Bitboard occupied[COLOR_NB];              // Ocupación por color (redundante pero útil para velocidad)
    Bitboard all;                             // Ocupación total
    
    uint64_t hash;          // Zobrist Hash de la posición
    uint64_t pawn_hash;     // Zobrist sólo de los peones (para correction history)
    int side_to_move;       // WHITE o BLACK
    int en_passant_sq;      // Casilla SQ_... o SQ_NONE
    int castling_rights;    // Máscara de bits (OR de CastlingRights)
    int half_move_clock;    // Para la regla de los 50 movimientos
    int full_move_number;   // Número de jugada
    int eval;               // Evaluación estática de la posición
    
    Position* prev;         // Puntero a la posición anterior (para detectar repeticiones en búsqueda)
    Accumulator accumulator; // Acumulador NNUE incremental
};

// Estructura para pasar los límites de tiempo y profundidad a la búsqueda
typedef struct
{
    int wtime;      // Tiempo blancas (ms)
    int btime;      // Tiempo negras (ms)
    int winc;       // Incremento blancas (ms)
    int binc;       // Incremento negras (ms)
    int movestogo;  // Movimientos hasta el control de tiempo
    int depth;      // Profundidad máxima
    long long nodes; // Nodos máximos (0 = sin límite)
    int movetime;   // Tiempo fijo por movimiento
    bool infinite;  // Analizar hasta recibir comando 'stop'
    bool ponder;    // Modo Ponder (pensar en tiempo del oponente)
    bool reloj;     // Llego wtime o btime (aunque valgan 0 o menos): hay que gestionar el tiempo (B12)
} SearchLimits;

// --- Codificación de Movimientos (Entero de 32 bits) ---
// 0-5:   Casilla origen (6 bits)
// 6-11:  Casilla destino (6 bits)
// 12-14: Pieza de promoción (3 bits: 0=None, 1=N, 2=B, 3=R, 4=Q)
// 15:    Bandera de captura
// 16:    Bandera de doble paso de peón
// 17:    Bandera de en passant
// 18:    Bandera de enroque

typedef int Move;

#define MOVE_NONE 0

// Macros para codificar y decodificar
#define ENCODE_MOVE(source, target, promo, capture, double, enpas, castle) \
    ((source) | ((target) << 6) | ((promo) << 12) | ((capture) << 15) | \
    ((double) << 16) | ((enpas) << 17) | ((castle) << 18))

#define GET_MOVE_SOURCE(m) ((m) & 0x3f)
#define GET_MOVE_TARGET(m) (((m) >> 6) & 0x3f)
#define GET_MOVE_PROMOTED(m) (((m) >> 12) & 0x7)
#define GET_MOVE_CAPTURE(m) ((m) & (1 << 15))
#define GET_MOVE_DOUBLE(m) ((m) & (1 << 16))
#define GET_MOVE_ENPASSANT(m) ((m) & (1 << 17))
#define GET_MOVE_CASTLING(m) ((m) & (1 << 18))

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
static inline int clamp_int(int a, int b, int c) {if (a < b) return b; if (a > c) return c; return a;}
static inline double clamp_double(double a, double b, double c) {if (a < b) return b; if (a > c) return c; return a;}

typedef struct
{
    Move moves[256];
    int count;
} MoveList;

// --- Tipos de Generación de Movimientos ---
enum { GEN_ALL, GEN_CAPTURES };

// --- Transposition Table (TT) ---

enum { TT_EXACT, TT_ALPHA, TT_BETA };

// Cada entrada de la TT contiene información compacta
typedef struct
{
    ATOMIC_UINT32 hash_key; // 32 bits superiores del hash (verificación) - Atomic en SMP
    Move best_move;         // Mejor movimiento encontrado
    int16_t score;          // Puntuación (ajustada para TT)
    int16_t eval;           // Evaluación estática (para mejores decisiones de reemplazo)
    uint8_t depth;          // Profundidad de búsqueda
    uint8_t generation;     // Generación (para aging)
    uint8_t flag;           // TT_EXACT, TT_ALPHA o TT_BETA
    uint8_t padding;        // Alineación
} TTEntry;

// Cluster de 4 entradas
#define TT_CLUSTER_SIZE 4
typedef struct
{
    TTEntry entries[TT_CLUSTER_SIZE];
} TTCluster;

// Prototipos TT
void init_tt();
void clear_tt();
void resize_tt(int mb);
void tt_new_search();  // Llamar al inicio de cada búsqueda (incrementa generación)
void tt_prefetch(uint64_t hash); // Traer la linea de cache de la TT por adelantado

// --- Globales de Control ---
#define MAX_GAME_HISTORY 16384  // un 'position ... moves' de 64 KB cabe entero (I35)

extern SHARED_INT stop_search;      // parar la busqueda: GUI, limite de nodos o de tiempo
extern SHARED_INT stop_gui;         // parada pedida por el GUI: la unica que acaba infinite/ponder (B13)
extern long long last_search_nodes;  // Nodos de la ultima busqueda (main + helpers). Lo usa 'bench'.
extern SHARED_INT is_searching;
extern uint64_t game_history[MAX_GAME_HISTORY];
extern int game_history_count;
extern int opt_threads;
extern int opt_ponder;
extern int opt_multi_pv;
extern char opt_syzygy_path[1024];
extern int opt_syzygy_probe_limit;
extern int opt_syzygy_probe_depth;
extern int opt_syzygy_50move_rule;
extern char opt_nnue_path[1024];
extern int opt_qs_poda_delta;
extern int opt_qs_fut_margin;
//extern int opt_qs_lmp;
extern int opt_qs_see;
extern int opt_qs_improve;
extern int opt_qs_stable;
extern int opt_qs_worsen;
extern int opt_ext_limit_multiplier;
extern int opt_lmp_base;
extern int opt_lmp_depth;
extern int opt_lmr_goodmove_margin;
extern int opt_bus_nm_cutyall;
extern int opt_bus_nm_div;
extern int opt_bus_nm_imp;
extern int opt_bus_nm_r;
extern int opt_bus_nm_verif;
extern int opt_bus_nm_ifworsen;
extern int opt_bus_nm_willimprove;
extern int opt_bus_nm_isstable;
extern int opt_bus_nm_willworsen;
extern int opt_bus_iid_depthmin;
extern int opt_bus_iid_improve;
extern int opt_bus_iid_worsen;
extern int opt_bus_podajc_depth;
extern int opt_bus_podajc_margin;
extern int opt_bus_podajc_cero;
extern int opt_bus_podajc_ifworsen;
extern int opt_bus_iir_depth;
extern int opt_bus_iir_pvred;
extern int opt_bus_iir_cutred;
extern int opt_bus_iir_allred;
extern int opt_bus_iir_pv_stable;
extern int opt_bus_iir_cut_improve;
extern int opt_bus_iir_all_worsen;
extern int opt_bus_razor_depth;
extern int opt_bus_razor_margin1;
extern int opt_bus_razor_margin2;
extern int opt_bus_razor_margin3;
extern int opt_bus_razor_margin4;
extern int opt_bus_razor_margin5;
extern int opt_bus_fullraz_base;
extern int opt_bus_fullraz_mult;
extern int opt_bus_fullraz_improve;
extern int opt_bus_fullraz_worsen;
extern int opt_lmr_base_depth;
extern int opt_lmr_base_move;
extern int opt_poda_hash_beta_prof_dif;
extern int opt_poda_hash_beta_base;
extern int opt_poda_hash_beta_mult_dif;
extern int opt_poda_hash_beta_mult_prof;
extern int opt_poda_hash_alfa_prof_dif;
extern int opt_poda_hash_alfa_base;
extern int opt_poda_hash_alfa_mult_dif;
extern int opt_poda_hash_alfa_mult_prof;

extern int opt_bus_fut_depth;
extern int opt_bus_fut_base;
extern int opt_bus_fut_margin;
extern int opt_bus_hp_depth;
extern int opt_bus_hp_margin;
extern int opt_bus_chp_depth;
extern int opt_bus_chp_margin;
extern int opt_bus_seep_depth;
extern int opt_bus_seep_margin;
extern int opt_bus_seep_div;
extern int opt_bus_seepod_depth;
extern int opt_bus_seepod_margin;
extern int opt_bus_seep_noisy_depth;
extern int opt_bus_seep_noisy_margin;
extern int opt_bus_seep_noisy_hist_div;
extern int opt_bus_seep_noisy_depth2;
extern int opt_bus_seep_noisy_margin2;
extern int opt_cfp_depth;
extern int opt_cfp_margin;
extern int opt_cfp_mult;
extern int opt_probcut_depth;
extern int opt_probcut_margin;
extern int opt_probcut_reduction;
extern int opt_probcut_willimprove;
extern int opt_probcut_isstable;
extern int opt_probcut_willworsen;
extern int opt_lmr_check_depth;
extern int opt_lmr_cut_depth;
extern int opt_lmr_mejo_depth;
extern int opt_lmr_eval_depth;
extern int opt_lmr_ttd_depth;
extern int opt_lmr_ttcapt_depth;
extern int opt_lmr_hist_menos2;
extern int opt_lmr_hist_menos1;
extern int opt_lmr_hist_mas1;
extern int opt_lmr_chist_mas1;
extern int opt_lmr_chist_menos1;
extern int opt_lmr_tc_willimprove;
extern int opt_lmr_tc_willworsen;
extern int opt_lmr_tc_isstable;
extern int opt_lmr_tc_wllimpbeta;
extern int opt_lmr_tc_wilworalpha;
extern int opt_tc_threshold;
extern int opt_tc_dir_min_depth;
extern int opt_corr_grain;
extern int opt_corr_limit;
extern int opt_corr_peso;
extern char opt_tc_path[1024];
extern int opt_se_min_depth;
extern int opt_se_tt_depth_margin;
extern int opt_se_margin_base;
extern int opt_se_margin_scale;
extern int opt_se_double_margin;
extern int opt_se_triple_margin;

// Estructura de pesos (Float32)
typedef struct
{
    float W1[INPUT_SIZE][HIDDEN1];  // Transpuesta [feature][neuron]
    float b1[HIDDEN1];
    float W2[HIDDEN1][HIDDEN2];
    float b2[HIDDEN2];
    float W3[HIDDEN2][HIDDEN3];
    float b3[HIDDEN3];
    float W4[HIDDEN3][HIDDEN4];
    float b4[HIDDEN4];
    float W_out[HIDDEN4][OUTPUT_SIZE];
    float b_out[OUTPUT_SIZE];
} NNUEWeights;

extern NNUEWeights nnue_weights;

// Pesos del clasificador táctico (32→16→3)
#define TC_INPUT   32
#define TC_HIDDEN  16
#define TC_OUTPUT   3

typedef struct
{
    float W1[TC_INPUT][TC_HIDDEN];   // [in][out], ya transpuesto al cargar
    float b1[TC_HIDDEN];
    float W2[TC_HIDDEN][TC_OUTPUT];  // [in][out], ya transpuesto al cargar
    float b2[TC_OUTPUT];
} TacticalWeights;

typedef struct
{
    float p_mejora;
    float p_estable;
    float p_empeora;
} TacticalClass;

extern TacticalWeights tc_weights;
extern int             tc_initialized;
int         nnue_tc_init(const char* path);  // 1 si carga; si falla se conserva el anterior
int         nnue_tc_init_interna(void);      // el clasificador incluido en el ejecutable
// Igual que nnue_evaluate, pero permite saltarse el clasificador tactico cuando
// el llamante no lo va a mirar. tc_compute() cuesta el 20 % de una evaluacion
// (tres expf en el softmax), y en quiescence su resultado se multiplica por cero.
int         nnue_evaluate_tc(const Position* pos, int calcular_tc);
TacticalClass nnue_get_tc(void);

// --- Macros de Bitboards (Helpers) ---
#define SET_BIT(bb, sq) ((bb) |= (1ULL << (sq)))
#define GET_BIT(bb, sq) ((bb) & (1ULL << (sq)))
#define POP_BIT(bb, sq) ((bb) &= ~(1ULL << (sq)))

// Optimización: Population Count (Hardware)
static inline int count_bits(Bitboard bb)
{
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
    return __popcnt64(bb);
#elif defined(__POPCNT__)
    return __builtin_popcountll(bb);
#else
    // Ejecutable generico: la instruccion POPCNT solo si la CPU la tiene. Va en
    // ensamblador porque el compilador no la emite sin -mpopcnt.
    if (cpu_popcnt)
    {
        uint64_t r;
        __asm__("popcntq %1, %0" : "=r"(r) : "rm"(bb));
        return (int)r;
    }
    return __builtin_popcountll(bb);
#endif
}

// Función portable para obtener el índice del bit menos significativo (LSB)
static inline int get_lsb_index(Bitboard bb)
{
    if (bb == 0) return -1;
    // _tzcnt_u64 (BMI1) es más rápido y seguro en CPUs modernas
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
    return _tzcnt_u64(bb);
#else
    return __builtin_ctzll(bb);
#endif
}

// Optimización: Pop LSB (Eliminar el bit menos significativo y devolver su índice)
// bb &= (bb - 1): con BMI1 el compilador lo convierte solo en BLSR. La intrinseca
// _blsr_u64 no compilaba en el ejecutable generico.
static inline int pop_lsb(Bitboard* bb)
{
    int sq = get_lsb_index(*bb);
    *bb &= *bb - 1;
    return sq;
}

static inline int lmr_red(uint32_t x)
{
    if (x < 4)
        return 0;
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
    unsigned long index;
    _BitScanReverse(&index, x);
    return ((int)index-1);
#else
    return ((31 - __builtin_clz(x)))-1;
#endif
}

static inline int RedondeoSimetricoBase3(int r)
{
    if (r == 0)
        return (0);
    if (r > 0)
        return (r + 1) / 3;
    return (r - 1) / 3;
}

// --- Prototipos de funciones (position.c) ---
int parse_fen(Position* pos, const char* fen);  // 1 si es valido; si no, *pos no se toca
void print_board(const Position* pos);
void copy_position(Position* dest, const Position* src);
int make_move(Position* pos, Move move);
void make_null_move(Position* pos);
void init_zobrist(); // Inicializar claves aleatorias
uint64_t compute_pawn_hash(const Position* pos);
int is_repetition(const Position* pos);

// --- Prototipos de funciones (bitboards.c) ---
void init_bitboards();
Bitboard get_pawn_attacks(int side, int sq);
Bitboard get_knight_attacks(int sq);
Bitboard get_king_attacks(int sq);
Bitboard get_bishop_attacks(int sq, Bitboard occ);
Bitboard get_rook_attacks(int sq, Bitboard occ);
Bitboard get_queen_attacks(int sq, Bitboard occ);

// --- Prototipos de funciones (movegen.c) ---
int gives_check(const Position* pos, Move move);
int is_square_attacked(const Position* pos, int sq, int side);
void generate_moves(const Position* pos, MoveList* move_list, int gen_type);
void generate_captures(const Position* pos, MoveList* move_list);
void print_move(Move move);
void move_to_str(Move move, char* out);   // "e2e4" o "e7e8q"; out de 6 bytes como minimo
extern int salida_busqueda;   // 0 = search_position no imprime info ni bestmove (bench)
int see(const Position* pos, Move move, int threshold);

// --- Prototipos de funciones (evaluate.c) ---
int evaluate(const Position* pos);

// --- Prototipos de funciones (nnue.c) ---
int nnue_init(const char* path);    // 1 si carga; si falla se conserva la red anterior (B15)
int nnue_init_interna(void);        // la red incluida en el ejecutable
void nnue_acumular(float* acc, int feature, int signo);   // acc +-= W1[feature], con SIMD si hay
int nnue_evaluate(const Position* pos);
void nnue_refresh_accumulator(Position* pos);

// --- Prototipos de funciones (search.c) ---
void search_position(Position* pos, SearchLimits limits);
void preparar_busqueda(const Position* pos, SearchLimits limits); // en el hilo UCI, antes de crear el de busqueda (B1)
int busqueda_espera_stop(void);     // la busqueda en curso es infinite/ponder: solo acaba con stop/ponderhit
void engine_ponderhit();
void clear_search_heuristics(); // Limpiar history, continuation history, etc.
void init_razor_margin();       // Inicializar el array de márgenes de razoring

// --- Prototipos de funciones (main.c / perft) ---
void perft_test(int depth);
void run_bench(int depth, int hash_mb);
long long get_time_ms();

//
// Parámetros de búsqueda
//

// ============================================================================
// Correction history
// ============================================================================
//
// La red se equivoca de forma SISTEMATICA en ciertas familias de posiciones: si
// en todas las que comparten una estructura de peones la busqueda acaba diciendo
// 30 cp mas que la eval estatica, eso no es ruido, es un sesgo que se puede medir
// y descontar. Se guarda la diferencia media (busqueda - eval) indexada por el
// hash de peones y se suma a la eval estatica la proxima vez.
//
// OJO, lo que hay que hacer bien: la correccion se aplica a static_eval, que es
// la que se usa para podar, y NUNCA a raw_eval, que es la que se guarda en la
// tabla hash. Si se guardase corregida, al releerla se volveria a corregir y el
// sesgo se realimentaria solo.
//
// USA_CORRHIST a 0 lo desactiva entero, para poder comparar A/B.
// Se puede forzar desde la linea de ordenes con -DUSA_CORRHIST=0 para
// compilar la variante sin correccion y comparar A/B sin tocar el fichero.
#ifndef USA_CORRHIST
#define USA_CORRHIST                    1
#endif
#define CORR_SIZE                       16384   // potencia de 2

// Conservar las heuristicas de ordenacion entre jugadas. A 0 la tabla se borra al
// empezar cada busqueda (comportamiento antiguo); a 1 se conserva durante toda la
// partida y solo se borra con ucinewgame. Los killers conservados se desplazan dos
// plies, porque entre dos busquedas la partida avanza una jugada de cada bando.
// Se activan de una en una y se apilan si la prueba A/B sale a favor.
#ifndef CONSERVA_HISTORIA
#define CONSERVA_HISTORIA               1       // history_moves
#endif
#ifndef CONSERVA_CAPTURAS
#define CONSERVA_CAPTURAS               1       // capture_history
#endif
#ifndef CONSERVA_CONTRAJUGADAS
#define CONSERVA_CONTRAJUGADAS          0       // counter_moves
#endif
#ifndef CONSERVA_KILLERS
#define CONSERVA_KILLERS                0       // killer_moves
#endif
#ifndef CONSERVA_CORRHIST
#define CONSERVA_CORRHIST               1       // correction_history
#endif

#ifndef OPTIMIZAR_CORR
#define OPTIMIZAR_CORR      0
#endif

#if (OPTIMIZAR_CORR)
    #define CORR_GRAIN                  opt_corr_grain
    #define CORR_LIMIT                  opt_corr_limit
    #define CORR_PESO                   opt_corr_peso
#else
    #define CORR_GRAIN                  256     // divisor al aplicar
    #define CORR_LIMIT                  8173    // tope: +-32 cp de correccion
    #define CORR_PESO                   16      // peso maximo de una muestra
#endif
// USA_TC a 0 quita el clasificador tactico entero: no se carga tcnet, no se calcula
// tc_compute() y WillImprove/WillWorsen/IsStable quedan siempre a false. Se fuerza
// con -DUSA_TC=0 para comparar A/B sin tocar el fichero, como USA_CORRHIST.
// Desactivado por defecto desde el 17/11/2026: 6000 partidas a 5+0.05 sin TC contra
// con TC dieron +2,2 +- 5,5 Elo (empate tecnico), y sin TC es un 5 % mas rapido.
#ifndef USA_TC
#define USA_TC                          0
#endif
#define OPTIMIZAR_TC        0   // Clasificador tactico (WillImprove/WillWorsen/IsStable en LMR)
#define OPTIMIZAR_QS        0
#define OPTIMIZAR_RAZOR     0   // Razoring normal y Full Razoring
#define OPTIMIZAR_PODAJC    0
#define OPTIMIZAR_SEEP      0   // SEE Pruning (with history)
#define OPTIMIZAR_SEEPOD    0   // SEE Pruning Only Depth
#define OPTIMIZAR_CSEEP     0   // SEE de capturas (noisy)
#define OPTIMIZAR_CFP       0   // Capture Futility Pruning
#ifndef OPTIMIZAR_HP
#define OPTIMIZAR_HP        0   // History Pruning
#endif
#define OPTIMIZAR_CHP       0   // Continuation History Pruning
#define OPTIMIZAR_FP        0   // Futility Pruning
#define OPTIMIZAR_IIDR      0   // IID + IIR
#define OPTIMIZAR_LMP       0
#define OPTIMIZAR_NM        0   // Null Move
#define OPTIMIZAR_LMR       0
#ifndef OPTIMIZAR_LMR_HIST
#define OPTIMIZAR_LMR_HIST  0
#endif
#define OPTIMIZAR_PBCT      0   // ProbCut
#define OPTIMIZAR_SE        0   // Singluar extension
#define OPTIMIZAR_PHB       0   // Poda Hash Beta
#define OPTIMIZAR_PHA       0   // Poda Hash Alfa

#if (OPTIMIZAR_QS)
    #define QS_PODA_DELTA               opt_qs_poda_delta
    #define QS_FUT_MARGIN               opt_qs_fut_margin
    #define QS_SEE                      opt_qs_see
    #define QS_IMPROVE                  opt_qs_improve
    #define QS_STABLE                   opt_qs_stable
    #define QS_WORSEN                   opt_qs_worsen
    //#define QS_LMP                      opt_qs_lmp
#else
    #define QS_PODA_DELTA               91  // 98
    #define QS_FUT_MARGIN               86  // 70
    #define QS_SEE                      -66 // -85
    #define QS_IMPROVE                  0
    #define QS_STABLE                   0
    #define QS_WORSEN                   0
    //#define QS_LMP                      10
#endif

// El clasificador tactico en qsearch solo se calcula si algun peso de QS lo usa. Al
// tunear se fuerza a 1: si no, un clon con los tres pesos a 0 se ahorraria tc_compute()
// e iria mas rapido, y el optimizador leeria la velocidad como merito. Lo puso 26b28fe
// y el commit de la corrhist lo borro sin querer (B31).
#if (OPTIMIZAR_QS)
    #define QS_USA_TC                   1
#else
    #define QS_USA_TC                   (QS_IMPROVE || QS_STABLE || QS_WORSEN)
#endif

#if (OPTIMIZAR_RAZOR)
    #define BUS_RAZOR_DEPTH             opt_bus_razor_depth
    #define BUS_RAZOR_MARGIN1           opt_bus_razor_margin1
    #define BUS_RAZOR_MARGIN2           opt_bus_razor_margin2
    #define BUS_RAZOR_MARGIN3           opt_bus_razor_margin3
    #define BUS_RAZOR_MARGIN4           opt_bus_razor_margin4
    #define BUS_RAZOR_MARGIN5           opt_bus_razor_margin5
    #define BUS_FULLRAZ_USE             1
    #define BUS_FULLRAZ_BASE            opt_bus_fullraz_base
    #define BUS_FULLRAZ_MULT            opt_bus_fullraz_mult
    #define BUS_FULLRAZ_IMPROVE         opt_bus_fullraz_improve
    #define BUS_FULLRAZ_WORSEN          opt_bus_fullraz_worsen
#else
    #define BUS_RAZOR_DEPTH             4   // OJO: preparado hasta 5, pero Probon se queda en 4, y muy justo
    #define BUS_RAZOR_MARGIN1           110 // 75
    #define BUS_RAZOR_MARGIN2           230 // 130
    #define BUS_RAZOR_MARGIN3           420 // 315
    #define BUS_RAZOR_MARGIN4           930 // 500
    #define BUS_RAZOR_MARGIN5           1200
    #define BUS_FULLRAZ_USE             1
    #define BUS_FULLRAZ_BASE            380
    #define BUS_FULLRAZ_MULT            260
    #define BUS_FULLRAZ_IMPROVE         0
    #define BUS_FULLRAZ_WORSEN          -30
#endif

#if (OPTIMIZAR_PODAJC)
    #define BUS_PODAJC_DEPTH            opt_bus_podajc_depth
    #define BUS_PODAJC_MARGIN           opt_bus_podajc_margin
    #define BUS_PODAJC_CERO             opt_bus_podajc_cero
    #define BUS_PODAJC_IFWORSEN         opt_bus_podajc_ifworsen
#else
    #define BUS_PODAJC_DEPTH            9
    #define BUS_PODAJC_MARGIN           84
    #define BUS_PODAJC_CERO             -46
    #define BUS_PODAJC_IFWORSEN         0
#endif

#if (OPTIMIZAR_SEEP)
    #define BUS_SEEP_DEPTH              opt_bus_seep_depth
    #define BUS_SEEP_MARGIN             opt_bus_seep_margin
    #define BUS_SEEP_DIV                opt_bus_seep_div
#else
    #define BUS_SEEP_DEPTH              5
    #define BUS_SEEP_MARGIN             50
    #define BUS_SEEP_DIV                60
#endif

#if (OPTIMIZAR_SEEPOD)
    #define BUS_SEEPOD_DEPTH            opt_bus_seepod_depth
    #define BUS_SEEPOD_MARGIN           opt_bus_seepod_margin
#else
    #define BUS_SEEPOD_DEPTH            4
    #define BUS_SEEPOD_MARGIN           10
#endif

#if (OPTIMIZAR_CSEEP)
    #define BUS_SEEP_NOISY_DEPTH        opt_bus_seep_noisy_depth
    #define BUS_SEEP_NOISY_MARGIN       opt_bus_seep_noisy_margin
    #define BUS_SEEP_NOISY_HIST_DIV     opt_bus_seep_noisy_hist_div
    #define BUS_SEEP_NOISY_DEPTH2       opt_bus_seep_noisy_depth2
    #define BUS_SEEP_NOISY_MARGIN2      opt_bus_seep_noisy_margin2
#else
    #define BUS_SEEP_NOISY_DEPTH        3
    #define BUS_SEEP_NOISY_MARGIN       53
    #define BUS_SEEP_NOISY_HIST_DIV     225
    #define BUS_SEEP_NOISY_DEPTH2       16
    #define BUS_SEEP_NOISY_MARGIN2      169
#endif

#if (OPTIMIZAR_CFP)
    #define CFP_DEPTH                   opt_cfp_depth
    #define CFP_MARGIN                  opt_cfp_margin
    #define CFP_MULT                    opt_cfp_mult
#else
    #define CFP_DEPTH                   4
    #define CFP_MARGIN                  50
    #define CFP_MULT                    100
#endif

#if (OPTIMIZAR_HP)
    #define BUS_HP_DEPTH                opt_bus_hp_depth
    #define BUS_HP_MARGIN               opt_bus_hp_margin
#else
    #define BUS_HP_DEPTH                8
    #define BUS_HP_MARGIN               961
#endif

#if (OPTIMIZAR_CHP)
    #define BUS_CHP_DEPTH               opt_bus_chp_depth
    #define BUS_CHP_MARGIN              opt_bus_chp_margin
#else
    #define BUS_CHP_DEPTH               4
    #define BUS_CHP_MARGIN              2000
#endif

#if (OPTIMIZAR_FP)
    #define BUS_FUT_DEPTH               opt_bus_fut_depth
    #define BUS_FUT_BASE                opt_bus_fut_base
    #define BUS_FUT_MARGIN              opt_bus_fut_margin
#else
    #define BUS_FUT_DEPTH               6
    #define BUS_FUT_BASE                25
    #define BUS_FUT_MARGIN              40
#endif

#if (OPTIMIZAR_IIDR)
    #define BUS_IID_DEPTH_MIN           opt_bus_iid_depthmin
    #define BUS_IID_IMPROVE             opt_bus_iid_improve
    #define BUS_IID_WORSEN              opt_bus_iid_worsen
    #define BUS_IIR_DEPTH               opt_bus_iir_depth
    #define BUS_IIR_PVRED               opt_bus_iir_pvred
    #define BUS_IIR_CUTRED              opt_bus_iir_cutred
    #define BUS_IIR_ALLRED              opt_bus_iir_allred
    #define BUS_IIR_PV_STABLE           opt_bus_iir_pv_stable
    #define BUS_IIR_CUT_IMPROVE         opt_bus_iir_cut_improve
    #define BUS_IIR_ALL_WORSEN          opt_bus_iir_all_worsen
#else
    #define BUS_IID_DEPTH_MIN           9
    #define BUS_IID_IMPROVE             60
    #define BUS_IID_WORSEN              40
    #define BUS_IIR_DEPTH               3
    #define BUS_IIR_PVRED               1
    #define BUS_IIR_CUTRED              1
    #define BUS_IIR_ALLRED              1
    #define BUS_IIR_PV_STABLE           0
    #define BUS_IIR_CUT_IMPROVE         0
    #define BUS_IIR_ALL_WORSEN          -1
#endif

#if (OPTIMIZAR_LMP)
    #define LMP_BASE                    opt_lmp_base
    #define LMP_DEPTH                   opt_lmp_depth
#else
    #define LMP_BASE                    9
    #define LMP_DEPTH                   10
#endif

#if (OPTIMIZAR_NM)
    #define BUS_NM_CUTYALL              opt_bus_nm_cutyall
    #define BUS_NM_R                    opt_bus_nm_r
    #define BUS_NM_IMP                  opt_bus_nm_imp
    #define BUS_NM_DIV                  opt_bus_nm_div
    #define BUS_NM_VERIF                opt_bus_nm_verif
    #define BUS_NM_IFWORSEN             opt_bus_nm_ifworsen
    #define BUS_NM_WILLIMPROVE          opt_bus_nm_willimprove
    #define BUS_NM_ISSTABLE             opt_bus_nm_isstable
    #define BUS_NM_WILLWORSEN           opt_bus_nm_willworsen
#else
    #define BUS_NM_CUTYALL              1
    #define BUS_NM_R                    4
    #define BUS_NM_IMP                  0
    #define BUS_NM_DIV                  229
    #define BUS_NM_VERIF                15
    #define BUS_NM_IFWORSEN             1
    #define BUS_NM_WILLIMPROVE          20
    #define BUS_NM_ISSTABLE             25
    #define BUS_NM_WILLWORSEN           5
#endif

#define LMR_MODIF_SCALE                 0

#if (OPTIMIZAR_LMR)
    #define LMR_BASE_DEPTH              opt_lmr_base_depth
    #define LMR_BASE_MOVE               opt_lmr_base_move
    #define LMR_CHECK_DEPTH             opt_lmr_check_depth
    #define LMR_CUT_DEPTH               opt_lmr_cut_depth
    #define LMR_MEJO_DEPTH              opt_lmr_mejo_depth
    #define LMR_GOODMOVE_MARGIN         opt_lmr_goodmove_margin
    #define LMR_EVAL_DEPTH              opt_lmr_eval_depth
    #define LMR_TTD_DEPTH               opt_lmr_ttd_depth
    #define LMR_TTCAPT_DEPTH            opt_lmr_ttcapt_depth
    #define LMR_TC_WILLIMPROVE          opt_lmr_tc_willimprove
    #define LMR_TC_WILLWORSEN           opt_lmr_tc_willworsen
    #define LMR_TC_ISSTABLE             opt_lmr_tc_isstable
    #define LMR_TC_WLLIMPBETA           opt_lmr_tc_wllimpbeta
    #define LMR_TC_WILWORALPHA          opt_lmr_tc_wilworalpha
#else
    #define LMR_BASE_DEPTH              1
    #define LMR_BASE_MOVE               4
    #define LMR_CHECK_DEPTH             -1
    #define LMR_CUT_DEPTH               5
    #define LMR_MEJO_DEPTH              -1
    #define LMR_GOODMOVE_MARGIN         230
    #define LMR_EVAL_DEPTH              0
    #define LMR_TTD_DEPTH               -1
    #define LMR_TTCAPT_DEPTH            1
    #define LMR_TC_WILLIMPROVE          -1
    #define LMR_TC_WILLWORSEN           1
    #define LMR_TC_ISSTABLE             0
    #define LMR_TC_WLLIMPBETA           -1
    #define LMR_TC_WILWORALPHA          1
#endif

#if (OPTIMIZAR_LMR_HIST)
    #define LMR_HIST_MENOS2             opt_lmr_hist_menos2
    #define LMR_HIST_MENOS1             opt_lmr_hist_menos1
    #define LMR_HIST_MAS1               opt_lmr_hist_mas1
    #define LMR_CHIST_MAS1              opt_lmr_chist_mas1
    #define LMR_CHIST_MENOS1            opt_lmr_chist_menos1
#else
    #define LMR_HIST_MENOS2             8004
    #define LMR_HIST_MENOS1             3432
    #define LMR_HIST_MAS1               -1936
    #define LMR_CHIST_MAS1              -235
    #define LMR_CHIST_MENOS1            140
#endif

// Clasificador tactico en LMR (unidades de 1/3 ply, igual que el resto de LMR)
#if (OPTIMIZAR_TC)
    #define TC_THRESHOLD                opt_tc_threshold
    #define TC_DIR_MIN_DEPTH            opt_tc_dir_min_depth
#else
    #define TC_THRESHOLD                82
    // Profundidad restante minima para hacer caso a la DIRECCION del clasificador
    // (bWillImprove / bWillWorsen). Medido sobre 2584 posiciones de partida, el
    // acierto de signo del clasificador cuando algo pasa de verdad (|diff| > 50 cp
    // a esa misma profundidad) y el clasificador se moja (|dir| > 0.3):
    //
    //     prof  1..7   41-45 %   <- por debajo de la moneda, hasta 3.8 sigma
    //     prof  8..11  47-54 %   <- ruido
    //     prof 12..15  56-60 %   <- util, hasta 5.1 sigma
    //
    // El clasificador predice DONDE ACABA la eval; una busqueda corta todavia va
    // de camino y a menudo se mueve al reves antes de darse la vuelta. Por eso el
    // dato esta invertido cerca de las hojas, que es donde estan casi todos los nodos.
    //
    // bIsStable (p_estable) NO entra aqui: ese si acierta a todas las profundidades
    // (separa el error de la eval por un factor 6-12x desde la profundidad 1).
    //
    // 0 = comportamiento de siempre (hacer caso a cualquier profundidad).
    #define TC_DIR_MIN_DEPTH            0
#endif

#if (OPTIMIZAR_PBCT)
    #define PROBCUT_DEPTH               opt_probcut_depth
    #define PROBCUT_MARGIN              opt_probcut_margin
    #define PROBCUT_REDUCTION           opt_probcut_reduction
    #define PROBCUT_WILLIMPROVE         opt_probcut_willimprove
    #define PROBCUT_ISSTABLE            opt_probcut_isstable
    #define PROBCUT_WILLWORSEN          opt_probcut_willworsen
#else
    #define PROBCUT_DEPTH               4
    #define PROBCUT_MARGIN              90
    #define PROBCUT_REDUCTION           6
    #define PROBCUT_WILLIMPROVE         20
    #define PROBCUT_ISSTABLE            40
    #define PROBCUT_WILLWORSEN          5
#endif

#if (OPTIMIZAR_SE)
    #define EXT_LIMIT_MULTIPLIER        (opt_ext_limit_multiplier / 10.0)
    #define SE_MIN_DEPTH                opt_se_min_depth
    #define SE_TT_DEPTH_MARGIN          opt_se_tt_depth_margin
    #define SE_MARGIN_BASE              opt_se_margin_base
    #define SE_MARGIN_SCALE             opt_se_margin_scale
    #define SE_DOUBLE_MARGIN            opt_se_double_margin
    #define SE_TRIPLE_MARGIN            opt_se_triple_margin
#else
    #define EXT_LIMIT_MULTIPLIER        0.5
    #define SE_MIN_DEPTH                9
    #define SE_TT_DEPTH_MARGIN          1
    #define SE_MARGIN_BASE              2
    #define SE_MARGIN_SCALE             1
    #define SE_DOUBLE_MARGIN            58
    #define SE_TRIPLE_MARGIN            143
#endif


#if (OPTIMIZAR_PHB)
    #define PODA_HASH_BETA_PROF_DIF     opt_poda_hash_beta_prof_dif
    #define PODA_HASH_BETA_BASE         opt_poda_hash_beta_base
    #define PODA_HASH_BETA_MULT_DIF     opt_poda_hash_beta_mult_dif
    #define PODA_HASH_BETA_MULT_PROF    opt_poda_hash_beta_mult_prof
#else
    #define PODA_HASH_BETA_PROF_DIF     14
    #define PODA_HASH_BETA_BASE         195
    #define PODA_HASH_BETA_MULT_DIF     14
    #define PODA_HASH_BETA_MULT_PROF    50
#endif

#if (OPTIMIZAR_PHA)
    #define PODA_HASH_ALFA_PROF_DIF     opt_poda_hash_alfa_prof_dif
    #define PODA_HASH_ALFA_BASE         opt_poda_hash_alfa_base
    #define PODA_HASH_ALFA_MULT_DIF     opt_poda_hash_alfa_mult_dif
    #define PODA_HASH_ALFA_MULT_PROF    opt_poda_hash_alfa_mult_prof
#else
    #define PODA_HASH_ALFA_PROF_DIF     0
    #define PODA_HASH_ALFA_BASE         99
    #define PODA_HASH_ALFA_MULT_DIF     111
    #define PODA_HASH_ALFA_MULT_PROF    -10
#endif
