/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

#include "common.h"
#include <math.h>
#include <immintrin.h>

NNUEWeights nnue_weights;
static int  nnue_initialized = 0;

TacticalWeights tc_weights;
int             tc_initialized = 0;

static _Thread_local TacticalClass tc_last = {0.333f, 0.334f, 0.333f};

// ============================================================================
// DETECCION DE LA CPU EN TIEMPO DE EJECUCION
// ============================================================================
//
// El ejecutable de distribucion se compila para x86-64 generico, y las partes que
// mas pesan eligen al arrancar la mejor variante que admite el procesador:
//
//   - la red y el acumulador: AVX-512, AVX2+FMA o generica (SSE2)
//   - los ataques de alfiles y torres: PEXT (BMI2) o magic bitboards
//   - count_bits: instruccion POPCNT o la rutina de la libreria
//
// Las funciones de cada variante llevan __attribute__((target(...))), asi que solo
// se ejecutan instrucciones AVX/BMI2 si la CPU (y el sistema operativo, para los
// registros YMM/ZMM) las admiten. AVX-512 y AVX2 dan exactamente la misma
// evaluacion; la generica no usa FMA y puede diferir en el ultimo bit.
//
// Para probar una variante inferior en una maquina moderna:
//   GILIPOL_CPU=generica|avx2|avx512   GILIPOL_PEXT=0   GILIPOL_POPCNT=0

int cpu_nivel  = CPU_GENERICA;
int cpu_pext   = 0;
int cpu_popcnt = 0;
static int cpu_detectada = 0;

void detectar_cpu(void)
{
    if (cpu_detectada)
        return;
    cpu_detectada = 1;

    __builtin_cpu_init();
    cpu_popcnt = __builtin_cpu_supports("popcnt") ? 1 : 0;

    // El PEXT de AMD anterior a Zen 3 es microcodigo (cientos de ciclos): ahi las
    // magic bitboards son mucho mas rapidas.
    cpu_pext = __builtin_cpu_supports("bmi2") ? 1 : 0;
    if (__builtin_cpu_is("amdfam15h") || __builtin_cpu_is("znver1") || __builtin_cpu_is("znver2"))
        cpu_pext = 0;

    if (__builtin_cpu_supports("avx512f"))
        cpu_nivel = CPU_AVX512;
    else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        cpu_nivel = CPU_AVX2;
    else
        cpu_nivel = CPU_GENERICA;

    // Solo se puede forzar una variante IGUAL O INFERIOR a la detectada
    const char* e = getenv("GILIPOL_CPU");
    if (e)
    {
        int forzado = cpu_nivel;
        if (strcmp(e, "generica") == 0)
            forzado = CPU_GENERICA;
        else if (strcmp(e, "avx2") == 0)
            forzado = CPU_AVX2;
        else if (strcmp(e, "avx512") == 0)
            forzado = CPU_AVX512;
        if (forzado < cpu_nivel)
            cpu_nivel = forzado;
    }
    e = getenv("GILIPOL_PEXT");
    if (e && e[0] == '0')
        cpu_pext = 0;
    e = getenv("GILIPOL_POPCNT");
    if (e && e[0] == '0')
        cpu_popcnt = 0;
}

const char* nombre_cpu(void)
{
    static char texto[64];
    detectar_cpu();
    snprintf(texto, sizeof(texto), "%s, %s, %s",
             cpu_nivel == CPU_AVX512 ? "AVX-512" : cpu_nivel == CPU_AVX2 ? "AVX2" : "generica",
             cpu_pext ? "PEXT" : "magics",
             cpu_popcnt ? "POPCNT" : "sin POPCNT");
    return texto;
}

// noinline: si el compilador mete las tres variantes dentro de nnue_evaluate_tc
// (pasa al compilar con -march=native), la evaluacion pierde un 15 % de velocidad.
#define ATTR_AVX2     __attribute__((target("avx2,fma"), noinline))
#define ATTR_AVX512   __attribute__((target("avx512f,avx2,fma"), noinline))
#define ATTR_GENERICA __attribute__((noinline))

// ============================================================================
// REDES INCLUIDAS EN EL EJECUTABLE
// ============================================================================
//
// Los ficheros se meten tal cual en la seccion de datos de solo lectura con la
// directiva .incbin del ensamblador (la ruta es relativa al directorio desde el
// que se compila). EvalFile/TCFile siguen pudiendo cargar otra red desde disco.

#ifndef RED_EMBEBIDA
#define RED_EMBEBIDA    RED_POR_DEFECTO
#endif
#ifndef TC_EMBEBIDA
#define TC_EMBEBIDA     TC_POR_DEFECTO
#endif

#ifdef _WIN32
#define SECCION_SOLO_LECTURA "    .section .rdata,\"dr\"\n"
#else
#define SECCION_SOLO_LECTURA "    .section .rodata\n"
#endif

__asm__(
    SECCION_SOLO_LECTURA
    "    .p2align 6\n"
    "    .globl gilipol_red_ini\n"
    "gilipol_red_ini:\n"
    "    .incbin \"" RED_EMBEBIDA "\"\n"
    "    .globl gilipol_red_fin\n"
    "gilipol_red_fin:\n"
    "    .p2align 6\n"
    "    .globl gilipol_tc_ini\n"
    "gilipol_tc_ini:\n"
    "    .incbin \"" TC_EMBEBIDA "\"\n"
    "    .globl gilipol_tc_fin\n"
    "gilipol_tc_fin:\n"
    "    .text\n");

extern const unsigned char gilipol_red_ini[], gilipol_red_fin[];
extern const unsigned char gilipol_tc_ini[],  gilipol_tc_fin[];

