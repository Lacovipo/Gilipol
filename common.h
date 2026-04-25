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

#define MATE_SCORE 32000
#define MATE_BOUND 31000
#define INFINITO 32001
#define TB_MAX 30000

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
    int nodes;      // Nodos máximos
    int movetime;   // Tiempo fijo por movimiento
    bool infinite;  // Analizar hasta recibir comando 'stop'
    bool ponder;    // Modo Ponder (pensar en tiempo del oponente)
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

// --- Globales de Control ---
#define MAX_GAME_HISTORY 1024

extern ATOMIC_INT stop_search;
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
extern int opt_ext_limit_multiplier;
extern int opt_lmp_base;
extern int opt_lmp_depth;
extern int opt_lmr_goodmove_margin;
extern int opt_bus_nm_cutyall;
extern int opt_bus_nm_div;
extern int opt_bus_nm_imp;
extern int opt_bus_nm_r;
extern int opt_bus_nm_verif;
extern int opt_bus_iid_depthmin;
extern int opt_bus_podajc_depth;
extern int opt_bus_podajc_margin;
extern int opt_bus_podajc_cero;
extern int opt_bus_iir_depth;
extern int opt_bus_iir_pvred;
extern int opt_bus_iir_cutred;
extern int opt_bus_iir_allred;
extern int opt_bus_razor_depth;
extern int opt_bus_razor_margin1;
extern int opt_bus_razor_margin2;
extern int opt_bus_razor_margin3;
extern int opt_bus_razor_margin4;
extern int opt_bus_razor_margin5;
extern int opt_bus_fullraz_base;
extern int opt_bus_fullraz_mult;
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

// --- Macros de Bitboards (Helpers) ---
#define SET_BIT(bb, sq) ((bb) |= (1ULL << (sq)))
#define GET_BIT(bb, sq) ((bb) & (1ULL << (sq)))
#define POP_BIT(bb, sq) ((bb) &= ~(1ULL << (sq)))

