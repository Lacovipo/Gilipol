/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

#include "common.h"

// --- Variables Globales para Magic Bitboards ---

Bitboard pawn_attacks[2][64]; // [Color][Casilla]
Bitboard knight_attacks[64];
Bitboard king_attacks[64];

// --- PEXT Bitboards (BMI2) ---
Bitboard rook_masks[64];
Bitboard bishop_masks[64];
Bitboard rook_table[64][4096];   // 12 bits max para torres = 4096 entradas
Bitboard bishop_table[64][512];  // 9 bits max para alfiles = 512 entradas

// --- Generación de Ataques "Lentos" (para inicializar) ---

Bitboard mask_bishop_attacks(int sq)
{
    Bitboard attacks = 0ULL;
    int r, f;
    int tr = sq / 8;
    int tf = sq % 8;
    
    for (r = tr + 1, f = tf + 1; r < 7 && f < 7; r++, f++)
        SET_BIT(attacks, r * 8 + f);
    for (r = tr - 1, f = tf + 1; r > 0 && f < 7; r--, f++)
        SET_BIT(attacks, r * 8 + f);
    for (r = tr + 1, f = tf - 1; r < 7 && f > 0; r++, f--)
        SET_BIT(attacks, r * 8 + f);
    for (r = tr - 1, f = tf - 1; r > 0 && f > 0; r--, f--)
        SET_BIT(attacks, r * 8 + f);
    
    return attacks;
}

Bitboard mask_rook_attacks(int sq)
{
    Bitboard attacks = 0ULL;
    int r, f;
    int tr = sq / 8;
    int tf = sq % 8;
    
    for (r = tr + 1; r < 7; r++)
        SET_BIT(attacks, r * 8 + tf);
    for (r = tr - 1; r > 0; r--)
        SET_BIT(attacks, r * 8 + tf);
    for (f = tf + 1; f < 7; f++)
        SET_BIT(attacks, tr * 8 + f);
    for (f = tf - 1; f > 0; f--)
        SET_BIT(attacks, tr * 8 + f);
    
    return attacks;
}

Bitboard bishop_attacks_on_the_fly(int sq, Bitboard block)
{
    Bitboard attacks = 0ULL;
    int r, f;
    int tr = sq / 8;
    int tf = sq % 8;
    
    for (r = tr + 1, f = tf + 1; r <= 7 && f <= 7; r++, f++)
    {
        SET_BIT(attacks, r * 8 + f);
        if (block & (1ULL << (r * 8 + f)))
            break;
    }
    for (r = tr - 1, f = tf + 1; r >= 0 && f <= 7; r--, f++)
    {
        SET_BIT(attacks, r * 8 + f);
        if (block & (1ULL << (r * 8 + f)))
            break;
    }
    for (r = tr + 1, f = tf - 1; r <= 7 && f >= 0; r++, f--)
    {
        SET_BIT(attacks, r * 8 + f);
        if (block & (1ULL << (r * 8 + f)))
            break;
    }
    for (r = tr - 1, f = tf - 1; r >= 0 && f >= 0; r--, f--)
    {
        SET_BIT(attacks, r * 8 + f);
        if (block & (1ULL << (r * 8 + f)))
            break;
    }
    
    return attacks;
}

Bitboard rook_attacks_on_the_fly(int sq, Bitboard block)
{
    Bitboard attacks = 0ULL;
    int r, f;
    int tr = sq / 8;
    int tf = sq % 8;
    
    for (r = tr + 1; r <= 7; r++)
    {
        SET_BIT(attacks, r * 8 + tf);
        if (block & (1ULL << (r * 8 + tf)))
            break;
    }
    for (r = tr - 1; r >= 0; r--)
    {
        SET_BIT(attacks, r * 8 + tf);
        if (block & (1ULL << (r * 8 + tf)))
            break;
    }
    for (f = tf + 1; f <= 7; f++)
    {
        SET_BIT(attacks, tr * 8 + f);
        if (block & (1ULL << (tr * 8 + f)))
            break;
    }
    for (f = tf - 1; f >= 0; f--)
    {
        SET_BIT(attacks, tr * 8 + f);
        if (block & (1ULL << (tr * 8 + f)))
            break;
    }
    
    return attacks;
}