// Clasificador tactico: 32 (h4 post-relu) -> 16 -> 3 (softmax)
static inline void tc_compute(const float* h4)
{
    float h[TC_HIDDEN];
    for (int i = 0; i < TC_HIDDEN; i++)
    {
        float s = tc_weights.b1[i];
        for (int j = 0; j < TC_INPUT; j++)
            s += h4[j] * tc_weights.W1[j][i];
        h[i] = s > 0.0f ? s : 0.0f;
    }
    float logits[TC_OUTPUT];
    float max_l = -1e30f;
    for (int i = 0; i < TC_OUTPUT; i++)
    {
        float s = tc_weights.b2[i];
        for (int j = 0; j < TC_HIDDEN; j++)
            s += h[j] * tc_weights.W2[j][i];
        logits[i] = s;
        if (s > max_l) max_l = s;
    }
    float sum = 0.0f;
    float exps[TC_OUTPUT];
    for (int i = 0; i < TC_OUTPUT; i++) { exps[i] = expf(logits[i] - max_l); sum += exps[i]; }
    tc_last.p_mejora  = exps[0] / sum;
    tc_last.p_estable = exps[1] / sum;
    tc_last.p_empeora = exps[2] / sum;
}

TacticalClass nnue_get_tc(void) { return tc_last; }

// Función ReLU
static inline float relu(float x)
{
    return x > 0.0f ? x : 0.0f;
}

// Helper para obtener TODOS los ataques de un bando a la vez
static Bitboard get_all_attacks(const Position* pos, int side)
{
    Bitboard attacks = 0;
    Bitboard occ = pos->all;

    // Peones
    Bitboard pawns = pos->pieces[side][PAWN];
    if (side == WHITE)
        attacks |= ((pawns << 7) & 0x7F7F7F7F7F7F7F7FULL) | ((pawns << 9) & 0xFEFEFEFEFEFEFEFEULL);
    else
        attacks |= ((pawns >> 9) & 0x7F7F7F7F7F7F7F7FULL) | ((pawns >> 7) & 0xFEFEFEFEFEFEFEFEULL);

    // Caballos y Rey
    Bitboard knights = pos->pieces[side][KNIGHT];
    while (knights)
        attacks |= get_knight_attacks(pop_lsb(&knights));

    Bitboard king = pos->pieces[side][KING];
    if (king)
        attacks |= get_king_attacks(get_lsb_index(king));

    // Piezas deslizantes (Alfiles, Torres, Damas)
    Bitboard bq = pos->pieces[side][BISHOP] | pos->pieces[side][QUEEN];
    while (bq)
        attacks |= get_bishop_attacks(pop_lsb(&bq), occ);

    Bitboard rq = pos->pieces[side][ROOK] | pos->pieces[side][QUEEN];
    while (rq)
        attacks |= get_rook_attacks(pop_lsb(&rq), occ);

    return attacks;
}

/*
 *
 * Carga de pesos
 *
 */
// Valida y reparte una red ya leida (de disco o de la copia interna). Devuelve 1 si
// la red es buena. Si falla, se CONSERVA la red que hubiera: antes un EvalFile mal
// escrito tiraba la red buena y el motor jugaba con eval 0 (B15).
static int nnue_cargar(const float* weights, size_t num_floats, const char* nombre)
{
    const size_t total_params =
        (INPUT_SIZE * HIDDEN1) + HIDDEN1 +
        (HIDDEN1 * HIDDEN2) + HIDDEN2 +
        (HIDDEN2 * HIDDEN3) + HIDDEN3 +
        (HIDDEN3 * HIDDEN4) + HIDDEN4 +
        (HIDDEN4 * OUTPUT_SIZE) + OUTPUT_SIZE;

    // Un fichero mas corto o mas largo no es esta red (I80)
    if (num_floats != total_params)
    {
        printf("info string Error: fichero NNUE corrupto o de tamano incorrecto (%s)\n", nombre);
        return 0;
    }

    // Pesos NaN o infinitos: la salida seria basura en todas las posiciones (I80, B25)
    for (size_t k = 0; k < num_floats; k++)
    {
        if (!isfinite(weights[k]))
        {
            printf("info string Error: la NNUE %s tiene pesos no finitos; se descarta\n", nombre);
            return 0;
        }
    }

    // Distribuir y transponer pesos (PyTorch exporta [out][in], nosotros queremos [in][out])
    const float* ptr = weights;

    // Layer 1: [512][786] -> [786][512]
    const float* w1 = ptr; ptr += INPUT_SIZE * HIDDEN1;
    const float* b1 = ptr; ptr += HIDDEN1;
    for (int i = 0; i < HIDDEN1; i++)
    {
        nnue_weights.b1[i] = b1[i];
        for (int j = 0; j < INPUT_SIZE; j++)
            nnue_weights.W1[j][i] = w1[i * INPUT_SIZE + j];
    }

    // Layer 2: [128][512] -> [512][128]
    const float* w2 = ptr; ptr += HIDDEN1 * HIDDEN2;
    const float* b2 = ptr; ptr += HIDDEN2;
    for (int i = 0; i < HIDDEN2; i++)
    {
        nnue_weights.b2[i] = b2[i];
        for (int j = 0; j < HIDDEN1; j++)
            nnue_weights.W2[j][i] = w2[i * HIDDEN1 + j];
    }

    // Layer 3: [64][128] -> [128][64]
    const float* w3 = ptr; ptr += HIDDEN2 * HIDDEN3;
    const float* b3 = ptr; ptr += HIDDEN3;
    for (int i = 0; i < HIDDEN3; i++)
    {
        nnue_weights.b3[i] = b3[i];
        for (int j = 0; j < HIDDEN2; j++)
            nnue_weights.W3[j][i] = w3[i * HIDDEN2 + j];
    }

    // Layer 4: [32][64] -> [64][32]
    const float* w4 = ptr; ptr += HIDDEN3 * HIDDEN4;
    const float* b4 = ptr; ptr += HIDDEN4;
    for (int i = 0; i < HIDDEN4; i++)
    {
        nnue_weights.b4[i] = b4[i];
        for (int j = 0; j < HIDDEN3; j++)
            nnue_weights.W4[j][i] = w4[i * HIDDEN3 + j];
    }

    // Output: [1][32] -> [32][1]
    const float* w_out = ptr; ptr += HIDDEN4 * OUTPUT_SIZE;
    const float* b_out = ptr;
    nnue_weights.b_out[0] = b_out[0];
    for (int i = 0; i < HIDDEN4; i++)
        nnue_weights.W_out[i][0] = w_out[i];

    nnue_initialized = 1;
    printf("info string NNUE cargada correctamente (%s)\n", nombre);
    return 1;
}