// Optimización: Population Count (Hardware)
static inline int count_bits(Bitboard bb)
{
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
    return __popcnt64(bb);
#else
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
// Usa instrucción BLSR (BMI1) para limpiar el bit: bb &= (bb - 1)
static inline int pop_lsb(Bitboard* bb)
{
    int sq = get_lsb_index(*bb);
    *bb = _blsr_u64(*bb); // Intrínseca directa para bb &= bb - 1
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
void parse_fen(Position* pos, const char* fen);
void print_board(const Position* pos);
void copy_position(Position* dest, const Position* src);
int make_move(Position* pos, Move move);
void make_null_move(Position* pos);
void init_zobrist(); // Inicializar claves aleatorias
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
int see(const Position* pos, Move move, int threshold);

// --- Prototipos de funciones (evaluate.c) ---
int evaluate(const Position* pos);

// --- Prototipos de funciones (nnue.c) ---
void nnue_init(const char* path);
int nnue_evaluate(const Position* pos);
void nnue_refresh_accumulator(Position* pos);

// --- Prototipos de funciones (search.c) ---
void search_position(Position* pos, SearchLimits limits);
void engine_ponderhit();
void clear_search_heuristics(); // Limpiar history, continuation history, etc.
void init_razor_margin();       // Inicializar el array de márgenes de razoring

// --- Prototipos de funciones (main.c / perft) ---
void perft_test(int depth);
long long get_time_ms();

//
// Parámetros de búsqueda
//

#define OPTIMIZAR_QS        0
#define OPTIMIZAR_RAZOR     0   // Razoring normal y Full Razoring
#define OPTIMIZAR_PODAJC    0
#define OPTIMIZAR_SEEP      0   // SEE Pruning (with history)
#define OPTIMIZAR_SEEPOD    0   // SEE Pruning Only Depth
#define OPTIMIZAR_CSEEP     0   // SEE de capturas (noisy)
#define OPTIMIZAR_CFP       0   // Capture Futility Pruning
#define OPTIMIZAR_HP        0   // History Pruning
#define OPTIMIZAR_CHP       0   // Continuation History Pruning
#define OPTIMIZAR_FP        0   // Futility Pruning
#define OPTIMIZAR_IIDR      0   // IID + IIR
#define OPTIMIZAR_LMP       0
#define OPTIMIZAR_NM        0   // Null Move
#define OPTIMIZAR_LMR       0
#define OPTIMIZAR_PBCT      0   // ProbCut
#define OPTIMIZAR_SE        0   // Singluar extension
#define OPTIMIZAR_PHB       0   // Poda Hash Beta
#define OPTIMIZAR_PHA       0   // Poda Hash Alfa

#if (OPTIMIZAR_QS)
    #define QS_PODA_DELTA               opt_qs_poda_delta
    #define QS_FUT_MARGIN               opt_qs_fut_margin
    //#define QS_LMP                      opt_qs_lmp
    #define QS_SEE                      opt_qs_see
#else
    #define QS_PODA_DELTA               98
    #define QS_FUT_MARGIN               70
    //#define QS_LMP                      10
    #define QS_SEE                      -85
#endif

#if (OPTIMIZAR_RAZOR)
    #define BUS_RAZOR_DEPTH             opt_bus_razor_depth
    #define BUS_RAZOR_MARGIN1           opt_bus_razor_margin1
    #define BUS_RAZOR_MARGIN2           opt_bus_razor_margin2
    #define BUS_RAZOR_MARGIN3           opt_bus_razor_margin3
    #define BUS_RAZOR_MARGIN4           opt_bus_razor_margin4
    #define BUS_RAZOR_MARGIN5           opt_bus_razor_margin5
    #define BUS_FULLRAZ_BASE            opt_bus_fullraz_base
    #define BUS_FULLRAZ_MULT            opt_bus_fullraz_mult
#else
    #define BUS_RAZOR_DEPTH             4   // 12/04/26 // OJO: preparado hasta 5, pero sin probar
    #define BUS_RAZOR_MARGIN1           75  // 07/04/26
    #define BUS_RAZOR_MARGIN2           130 // 08/04/26
    #define BUS_RAZOR_MARGIN3           315 // 11/04/26
    #define BUS_RAZOR_MARGIN4           500 // 12/04/26
    #define BUS_RAZOR_MARGIN5           700
    #define BUS_FULLRAZ_BASE            350
    #define BUS_FULLRAZ_MULT            260
#endif

#if (OPTIMIZAR_PODAJC)
    #define BUS_PODAJC_DEPTH            opt_bus_podajc_depth
    #define BUS_PODAJC_MARGIN           opt_bus_podajc_margin
    #define BUS_PODAJC_CERO             opt_bus_podajc_cero
#else
    #define BUS_PODAJC_DEPTH            10
    #define BUS_PODAJC_MARGIN           50
    #define BUS_PODAJC_CERO             -30
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
    #define BUS_HP_DEPTH                9
    #define BUS_HP_MARGIN               0
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
    #define BUS_IIR_DEPTH               opt_bus_iir_depth
    #define BUS_IIR_PVRED               opt_bus_iir_pvred
    #define BUS_IIR_CUTRED              opt_bus_iir_cutred
    #define BUS_IIR_ALLRED              opt_bus_iir_allred
#else
    #define BUS_IID_DEPTH_MIN           10
    #define BUS_IIR_DEPTH               4
    #define BUS_IIR_PVRED               2
    #define BUS_IIR_CUTRED              2
    #define BUS_IIR_ALLRED              0
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
#else
    #define BUS_NM_CUTYALL              1
    #define BUS_NM_R                    4
    #define BUS_NM_IMP                  1
    #define BUS_NM_DIV                  197
    #define BUS_NM_VERIF                8
#endif

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
    #define LMR_HIST_MENOS2             opt_lmr_hist_menos2
    #define LMR_HIST_MENOS1             opt_lmr_hist_menos1
    #define LMR_HIST_MAS1               opt_lmr_hist_mas1
    #define LMR_CHIST_MAS1              opt_lmr_chist_mas1
    #define LMR_CHIST_MENOS1            opt_lmr_chist_menos1
#else
    #define LMR_BASE_DEPTH              2
    #define LMR_BASE_MOVE               4
    #define LMR_CHECK_DEPTH             -3
    #define LMR_CUT_DEPTH               3
    #define LMR_MEJO_DEPTH              -3
    #define LMR_GOODMOVE_MARGIN         230
    #define LMR_EVAL_DEPTH              4
    #define LMR_TTD_DEPTH               -1
    #define LMR_TTCAPT_DEPTH            1
    #define LMR_HIST_MENOS2             7974
    #define LMR_HIST_MENOS1             3424
    #define LMR_HIST_MAS1               -2252
    #define LMR_CHIST_MAS1              -240
    #define LMR_CHIST_MENOS1            135
#endif

#if (OPTIMIZAR_PBCT)
    #define PROBCUT_DEPTH               opt_probcut_depth
    #define PROBCUT_MARGIN              opt_probcut_margin
    #define PROBCUT_REDUCTION           opt_probcut_reduction
#else
    #define PROBCUT_DEPTH               7
    #define PROBCUT_MARGIN              100
    #define PROBCUT_REDUCTION           5
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

#define LMR_MODIF_SCALE             0