// --- Inicialización Principal ---

void init_bitboards()
{
    printf("Info: Inicializando tablas de ataques...\n");
    
    // Inicializar ataques de peones, caballos y rey
    for (int sq = 0; sq < 64; sq++)
    {
        // Peones
        Bitboard bb = 0ULL; SET_BIT(bb, sq);
        
        // pawn_attacks[color][square] = squares attacked BY a 'color' pawn ON 'square'
        
        // Ataques de peones blancos (hacia arriba)
        pawn_attacks[WHITE][sq] = ((bb << 7) & 0x7F7F7F7F7F7F7F7FULL) | // Izquierda (NW)
                                  ((bb << 9) & 0xFEFEFEFEFEFEFEFEULL);  // Derecha (NE)

        // Ataques de peones negros (hacia abajo)
        pawn_attacks[BLACK][sq] = ((bb >> 9) & 0x7F7F7F7F7F7F7F7FULL) | // Izquierda (SW)
                                  ((bb >> 7) & 0xFEFEFEFEFEFEFEFEULL);  // Derecha (SE)

        // Caballos
        knight_attacks[sq] = 0ULL;
        int knight_offsets[] = {-17, -15, -10, -6, 6, 10, 15, 17};
        int r = sq / 8, f = sq % 8;
        for (int i = 0; i < 8; i++)
        {
            int target_sq = sq + knight_offsets[i];
            int tr = target_sq / 8, tf = target_sq % 8;
            if (target_sq >= 0 && target_sq < 64 && abs(r - tr) <= 2 && abs(f - tf) <= 2)
                SET_BIT(knight_attacks[sq], target_sq);
        }

        // Rey
        king_attacks[sq] = 0ULL;
        int king_offsets[] = {-9, -8, -7, -1, 1, 7, 8, 9};
        for (int i = 0; i < 8; i++)
        {
            int target_sq = sq + king_offsets[i];
            int tr = target_sq / 8, tf = target_sq % 8;
            if (target_sq >= 0 && target_sq < 64 && abs(r - tr) <= 1 && abs(f - tf) <= 1)
                SET_BIT(king_attacks[sq], target_sq);
        }

        // --- Inicializar PEXT Tables ---
        
        // 1. Torres
        rook_masks[sq] = mask_rook_attacks(sq);
        int r_bits = count_bits(rook_masks[sq]);
        int r_indices = 1 << r_bits;
        
        for (int i = 0; i < r_indices; i++)
        {
            Bitboard occ = _pdep_u64(i, rook_masks[sq]); // Depositar bits (BMI2)
            int idx = _pext_u64(occ, rook_masks[sq]);    // Extraer bits (BMI2) - debería ser == i
            rook_table[sq][idx] = rook_attacks_on_the_fly(sq, occ);
        }

        // 2. Alfiles
        bishop_masks[sq] = mask_bishop_attacks(sq);
        int b_bits = count_bits(bishop_masks[sq]);
        int b_indices = 1 << b_bits;
        
        for (int i = 0; i < b_indices; i++)
        {
            Bitboard occ = _pdep_u64(i, bishop_masks[sq]);
            int idx = _pext_u64(occ, bishop_masks[sq]);
            bishop_table[sq][idx] = bishop_attacks_on_the_fly(sq, occ);
        }
    }
    printf("Info: Tablas de ataques inicializadas.\n");
}

// --- Funciones de Acceso (API) ---

Bitboard get_pawn_attacks(int side, int sq) { return pawn_attacks[side][sq]; }
Bitboard get_knight_attacks(int sq) { return knight_attacks[sq]; }
Bitboard get_king_attacks(int sq) { return king_attacks[sq]; }
Bitboard get_bishop_attacks(int sq, Bitboard occ) { return bishop_table[sq][_pext_u64(occ, bishop_masks[sq])]; }
Bitboard get_rook_attacks(int sq, Bitboard occ) { return rook_table[sq][_pext_u64(occ, rook_masks[sq])]; }
Bitboard get_queen_attacks(int sq, Bitboard occ) { return get_bishop_attacks(sq, occ) | get_rook_attacks(sq, occ); }