// Red desde disco (EvalFile)
int nnue_init(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        if (nnue_initialized)
            printf("info string No se pudo abrir la NNUE %s; se mantiene la red anterior\n", path);
        else
            printf("info string ERROR: no se pudo cargar la NNUE desde %s: SIN RED, el motor juega con eval 0\n", path);
        return 0;
    }

    const size_t total_params =
        (INPUT_SIZE * HIDDEN1) + HIDDEN1 +
        (HIDDEN1 * HIDDEN2) + HIDDEN2 +
        (HIDDEN2 * HIDDEN3) + HIDDEN3 +
        (HIDDEN3 * HIDDEN4) + HIDDEN4 +
        (HIDDEN4 * OUTPUT_SIZE) + OUTPUT_SIZE;

    float* weights = (float*)malloc(total_params * sizeof(float));
    if (!weights)
    {
        printf("info string Error: memoria insuficiente para cargar la NNUE\n");
        fclose(f);
        return 0;
    }

    size_t read = fread(weights, sizeof(float), total_params, f);
    if (read == total_params && fgetc(f) != EOF)    // sobran bytes: tampoco es esta red (I80)
        read = total_params + 1;
    fclose(f);

    const int ok = nnue_cargar(weights, read, path);
    free(weights);
    return ok;
}

// Red incluida en el ejecutable
int nnue_init_interna(void)
{
    const size_t bytes = (size_t)(gilipol_red_fin - gilipol_red_ini);
    if (bytes % sizeof(float) != 0)
    {
        printf("info string Error: la red interna tiene un tamano invalido\n");
        return 0;
    }
    return nnue_cargar((const float*)gilipol_red_ini, bytes / sizeof(float), "interna, " RED_EMBEBIDA);
}

// Clasificador tactico a partir de un bloque de bytes (fichero o copia interna).
// Todo se lee a temporales y solo se copia si el bloque esta completo: si falla, se
// conserva el clasificador que hubiera.
static int nnue_tc_cargar(const unsigned char* datos, size_t bytes, const char* nombre)
{
    int dims[3] = { 0, 0, 0 };     // inicializadas: con la cabecera truncada se imprimia basura (I27)
    if (bytes >= sizeof(dims))
        memcpy(dims, datos, sizeof(dims));
    if (bytes < sizeof(dims) || dims[0] != TC_INPUT || dims[1] != TC_HIDDEN || dims[2] != TC_OUTPUT)
    {
        printf("info string Error: clasificador tactico incompatible (%d->%d->%d, esperado %d->%d->%d)\n",
               dims[0], dims[1], dims[2], TC_INPUT, TC_HIDDEN, TC_OUTPUT);
        return 0;
    }

    // OJO: hay que comprobar el tamano. Antes un fichero truncado dejaba pesos basura
    // con tc_initialized = 1: el motor guiando la busqueda con un clasificador
    // aleatorio y sin decir ni pio.
    const size_t esperados = (size_t)TC_HIDDEN * TC_INPUT + TC_HIDDEN
                           + (size_t)TC_OUTPUT * TC_HIDDEN + TC_OUTPUT;
    const size_t disponibles = (bytes - sizeof(dims)) / sizeof(float);
    if (disponibles < esperados)
    {
        printf("info string Error: clasificador tactico truncado (%zu de %zu valores)\n",
               disponibles, esperados);
        return 0;
    }

    // W1: [TC_HIDDEN][TC_INPUT] en el fichero -> transponer a [TC_INPUT][TC_HIDDEN]
    float W1_raw[TC_HIDDEN][TC_INPUT];
    float b1_raw[TC_HIDDEN];
    float W2_raw[TC_OUTPUT][TC_HIDDEN];
    float b2_raw[TC_OUTPUT];
    const unsigned char* p = datos + sizeof(dims);
    memcpy(W1_raw, p, sizeof(W1_raw)); p += sizeof(W1_raw);
    memcpy(b1_raw, p, sizeof(b1_raw)); p += sizeof(b1_raw);
    memcpy(W2_raw, p, sizeof(W2_raw)); p += sizeof(W2_raw);
    memcpy(b2_raw, p, sizeof(b2_raw));

    for (int i = 0; i < TC_HIDDEN; i++)
    {
        tc_weights.b1[i] = b1_raw[i];
        for (int j = 0; j < TC_INPUT; j++)
            tc_weights.W1[j][i] = W1_raw[i][j];
    }
    for (int i = 0; i < TC_OUTPUT; i++)
    {
        tc_weights.b2[i] = b2_raw[i];
        for (int j = 0; j < TC_HIDDEN; j++)
            tc_weights.W2[j][i] = W2_raw[i][j];
    }

    tc_initialized = 1;
    printf("info string Clasificador tactico cargado correctamente (%s)\n", nombre);
    return 1;
}

// Clasificador desde disco (TCFile)
int nnue_tc_init(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        printf("info string No se pudo cargar el clasificador tactico desde %s\n", path);
        return 0;
    }
    unsigned char buf[16384];       // el clasificador ocupa unos 2 KB
    const size_t bytes = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return nnue_tc_cargar(buf, bytes, path);
}

// Clasificador incluido en el ejecutable
int nnue_tc_init_interna(void)
{
    return nnue_tc_cargar(gilipol_tc_ini, (size_t)(gilipol_tc_fin - gilipol_tc_ini),
                          "interno, " TC_EMBEBIDA);
}

// ============================================================================
// CÁLCULO DE FEATURES
// ============================================================================

