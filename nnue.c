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
static int nnue_initialized = 0;

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
    
    // Piezas deslizantes (Alfiles, Torres, Damas) usando BMI2 PEXT
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
 * nnue_init (carga de pesos)
 * 
 */
void nnue_init(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        printf("Info: No se pudo cargar NNUE desde %s (evaluación deshabilitada)\n", path);
        nnue_initialized = 0;
        return;
    }

    size_t total_params =
        (INPUT_SIZE * HIDDEN1) + HIDDEN1 +
        (HIDDEN1 * HIDDEN2) + HIDDEN2 +
        (HIDDEN2 * HIDDEN3) + HIDDEN3 +
        (HIDDEN3 * HIDDEN4) + HIDDEN4 +
        (HIDDEN4 * OUTPUT_SIZE) + OUTPUT_SIZE;

    float* weights = (float*)malloc(total_params * sizeof(float));
    if (!weights)
    {
        printf("Error: Memoria insuficiente para cargar NNUE\n");
        fclose(f);
        return;
    }

    size_t read = fread(weights, sizeof(float), total_params, f);
    fclose(f);

    if (read != total_params)
    {
        printf("Error: Archivo NNUE corrupto o tamaño incorrecto\n");
        free(weights);
        return;
    }

    // Distribuir y transponer pesos (PyTorch exporta [out][in], nosotros queremos [in][out])
    float* ptr = weights;

    // Layer 1: [512][786] -> [786][512]
    float* w1 = ptr; ptr += INPUT_SIZE * HIDDEN1;
    float* b1 = ptr; ptr += HIDDEN1;
    for (int i = 0; i < HIDDEN1; i++)
    {
        nnue_weights.b1[i] = b1[i];
        for (int j = 0; j < INPUT_SIZE; j++)
            nnue_weights.W1[j][i] = w1[i * INPUT_SIZE + j];
    }

    // Layer 2: [128][512] -> [512][128]
    float* w2 = ptr; ptr += HIDDEN1 * HIDDEN2;
    float* b2 = ptr; ptr += HIDDEN2;
    for (int i = 0; i < HIDDEN2; i++)
    {
        nnue_weights.b2[i] = b2[i];
        for (int j = 0; j < HIDDEN1; j++)
            nnue_weights.W2[j][i] = w2[i * HIDDEN1 + j];
    }

    // Layer 3: [64][128] -> [128][64]
    float* w3 = ptr; ptr += HIDDEN2 * HIDDEN3;
    float* b3 = ptr; ptr += HIDDEN3;
    for (int i = 0; i < HIDDEN3; i++)
    {
        nnue_weights.b3[i] = b3[i];
        for (int j = 0; j < HIDDEN2; j++)
            nnue_weights.W3[j][i] = w3[i * HIDDEN2 + j];
    }

    // Layer 4: [32][64] -> [64][32]
    float* w4 = ptr; ptr += HIDDEN3 * HIDDEN4;
    float* b4 = ptr; ptr += HIDDEN4;
    for (int i = 0; i < HIDDEN4; i++)
    {
        nnue_weights.b4[i] = b4[i];
        for (int j = 0; j < HIDDEN3; j++)
            nnue_weights.W4[j][i] = w4[i * HIDDEN3 + j];
    }

    // Output: [1][32] -> [32][1]
    float* w_out = ptr; ptr += HIDDEN4 * OUTPUT_SIZE;
    float* b_out = ptr;
    nnue_weights.b_out[0] = b_out[0];
    for (int i = 0; i < HIDDEN4; i++)
        nnue_weights.W_out[i][0] = w_out[i];

    free(weights);
    nnue_initialized = 1;
    printf("Info: NNUE cargada correctamente (%s)\n", path);
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
        Bitboard target_sqs = get_knight_attacks(b_king) & ~black_attacks & ~pos->occupied[WHITE];
        Bitboard w_knight_attacks = 0;
        Bitboard n = pos->pieces[WHITE][KNIGHT];
        while (n)
            w_knight_attacks |= get_knight_attacks(pop_lsb(&n));
        
        if (target_sqs & w_knight_attacks)
            features[(*count)++] = 782;
    }
    
    if (w_king != -1)
    {
        Bitboard target_sqs = get_knight_attacks(w_king) & ~white_attacks & ~pos->occupied[BLACK];
        Bitboard b_knight_attacks = 0;
        Bitboard n = pos->pieces[BLACK][KNIGHT];
        while (n)
            b_knight_attacks |= get_knight_attacks(pop_lsb(&n));
        
        if (target_sqs & b_knight_attacks)
            features[(*count)++] = 783;
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

int nnue_evaluate(const Position* pos)
{
    if (!nnue_initialized)
        return 0; // Retornamos 0 si no hay red

    // Macro de seguridad para FMA en AVX2 (por si la CPU es antigua)
    #if defined(__AVX2__) && !defined(__FMA__)
        #define _mm256_fmadd_ps(a, b, c) _mm256_add_ps(_mm256_mul_ps(a, b), c)
    #endif

    int active_features[INPUT_SIZE];
    int num_active = 0;

    // Features Tácticas (773-785) - Se calculan al vuelo
    calculate_tactical_features(pos, active_features, &num_active);

    // --- Forward Pass ---
    
    // Layer 1 (Sparse)
    float h1[HIDDEN1] __attribute__((aligned(64)));

    #if defined(__AVX512F__)
        // Copia optimizada AVX512
        for (int i = 0; i < HIDDEN1; i += 16)
        {
            _mm512_store_ps(&h1[i], _mm512_loadu_ps(&pos->accumulator.values[i]));
        }
        // Features tácticas
        for (int k = 0; k < num_active; k++)
        {
            int feat = active_features[k];
            for (int i = 0; i < HIDDEN1; i += 16)
            {
                _mm512_store_ps(&h1[i], _mm512_add_ps(_mm512_load_ps(&h1[i]), _mm512_loadu_ps(&nnue_weights.W1[feat][i])));
            }
        }
        // ReLU
        __m512 zero512 = _mm512_setzero_ps();
        for (int i = 0; i < HIDDEN1; i += 16)
        {
            _mm512_store_ps(&h1[i], _mm512_max_ps(_mm512_load_ps(&h1[i]), zero512));
        }
    #elif defined(__AVX2__)
        // Copia optimizada AVX2
        for (int i = 0; i < HIDDEN1; i += 8)
        {
            _mm256_store_ps(&h1[i], _mm256_loadu_ps(&pos->accumulator.values[i]));
        }
        // Features tácticas
        for (int k = 0; k < num_active; k++)
        {
            int feat = active_features[k];
            for (int i = 0; i < HIDDEN1; i += 8)
            {
                _mm256_store_ps(&h1[i], _mm256_add_ps(_mm256_load_ps(&h1[i]), _mm256_loadu_ps(&nnue_weights.W1[feat][i])));
            }
        }
        // ReLU
        __m256 zero256 = _mm256_setzero_ps();
        for (int i = 0; i < HIDDEN1; i += 8)
        {
            _mm256_store_ps(&h1[i], _mm256_max_ps(_mm256_load_ps(&h1[i]), zero256));
        }
    #else
        // Scalar Fallback
        memcpy(h1, pos->accumulator.values, HIDDEN1 * sizeof(float));
        for (int k = 0; k < num_active; k++)
        {
            int feat = active_features[k];
            for (int i = 0; i < HIDDEN1; i++)
                h1[i] += nnue_weights.W1[feat][i];
        }
        for (int i = 0; i < HIDDEN1; i++)
            h1[i] = relu(h1[i]);
    #endif

    // Layer 2 (Dense)
    float h2[HIDDEN2] __attribute__((aligned(64)));

    #if defined(__AVX512F__)
        // AVX512: 128 outputs = 8 registros ZMM
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
        __m512 zero_l2 = _mm512_setzero_ps();
        _mm512_store_ps(&h2[0], _mm512_max_ps(acc0, zero_l2));
        _mm512_store_ps(&h2[16], _mm512_max_ps(acc1, zero_l2));
        _mm512_store_ps(&h2[32], _mm512_max_ps(acc2, zero_l2));
        _mm512_store_ps(&h2[48], _mm512_max_ps(acc3, zero_l2));
        _mm512_store_ps(&h2[64], _mm512_max_ps(acc4, zero_l2));
        _mm512_store_ps(&h2[80], _mm512_max_ps(acc5, zero_l2));
        _mm512_store_ps(&h2[96], _mm512_max_ps(acc6, zero_l2));
        _mm512_store_ps(&h2[112], _mm512_max_ps(acc7, zero_l2));

    #elif defined(__AVX2__)
        // AVX2: Cache Blocking (2 pasadas de 64 outputs)
        __m256 zero_l2 = _mm256_setzero_ps();

        // Pasada 1: Outputs 0..63
        __m256 acc0 = _mm256_loadu_ps(&nnue_weights.b2[0]);
        __m256 acc1 = _mm256_loadu_ps(&nnue_weights.b2[8]);
        __m256 acc2 = _mm256_loadu_ps(&nnue_weights.b2[16]);
        __m256 acc3 = _mm256_loadu_ps(&nnue_weights.b2[24]);
        __m256 acc4 = _mm256_loadu_ps(&nnue_weights.b2[32]);
        __m256 acc5 = _mm256_loadu_ps(&nnue_weights.b2[40]);
        __m256 acc6 = _mm256_loadu_ps(&nnue_weights.b2[48]);
        __m256 acc7 = _mm256_loadu_ps(&nnue_weights.b2[56]);

        for (int j = 0; j < HIDDEN1; j++)
        {
            float val = h1[j];
            if (val == 0.0f)
                continue;
            __m256 v_in = _mm256_set1_ps(val);
            acc0 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][0]), acc0);
            acc1 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][8]), acc1);
            acc2 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][16]), acc2);
            acc3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][24]), acc3);
            acc4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][32]), acc4);
            acc5 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][40]), acc5);
            acc6 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][48]), acc6);
            acc7 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][56]), acc7);
        }
        _mm256_store_ps(&h2[0], _mm256_max_ps(acc0, zero_l2));
        _mm256_store_ps(&h2[8], _mm256_max_ps(acc1, zero_l2));
        _mm256_store_ps(&h2[16], _mm256_max_ps(acc2, zero_l2));
        _mm256_store_ps(&h2[24], _mm256_max_ps(acc3, zero_l2));
        _mm256_store_ps(&h2[32], _mm256_max_ps(acc4, zero_l2));
        _mm256_store_ps(&h2[40], _mm256_max_ps(acc5, zero_l2));
        _mm256_store_ps(&h2[48], _mm256_max_ps(acc6, zero_l2));
        _mm256_store_ps(&h2[56], _mm256_max_ps(acc7, zero_l2));

        // Pasada 2: Outputs 64..127
        acc0 = _mm256_loadu_ps(&nnue_weights.b2[64]);
        acc1 = _mm256_loadu_ps(&nnue_weights.b2[72]);
        acc2 = _mm256_loadu_ps(&nnue_weights.b2[80]);
        acc3 = _mm256_loadu_ps(&nnue_weights.b2[88]);
        acc4 = _mm256_loadu_ps(&nnue_weights.b2[96]);
        acc5 = _mm256_loadu_ps(&nnue_weights.b2[104]);
        acc6 = _mm256_loadu_ps(&nnue_weights.b2[112]);
        acc7 = _mm256_loadu_ps(&nnue_weights.b2[120]);

        for (int j = 0; j < HIDDEN1; j++)
        {
            float val = h1[j];
            if (val == 0.0f)
                continue;
            __m256 v_in = _mm256_set1_ps(val);
            acc0 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][64]), acc0);
            acc1 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][72]), acc1);
            acc2 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][80]), acc2);
            acc3 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][88]), acc3);
            acc4 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][96]), acc4);
            acc5 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][104]), acc5);
            acc6 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][112]), acc6);
            acc7 = _mm256_fmadd_ps(v_in, _mm256_loadu_ps(&nnue_weights.W2[j][120]), acc7);
        }
        _mm256_store_ps(&h2[64], _mm256_max_ps(acc0, zero_l2));
        _mm256_store_ps(&h2[72], _mm256_max_ps(acc1, zero_l2));
        _mm256_store_ps(&h2[80], _mm256_max_ps(acc2, zero_l2));
        _mm256_store_ps(&h2[88], _mm256_max_ps(acc3, zero_l2));
        _mm256_store_ps(&h2[96], _mm256_max_ps(acc4, zero_l2));
        _mm256_store_ps(&h2[104], _mm256_max_ps(acc5, zero_l2));
        _mm256_store_ps(&h2[112], _mm256_max_ps(acc6, zero_l2));
        _mm256_store_ps(&h2[120], _mm256_max_ps(acc7, zero_l2));

    #else
        for (int i = 0; i < HIDDEN2; i++)
        {
            float sum = nnue_weights.b2[i];
            for (int j = 0; j < HIDDEN1; j++)
                sum += h1[j] * nnue_weights.W2[j][i];
            h2[i] = relu(sum);
        }
    #endif

    // Layer 3 (Dense)
    float h3[HIDDEN3] __attribute__((aligned(64)));

    #if defined(__AVX512F__)
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
        __m512 zero_l3 = _mm512_setzero_ps();
        _mm512_store_ps(&h3[0], _mm512_max_ps(acc0_l3, zero_l3));
        _mm512_store_ps(&h3[16], _mm512_max_ps(acc1_l3, zero_l3));
        _mm512_store_ps(&h3[32], _mm512_max_ps(acc2_l3, zero_l3));
        _mm512_store_ps(&h3[48], _mm512_max_ps(acc3_l3, zero_l3));

    #elif defined(__AVX2__)
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
        __m256 zero_l3 = _mm256_setzero_ps();
        _mm256_store_ps(&h3[0], _mm256_max_ps(acc0_l3, zero_l3));
        _mm256_store_ps(&h3[8], _mm256_max_ps(acc1_l3, zero_l3));
        _mm256_store_ps(&h3[16], _mm256_max_ps(acc2_l3, zero_l3));
        _mm256_store_ps(&h3[24], _mm256_max_ps(acc3_l3, zero_l3));
        _mm256_store_ps(&h3[32], _mm256_max_ps(acc4_l3, zero_l3));
        _mm256_store_ps(&h3[40], _mm256_max_ps(acc5_l3, zero_l3));
        _mm256_store_ps(&h3[48], _mm256_max_ps(acc6_l3, zero_l3));
        _mm256_store_ps(&h3[56], _mm256_max_ps(acc7_l3, zero_l3));

    #else
        for (int i = 0; i < HIDDEN3; i++)
        {
            float sum = nnue_weights.b3[i];
            for (int j = 0; j < HIDDEN2; j++)
                sum += h2[j] * nnue_weights.W3[j][i];
            h3[i] = relu(sum);
        }
    #endif

    // Layer 4 (Dense)
    float h4[HIDDEN4] __attribute__((aligned(64)));

    #if defined(__AVX512F__)
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
        __m512 zero_l4 = _mm512_setzero_ps();
        _mm512_store_ps(&h4[0], _mm512_max_ps(acc0_l4, zero_l4));
        _mm512_store_ps(&h4[16], _mm512_max_ps(acc1_l4, zero_l4));

    #elif defined(__AVX2__)
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
        __m256 zero_l4 = _mm256_setzero_ps();
        _mm256_store_ps(&h4[0], _mm256_max_ps(acc0_l4, zero_l4));
        _mm256_store_ps(&h4[8], _mm256_max_ps(acc1_l4, zero_l4));
        _mm256_store_ps(&h4[16], _mm256_max_ps(acc2_l4, zero_l4));
        _mm256_store_ps(&h4[24], _mm256_max_ps(acc3_l4, zero_l4));

    #else
        for (int i = 0; i < HIDDEN4; i++)
        {
            float sum = nnue_weights.b4[i];
            for (int j = 0; j < HIDDEN3; j++)
                sum += h3[j] * nnue_weights.W4[j][i];
            h4[i] = relu(sum);
        }
    #endif

    // Output
    float output = nnue_weights.b_out[0];
    
    #if defined(__AVX512F__)
        __m512 sum512 = _mm512_setzero_ps();
        for (int i = 0; i < HIDDEN4; i += 16)
        {
            sum512 = _mm512_fmadd_ps(_mm512_load_ps(&h4[i]), _mm512_loadu_ps(&nnue_weights.W_out[i][0]), sum512);
        }
        // Reducción 512 -> 256 -> Scalar
        __m256 low = _mm512_castps512_ps256(sum512);
        __m256 high = _mm512_extractf32x8_ps(sum512, 1);
        __m256 sum256 = _mm256_add_ps(low, high);
        
        __m128 low128 = _mm256_castps256_ps128(sum256);
        __m128 high128 = _mm256_extractf128_ps(sum256, 1);
        __m128 sum128 = _mm_add_ps(low128, high128);
        sum128 = _mm_hadd_ps(sum128, sum128);
        sum128 = _mm_hadd_ps(sum128, sum128);
        output += _mm_cvtss_f32(sum128);

    #elif defined(__AVX2__)
        __m256 sum256 = _mm256_setzero_ps();
        for (int i = 0; i < HIDDEN4; i += 8)
        {
            sum256 = _mm256_fmadd_ps(_mm256_load_ps(&h4[i]), _mm256_loadu_ps(&nnue_weights.W_out[i][0]), sum256);
        }
        // Reducción horizontal YMM -> float
        __m128 low = _mm256_castps256_ps128(sum256);
        __m128 high = _mm256_extractf128_ps(sum256, 1);
        __m128 sum128 = _mm_add_ps(low, high);
        sum128 = _mm_hadd_ps(sum128, sum128);
        sum128 = _mm_hadd_ps(sum128, sum128);
        output += _mm_cvtss_f32(sum128);

    #else
        for (int i = 0; i < HIDDEN4; i++)
        {
            output += h4[i] * nnue_weights.W_out[i][0];
        }
    #endif

    // Escalar y convertir a int
    int score = (int)(output * 100.0f);

    // Perspectiva: NNUE evalúa para blancas. Si juegan negras, invertir.
    if (pos->side_to_move == BLACK)
        score = -score;

    // Ajustar con respecto a la regla de 50 jugadas (aproximo hacia la mitad de la eval estática)
    score = score * (200 - pos->half_move_clock) / 200;

    // Asegurar que no sobrepase límites
    score = clamp_int(score, -TB_MAX+1, TB_MAX-1);

    return score;
}