static void calculate_tactical_features(const Position* pos, int* features, int* count)
{
    int side = pos->side_to_move;
    int enemy = side ^ 1;

    // Generamos todos los ataques de ambos bandos de golpe
    Bitboard my_attacks = get_all_attacks(pos, side);
    Bitboard enemy_attacks = get_all_attacks(pos, enemy);

    Bitboard white_attacks = (side == WHITE) ? my_attacks : enemy_attacks;
    Bitboard black_attacks = (side == BLACK) ? my_attacks : enemy_attacks;

    int w_king = get_lsb_index(pos->pieces[WHITE][KING]);
    int b_king = get_lsb_index(pos->pieces[BLACK][KING]);

    // --- 1. Peón en 7ª puede coronar (773-774) ---
    // Blanco
    Bitboard wp7 = pos->pieces[WHITE][PAWN] & 0x00FF000000000000ULL; // Rank 7
    if ((wp7 << 8) & ~pos->all & ~black_attacks)
        features[(*count)++] = 773;

    // Negro
    Bitboard bp2 = pos->pieces[BLACK][PAWN] & 0x000000000000FF00ULL; // Rank 2
    if ((bp2 >> 8) & ~pos->all & ~white_attacks)
        features[(*count)++] = 774;

    // --- 2. Captura de piezas desprotegidas (775-778) ---
    // Piezas enemigas AND mis ataques AND NO defendidas por el enemigo
    Bitboard unprotected_enemies = pos->occupied[enemy] & my_attacks & ~enemy_attacks;
    if (unprotected_enemies)
    {
        if (unprotected_enemies & pos->pieces[enemy][PAWN])
            features[(*count)++] = 775;
        if (unprotected_enemies & (pos->pieces[enemy][KNIGHT] | pos->pieces[enemy][BISHOP]))
            features[(*count)++] = 776;
        if (unprotected_enemies & pos->pieces[enemy][ROOK])
            features[(*count)++] = 777;
        if (unprotected_enemies & pos->pieces[enemy][QUEEN])
            features[(*count)++] = 778;
    }

    // --- 3. Pieza pequeña captura grande (779) ---
    Bitboard pawn_attacks_bb = (side == WHITE) 
        ? (((pos->pieces[WHITE][PAWN] << 7) & 0x7F7F7F7F7F7F7F7FULL) | ((pos->pieces[WHITE][PAWN] << 9) & 0xFEFEFEFEFEFEFEFEULL))
        : (((pos->pieces[BLACK][PAWN] >> 9) & 0x7F7F7F7F7F7F7F7FULL) | ((pos->pieces[BLACK][PAWN] >> 7) & 0xFEFEFEFEFEFEFEFEULL));
        
    if (pawn_attacks_bb & (pos->occupied[enemy] ^ pos->pieces[enemy][PAWN]))
        features[(*count)++] = 779; // Peón ataca algo que no es peón
    else
    {
        Bitboard knight_attacks_bb = 0;
        Bitboard n = pos->pieces[side][KNIGHT];
        while (n)
            knight_attacks_bb |= get_knight_attacks(pop_lsb(&n));
        
        Bitboard bishop_attacks_bb = 0;
        Bitboard b = pos->pieces[side][BISHOP];
        while (b)
            bishop_attacks_bb |= get_bishop_attacks(pop_lsb(&b), pos->all);
        
        Bitboard rook_attacks_bb = 0;
        Bitboard r = pos->pieces[side][ROOK];
        while (r)
            rook_attacks_bb |= get_rook_attacks(pop_lsb(&r), pos->all);

        Bitboard queen_attacks_bb = 0;
        Bitboard q = pos->pieces[side][QUEEN];
        while (q)
        {
            int sq = pop_lsb(&q);
            queen_attacks_bb |= get_bishop_attacks(sq, pos->all) | get_rook_attacks(sq, pos->all);
        }

        if ((knight_attacks_bb | bishop_attacks_bb) & (pos->pieces[enemy][ROOK] | pos->pieces[enemy][QUEEN] | pos->pieces[enemy][KING]))
            features[(*count)++] = 779; // Menor ataca mayor
        else if (rook_attacks_bb & (pos->pieces[enemy][QUEEN] | pos->pieces[enemy][KING]))
            features[(*count)++] = 779; // Torre ataca Dama o Rey
        else if (queen_attacks_bb & pos->pieces[enemy][KING])
            features[(*count)++] = 779; // Reina ataca Rey
    }

    // --- 4. Pareja de alfiles (780-781) ---
    if (count_bits(pos->pieces[WHITE][BISHOP]) >= 2)
        features[(*count)++] = 780;
    if (count_bits(pos->pieces[BLACK][BISHOP]) >= 2)
        features[(*count)++] = 781;

    // --- 5. Jaque Burro (782-783) ---
    if (b_king != -1)
    {
        Bitboard black_majors = pos->pieces[BLACK][QUEEN] | pos->pieces[BLACK][ROOK];
        Bitboard candidate_sqs = get_knight_attacks(b_king) & ~black_attacks & ~pos->occupied[WHITE];
        Bitboard n = pos->pieces[WHITE][KNIGHT];
        while (n)
        {
            int knight_sq = pop_lsb(&n);
            Bitboard reachable = get_knight_attacks(knight_sq) & candidate_sqs;
            while (reachable)
            {
                int dest = pop_lsb(&reachable);
                if (get_knight_attacks(dest) & black_majors)
                {
                    features[(*count)++] = 782;
                    goto done_782;
                }
            }
        }
        done_782:;
    }

    if (w_king != -1)
    {
        Bitboard white_majors = pos->pieces[WHITE][QUEEN] | pos->pieces[WHITE][ROOK];
        Bitboard candidate_sqs = get_knight_attacks(w_king) & ~white_attacks & ~pos->occupied[BLACK];
        Bitboard n = pos->pieces[BLACK][KNIGHT];
        while (n)
        {
            int knight_sq = pop_lsb(&n);
            Bitboard reachable = get_knight_attacks(knight_sq) & candidate_sqs;
            while (reachable)
            {
                int dest = pop_lsb(&reachable);
                if (get_knight_attacks(dest) & white_majors)
                {
                    features[(*count)++] = 783;
                    goto done_783;
                }
            }
        }
        done_783:;
    }

    // --- 6. Peón corona con jaque (784-785) ---
    if (b_king != -1)
    {
        Bitboard wp7_promo = (pos->pieces[WHITE][PAWN] & 0x00FF000000000000ULL) << 8;
        if ((wp7_promo & ~pos->all) & white_attacks & get_king_attacks(b_king))
            features[(*count)++] = 784;
    }
    if (w_king != -1)
    {
        Bitboard bp2_promo = (pos->pieces[BLACK][PAWN] & 0x000000000000FF00ULL) >> 8;
        if ((bp2_promo & ~pos->all) & black_attacks & get_king_attacks(w_king))
            features[(*count)++] = 785;
    }
}

// ============================================================================
// INFERENCIA
// ============================================================================

// Refresca el acumulador desde cero (usado en parse_fen)
void nnue_refresh_accumulator(Position* pos)
{
    if (!nnue_initialized) return;

    // 1. Copiar biases
    for (int i = 0; i < HIDDEN1; i++)
        pos->accumulator.values[i] = nnue_weights.b1[i];

    // 2. Añadir features activas (Piezas)
    for (int c = WHITE; c <= BLACK; c++)
    {
        for (int p = PAWN; p <= KING; p++)
        {
            Bitboard bb = pos->pieces[c][p];
            int piece_idx = c * 6 + p;
            while (bb)
            {
                int sq = pop_lsb(&bb);
                int feature = piece_idx * 64 + sq;
                for (int i = 0; i < HIDDEN1; i++)
                    pos->accumulator.values[i] += nnue_weights.W1[feature][i];
            }
        }
    }

    // 3. Añadir features activas (Metadata)
    if (pos->side_to_move == WHITE)
        for (int i = 0; i < HIDDEN1; i++)
            pos->accumulator.values[i] += nnue_weights.W1[768][i];

    if (pos->castling_rights & WHITE_OO)
        for (int i = 0; i < HIDDEN1; i++)
            pos->accumulator.values[i] += nnue_weights.W1[769][i];
    if (pos->castling_rights & WHITE_OOO)
        for (int i = 0; i < HIDDEN1; i++)
            pos->accumulator.values[i] += nnue_weights.W1[770][i];
    if (pos->castling_rights & BLACK_OO)
        for (int i = 0; i < HIDDEN1; i++)
            pos->accumulator.values[i] += nnue_weights.W1[771][i];
    if (pos->castling_rights & BLACK_OOO)
        for (int i = 0; i < HIDDEN1; i++)
            pos->accumulator.values[i] += nnue_weights.W1[772][i];
}

// --- Actualizacion incremental del acumulador (make_move) ---------------------
//
// acc += W1[feature] o acc -= W1[feature]. Multiplicar por +-1 es exacto, asi que
// las tres variantes dan el mismo resultado bit a bit.

ATTR_AVX512 static void acum_avx512(float* acc, const float* w, int signo)
{
    if (signo > 0)
        for (int i = 0; i < HIDDEN1; i += 16)
            _mm512_storeu_ps(&acc[i], _mm512_add_ps(_mm512_loadu_ps(&acc[i]), _mm512_loadu_ps(&w[i])));
    else
        for (int i = 0; i < HIDDEN1; i += 16)
            _mm512_storeu_ps(&acc[i], _mm512_sub_ps(_mm512_loadu_ps(&acc[i]), _mm512_loadu_ps(&w[i])));
}

ATTR_AVX2 static void acum_avx2(float* acc, const float* w, int signo)
{
    if (signo > 0)
        for (int i = 0; i < HIDDEN1; i += 8)
            _mm256_storeu_ps(&acc[i], _mm256_add_ps(_mm256_loadu_ps(&acc[i]), _mm256_loadu_ps(&w[i])));
    else
        for (int i = 0; i < HIDDEN1; i += 8)
            _mm256_storeu_ps(&acc[i], _mm256_sub_ps(_mm256_loadu_ps(&acc[i]), _mm256_loadu_ps(&w[i])));
}

void nnue_acumular(float* acc, int feature, int signo)
{
    const float* w = nnue_weights.W1[feature];
    if (cpu_nivel == CPU_AVX512)
        acum_avx512(acc, w, signo);
    else if (cpu_nivel == CPU_AVX2)
        acum_avx2(acc, w, signo);
    else if (signo > 0)
        for (int i = 0; i < HIDDEN1; i++)
            acc[i] += w[i];
    else
        for (int i = 0; i < HIDDEN1; i++)
            acc[i] -= w[i];
}

// --- Pasada hacia delante --------------------------------------------------------
//
// Cada variante recibe el acumulador y las features tacticas activas, deja h4
// (post-relu, para el clasificador tactico) y devuelve la salida de la red sin
// escalar.
//
// Para que AVX-512 y AVX2 den exactamente el mismo numero:
//   - capas 1 a 4: cada neurona acumula sus entradas en el mismo orden (j creciente)
//     con FMA, sea cual sea el ancho del registro;
//   - salida: las dos usan la misma reduccion de 8 carriles (la de AVX2). Antes la
//     de AVX-512 sumaba en otro orden y un binario AVX2 podia jugar distinto.

ATTR_AVX512 static float forward_avx512(const float* acc, const int* feats, int n, float* h4)
{
    // Layer 1 (Sparse)
    float h1[HIDDEN1] __attribute__((aligned(64)));
    for (int i = 0; i < HIDDEN1; i += 16)
        _mm512_store_ps(&h1[i], _mm512_loadu_ps(&acc[i]));
    // Features tácticas
    for (int k = 0; k < n; k++)
    {
        int feat = feats[k];
        for (int i = 0; i < HIDDEN1; i += 16)
            _mm512_store_ps(&h1[i], _mm512_add_ps(_mm512_load_ps(&h1[i]), _mm512_loadu_ps(&nnue_weights.W1[feat][i])));
    }
    // ReLU
    __m512 zero512 = _mm512_setzero_ps();
    for (int i = 0; i < HIDDEN1; i += 16)
        _mm512_store_ps(&h1[i], _mm512_max_ps(_mm512_load_ps(&h1[i]), zero512));

    // Layer 2 (Dense): 128 outputs = 8 registros ZMM
    float h2[HIDDEN2] __attribute__((aligned(64)));
    __m512 acc0 = _mm512_loadu_ps(&nnue_weights.b2[0]);
    __m512 acc1 = _mm512_loadu_ps(&nnue_weights.b2[16]);
    __m512 acc2 = _mm512_loadu_ps(&nnue_weights.b2[32]);
    __m512 acc3 = _mm512_loadu_ps(&nnue_weights.b2[48]);
    __m512 acc4 = _mm512_loadu_ps(&nnue_weights.b2[64]);
    __m512 acc5 = _mm512_loadu_ps(&nnue_weights.b2[80]);
    __m512 acc6 = _mm512_loadu_ps(&nnue_weights.b2[96]);
    __m512 acc7 = _mm512_loadu_ps(&nnue_weights.b2[112]);
    for (int j = 0; j < HIDDEN1; j++)
    {
        float val = h1[j];
        if (val == 0.0f)
            continue; // Sparsity optimization
        __m512 v_in = _mm512_set1_ps(val);
        acc0 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][0]), acc0);
        acc1 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][16]), acc1);
        acc2 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][32]), acc2);
        acc3 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][48]), acc3);
        acc4 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][64]), acc4);
        acc5 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][80]), acc5);
        acc6 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][96]), acc6);
        acc7 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W2[j][112]), acc7);
    }
    _mm512_store_ps(&h2[0],   _mm512_max_ps(acc0, zero512));
    _mm512_store_ps(&h2[16],  _mm512_max_ps(acc1, zero512));
    _mm512_store_ps(&h2[32],  _mm512_max_ps(acc2, zero512));
    _mm512_store_ps(&h2[48],  _mm512_max_ps(acc3, zero512));
    _mm512_store_ps(&h2[64],  _mm512_max_ps(acc4, zero512));
    _mm512_store_ps(&h2[80],  _mm512_max_ps(acc5, zero512));
    _mm512_store_ps(&h2[96],  _mm512_max_ps(acc6, zero512));
    _mm512_store_ps(&h2[112], _mm512_max_ps(acc7, zero512));

    // Layer 3 (Dense)
    float h3[HIDDEN3] __attribute__((aligned(64)));
    __m512 acc0_l3 = _mm512_loadu_ps(&nnue_weights.b3[0]);
    __m512 acc1_l3 = _mm512_loadu_ps(&nnue_weights.b3[16]);
    __m512 acc2_l3 = _mm512_loadu_ps(&nnue_weights.b3[32]);
    __m512 acc3_l3 = _mm512_loadu_ps(&nnue_weights.b3[48]);
    for (int j = 0; j < HIDDEN2; j++)
    {
        float val = h2[j];
        if (val == 0.0f)
            continue;
        __m512 v_in = _mm512_set1_ps(val);
        acc0_l3 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W3[j][0]), acc0_l3);
        acc1_l3 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W3[j][16]), acc1_l3);
        acc2_l3 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W3[j][32]), acc2_l3);
        acc3_l3 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W3[j][48]), acc3_l3);
    }
    _mm512_store_ps(&h3[0],  _mm512_max_ps(acc0_l3, zero512));
    _mm512_store_ps(&h3[16], _mm512_max_ps(acc1_l3, zero512));
    _mm512_store_ps(&h3[32], _mm512_max_ps(acc2_l3, zero512));
    _mm512_store_ps(&h3[48], _mm512_max_ps(acc3_l3, zero512));

    // Layer 4 (Dense)
    __m512 acc0_l4 = _mm512_loadu_ps(&nnue_weights.b4[0]);
    __m512 acc1_l4 = _mm512_loadu_ps(&nnue_weights.b4[16]);
    for (int j = 0; j < HIDDEN3; j++)
    {
        float val = h3[j];
        if (val == 0.0f)
            continue;
        __m512 v_in = _mm512_set1_ps(val);
        acc0_l4 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W4[j][0]), acc0_l4);
        acc1_l4 = _mm512_fmadd_ps(v_in, _mm512_loadu_ps(&nnue_weights.W4[j][16]), acc1_l4);
    }
    _mm512_storeu_ps(&h4[0],  _mm512_max_ps(acc0_l4, zero512));
    _mm512_storeu_ps(&h4[16], _mm512_max_ps(acc1_l4, zero512));

    // Output: la misma reduccion de 8 carriles que AVX2
    float output = nnue_weights.b_out[0];
    __m256 sum256 = _mm256_setzero_ps();
    for (int i = 0; i < HIDDEN4; i += 8)
        sum256 = _mm256_fmadd_ps(_mm256_loadu_ps(&h4[i]), _mm256_loadu_ps(&nnue_weights.W_out[i][0]), sum256);
    __m128 low = _mm256_castps256_ps128(sum256);
    __m128 high = _mm256_extractf128_ps(sum256, 1);
    __m128 sum128 = _mm_add_ps(low, high);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    output += _mm_cvtss_f32(sum128);
    return output;
}

ATTR_AVX2 static float forward_avx2(const float* acc, const int* feats, int n, float* h4)
{
    // Layer 1 (Sparse)
    float h1[HIDDEN1] __attribute__((aligned(64)));
    for (int i = 0; i < HIDDEN1; i += 8)
        _mm256_store_ps(&h1[i], _mm256_loadu_ps(&acc[i]));
    // Features tácticas
    for (int k = 0; k < n; k++)
    {
        int feat = feats[k];
        for (int i = 0; i < HIDDEN1; i += 8)
            _mm256_store_ps(&h1[i], _mm256_add_ps(_mm256_load_ps(&h1[i]), _mm256_loadu_ps(&nnue_weights.W1[feat][i])));
    }
    // ReLU
    __m256 zero256 = _mm256_setzero_ps();
    for (int i = 0; i < HIDDEN1; i += 8)
        _mm256_store_ps(&h1[i], _mm256_max_ps(_mm256_load_ps(&h1[i]), zero256));

    // Layer 2 (Dense): cache blocking, 2 pasadas de 64 outputs
    float h2[HIDDEN2] __attribute__((aligned(64)));
    for (int base = 0; base < HIDDEN2; base += 64)
    {
        __m256 acc0 = _mm256_loadu_ps(&nnue_weights.b2[base + 0]);
        __m256 acc1 = _mm256_loadu_ps(&nnue_weights.b2[base + 8]);
        __m256 acc2 = _mm256_loadu_ps(&nnue_weights.b2[base + 16]);
        __m256 acc3 = _mm256_loadu_ps(&nnue_weights.b2[base + 24]);
        __m256 acc4 = _mm256_loadu_ps(&nnue_weights.b2[base + 32]);
        __m256 acc5 = _mm256_loadu_ps(&nnue_weights.b2[base + 40]);
        __m256 acc6 = _mm256_loadu_ps(&nnue_weights.b2[base + 48]);
        __m256 acc7 = _mm256_loadu_ps(&nnue_weights.b2[base + 56]);
        for (int j = 0; j < HIDDEN1; j++)
        {
            float val = h1[j];
            if (val == 0.0f)
                continue;
            __m256 v_in = _mm256_set1_ps(val);
            const float* w = &nnue_weights.W2[j][base];
            acc0 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 0), acc0);
            acc1 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 8), acc1);
            acc2 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 16), acc2);
            acc3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 24), acc3);
            acc4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 32), acc4);
            acc5 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 40), acc5);
            acc6 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 48), acc6);
            acc7 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(w + 56), acc7);
        }
        _mm256_store_ps(&h2[base + 0],  _mm256_max_ps(acc0, zero256));
        _mm256_store_ps(&h2[base + 8],  _mm256_max_ps(acc1, zero256));
        _mm256_store_ps(&h2[base + 16], _mm256_max_ps(acc2, zero256));
        _mm256_store_ps(&h2[base + 24], _mm256_max_ps(acc3, zero256));
        _mm256_store_ps(&h2[base + 32], _mm256_max_ps(acc4, zero256));
        _mm256_store_ps(&h2[base + 40], _mm256_max_ps(acc5, zero256));
        _mm256_store_ps(&h2[base + 48], _mm256_max_ps(acc6, zero256));
        _mm256_store_ps(&h2[base + 56], _mm256_max_ps(acc7, zero256));
    }

    // Layer 3 (Dense)
    float h3[HIDDEN3] __attribute__((aligned(64)));
    __m256 acc0_l3 = _mm256_loadu_ps(&nnue_weights.b3[0]);
    __m256 acc1_l3 = _mm256_loadu_ps(&nnue_weights.b3[8]);
    __m256 acc2_l3 = _mm256_loadu_ps(&nnue_weights.b3[16]);
    __m256 acc3_l3 = _mm256_loadu_ps(&nnue_weights.b3[24]);
    __m256 acc4_l3 = _mm256_loadu_ps(&nnue_weights.b3[32]);
    __m256 acc5_l3 = _mm256_loadu_ps(&nnue_weights.b3[40]);
    __m256 acc6_l3 = _mm256_loadu_ps(&nnue_weights.b3[48]);
    __m256 acc7_l3 = _mm256_loadu_ps(&nnue_weights.b3[56]);
    for (int j = 0; j < HIDDEN2; j++)
    {
        float val = h2[j];
        if (val == 0.0f)
            continue;
        __m256 v_in = _mm256_set1_ps(val);
        acc0_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][0]), acc0_l3);
        acc1_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][8]), acc1_l3);
        acc2_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][16]), acc2_l3);
        acc3_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][24]), acc3_l3);
        acc4_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][32]), acc4_l3);
        acc5_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][40]), acc5_l3);
        acc6_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][48]), acc6_l3);
        acc7_l3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W3[j][56]), acc7_l3);
    }
    _mm256_store_ps(&h3[0],  _mm256_max_ps(acc0_l3, zero256));
    _mm256_store_ps(&h3[8],  _mm256_max_ps(acc1_l3, zero256));
    _mm256_store_ps(&h3[16], _mm256_max_ps(acc2_l3, zero256));
    _mm256_store_ps(&h3[24], _mm256_max_ps(acc3_l3, zero256));
    _mm256_store_ps(&h3[32], _mm256_max_ps(acc4_l3, zero256));
    _mm256_store_ps(&h3[40], _mm256_max_ps(acc5_l3, zero256));
    _mm256_store_ps(&h3[48], _mm256_max_ps(acc6_l3, zero256));
    _mm256_store_ps(&h3[56], _mm256_max_ps(acc7_l3, zero256));

    // Layer 4 (Dense)
    __m256 acc0_l4 = _mm256_loadu_ps(&nnue_weights.b4[0]);
    __m256 acc1_l4 = _mm256_loadu_ps(&nnue_weights.b4[8]);
    __m256 acc2_l4 = _mm256_loadu_ps(&nnue_weights.b4[16]);
    __m256 acc3_l4 = _mm256_loadu_ps(&nnue_weights.b4[24]);
    for (int j = 0; j < HIDDEN3; j++)
    {
        float val = h3[j];
        if (val == 0.0f)
            continue;
        __m256 v_in = _mm256_set1_ps(val);
        acc0_l4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W4[j][0]), acc0_l4);
        acc1_l4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W4[j][8]), acc1_l4);
        acc2_l4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W4[j][16]), acc2_l4);
        acc3_l4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W4[j][24]), acc3_l4);
    }
    _mm256_storeu_ps(&h4[0],  _mm256_max_ps(acc0_l4, zero256));
    _mm256_storeu_ps(&h4[8],  _mm256_max_ps(acc1_l4, zero256));
    _mm256_storeu_ps(&h4[16], _mm256_max_ps(acc2_l4, zero256));
    _mm256_storeu_ps(&h4[24], _mm256_max_ps(acc3_l4, zero256));

    // Output: reduccion horizontal YMM -> float
    float output = nnue_weights.b_out[0];
    __m256 sum256 = _mm256_setzero_ps();
    for (int i = 0; i < HIDDEN4; i += 8)
        sum256 = _mm256_fmadd_ps(_mm256_loadu_ps(&h4[i]), _mm256_loadu_ps(&nnue_weights.W_out[i][0]), sum256);
    __m128 low = _mm256_castps256_ps128(sum256);
    __m128 high = _mm256_extractf128_ps(sum256, 1);
    __m128 sum128 = _mm_add_ps(low, high);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    output += _mm_cvtss_f32(sum128);
    return output;
}

// Variante generica (cualquier x86-64): el mismo orden de operaciones que las SIMD,
// pero sin FMA (multiplicacion y suma redondean por separado). El compilador la
// vectoriza con SSE2.
ATTR_GENERICA static float forward_generica(const float* acc, const int* feats, int n, float* h4)
{
    // Layer 1 (Sparse)
    float h1[HIDDEN1];
    memcpy(h1, acc, HIDDEN1 * sizeof(float));
    for (int k = 0; k < n; k++)
    {
        const float* w = nnue_weights.W1[feats[k]];
        for (int i = 0; i < HIDDEN1; i++)
            h1[i] += w[i];
    }
    for (int i = 0; i < HIDDEN1; i++)
        h1[i] = relu(h1[i]);

    // Layer 2 (Dense)
    float h2[HIDDEN2];
    memcpy(h2, nnue_weights.b2, sizeof(h2));
    for (int j = 0; j < HIDDEN1; j++)
    {
        const float val = h1[j];
        if (val == 0.0f)
            continue;
        const float* w = nnue_weights.W2[j];
        for (int i = 0; i < HIDDEN2; i++)
            h2[i] += val * w[i];
    }
    for (int i = 0; i < HIDDEN2; i++)
        h2[i] = relu(h2[i]);

    // Layer 3 (Dense)
    float h3[HIDDEN3];
    memcpy(h3, nnue_weights.b3, sizeof(h3));
    for (int j = 0; j < HIDDEN2; j++)
    {
        const float val = h2[j];
        if (val == 0.0f)
            continue;
        const float* w = nnue_weights.W3[j];
        for (int i = 0; i < HIDDEN3; i++)
            h3[i] += val * w[i];
    }
    for (int i = 0; i < HIDDEN3; i++)
        h3[i] = relu(h3[i]);

    // Layer 4 (Dense)
    memcpy(h4, nnue_weights.b4, HIDDEN4 * sizeof(float));
    for (int j = 0; j < HIDDEN3; j++)
    {
        const float val = h3[j];
        if (val == 0.0f)
            continue;
        const float* w = nnue_weights.W4[j];
        for (int i = 0; i < HIDDEN4; i++)
            h4[i] += val * w[i];
    }
    for (int i = 0; i < HIDDEN4; i++)
        h4[i] = relu(h4[i]);

    // Output: 8 carriles y la misma reduccion en arbol que AVX2
    float s[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < HIDDEN4; i += 8)
        for (int k = 0; k < 8; k++)
            s[k] += h4[i + k] * nnue_weights.W_out[i + k][0];
    const float t0 = s[0] + s[4], t1 = s[1] + s[5], t2 = s[2] + s[6], t3 = s[3] + s[7];
    return nnue_weights.b_out[0] + ((t0 + t1) + (t2 + t3));
}

int nnue_evaluate(const Position* pos)
{
    return nnue_evaluate_tc(pos, 1);
}

int nnue_evaluate_tc(const Position* pos, int calcular_tc)
{
    if (!nnue_initialized)
        return 0; // Retornamos 0 si no hay red

    int active_features[INPUT_SIZE];
    int num_active = 0;

    // Features Tácticas (773-785) - Se calculan al vuelo
    calculate_tactical_features(pos, active_features, &num_active);

    // --- Forward Pass ---
    float h4[HIDDEN4] __attribute__((aligned(64)));
    float output;
    if (cpu_nivel == CPU_AVX512)
        output = forward_avx512(pos->accumulator.values, active_features, num_active, h4);
    else if (cpu_nivel == CPU_AVX2)
        output = forward_avx2(pos->accumulator.values, active_features, num_active, h4);
    else
        output = forward_generica(pos->accumulator.values, active_features, num_active, h4);

    // Clasificador tactico (usa h4 post-relu recien calculado).
    //
    // Solo si el llamante lo va a mirar: son tres expf y cuestan el 20 % de la
    // evaluacion. Ver QS_USA_TC en common.h.
    if (USA_TC && tc_initialized && calcular_tc)
        tc_compute(h4);

    // Escalar y convertir a int. Se acota en float ANTES de convertir: (int) de un NaN
    // o de un valor fuera de rango es UB, y con negras se negaba INT_MIN (B25).
    float escalado = output * 100.0f;
    if (!(escalado == escalado))
        escalado = 0.0f;
    if (escalado > (float)EVAL_MAX)
        escalado = (float)EVAL_MAX;
    if (escalado < -(float)EVAL_MAX)
        escalado = -(float)EVAL_MAX;
    int score = (int)escalado;

    // Perspectiva: NNUE evalúa para blancas. Si juegan negras, invertir.
    if (pos->side_to_move == BLACK)
        score = -score;

    // Asegurar que no sobrepase límites
    score = clamp_int(score, -EVAL_MAX, EVAL_MAX);

    return score;
}
