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

// --- Ataques de alfiles y torres: PEXT (BMI2) o magic bitboards ---
//
// Las dos formas usan las mismas tablas; solo cambia como se calcula el indice a
// partir de la ocupacion. Compilado con -mbmi2 (o -march=native en una CPU con BMI2)
// se usa siempre PEXT; en el ejecutable generico se decide al arrancar (cpu_pext):
// sin BMI2, o en AMD anterior a Zen 3 (PEXT por microcodigo, lentisimo), magics.
Bitboard rook_masks[64];
Bitboard bishop_masks[64];
Bitboard rook_table[64][4096];   // 12 bits max para torres = 4096 entradas
Bitboard bishop_table[64][512];  // 9 bits max para alfiles = 512 entradas
static Bitboard rook_magics[64];
static Bitboard bishop_magics[64];
static int      rook_shift[64];
static int      bishop_shift[64];

#if !defined(__BMI2__)
// PEXT en ensamblador: el compilador no emite la instruccion sin -mbmi2, y solo se
// ejecuta si cpu_pext dice que la CPU la tiene.
static inline uint64_t pext_hw(uint64_t x, uint64_t mascara)
{
    uint64_t r;
    __asm__("pextq %2, %1, %0" : "=r"(r) : "r"(x), "rm"(mascara));
    return r;
}
#endif

static inline int indice_torre(int sq, Bitboard occ)
{
#if defined(__BMI2__)
    return (int)_pext_u64(occ, rook_masks[sq]);
#else
    if (cpu_pext)
        return (int)pext_hw(occ, rook_masks[sq]);
    return (int)(((occ & rook_masks[sq]) * rook_magics[sq]) >> rook_shift[sq]);
#endif
}

static inline int indice_alfil(int sq, Bitboard occ)
{
#if defined(__BMI2__)
    return (int)_pext_u64(occ, bishop_masks[sq]);
#else
    if (cpu_pext)
        return (int)pext_hw(occ, bishop_masks[sq]);
    return (int)(((occ & bishop_masks[sq]) * bishop_magics[sq]) >> bishop_shift[sq]);
#endif
}

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

// --- Busqueda de magics (solo si no se usa PEXT) ---
//
// Para cada casilla se prueban multiplicadores aleatorios con pocos bits hasta dar
// con uno que mande cada ocupacion a un indice sin colisiones destructivas. El
// generador tiene semilla fija: siempre salen los mismos magics. Tarda milisegundos.

static uint64_t estado_magic = 0x2545F4914F6CDD1DULL;

static uint64_t aleatorio_magic(void)
{
    estado_magic ^= estado_magic >> 12;
    estado_magic ^= estado_magic << 25;
    estado_magic ^= estado_magic >> 27;
    return estado_magic * 0x2545F4914F6CDD1DULL;
}

static Bitboard buscar_magic(int sq, Bitboard mascara, int bits, int torre)
{
    static Bitboard ocupaciones[4096], ataques[4096], usado[4096];
    static int      marca[4096];
    static int      intento = 0;

    int n = 0;
    Bitboard occ = 0;
    do      // todos los subconjuntos de la mascara (Carry-Rippler)
    {
        ocupaciones[n] = occ;
        ataques[n] = torre ? rook_attacks_on_the_fly(sq, occ) : bishop_attacks_on_the_fly(sq, occ);
        n++;
        occ = (occ - mascara) & mascara;
    } while (occ);

    for (;;)
    {
        Bitboard magic = aleatorio_magic() & aleatorio_magic() & aleatorio_magic();
        if (count_bits((mascara * magic) & 0xFF00000000000000ULL) < 6)
            continue;
        intento++;
        int ok = 1;
        for (int i = 0; i < n && ok; i++)
        {
            int idx = (int)((ocupaciones[i] * magic) >> (64 - bits));
            if (marca[idx] != intento)
            {
                marca[idx] = intento;
                usado[idx] = ataques[i];
            }
            else if (usado[idx] != ataques[i])
                ok = 0;
        }
        if (ok)
            return magic;
    }
}

// --- Inicialización Principal ---

void init_bitboards()
{
    detectar_cpu();     // cpu_pext decide como se indexan las tablas
    printf("info string Inicializando tablas de ataques...\n");
    
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

        // --- Tablas de alfiles y torres (PEXT o magics) ---
#if defined(__BMI2__)
        const int usa_magics = 0;
#else
        const int usa_magics = !cpu_pext;
#endif

        // 1. Torres
        rook_masks[sq] = mask_rook_attacks(sq);
        int r_bits = count_bits(rook_masks[sq]);
        rook_shift[sq] = 64 - r_bits;
        if (usa_magics)
            rook_magics[sq] = buscar_magic(sq, rook_masks[sq], r_bits, 1);
        Bitboard occ = 0;
        do      // todos los subconjuntos de la mascara (Carry-Rippler)
        {
            rook_table[sq][indice_torre(sq, occ)] = rook_attacks_on_the_fly(sq, occ);
            occ = (occ - rook_masks[sq]) & rook_masks[sq];
        } while (occ);

        // 2. Alfiles
        bishop_masks[sq] = mask_bishop_attacks(sq);
        int b_bits = count_bits(bishop_masks[sq]);
        bishop_shift[sq] = 64 - b_bits;
        if (usa_magics)
            bishop_magics[sq] = buscar_magic(sq, bishop_masks[sq], b_bits, 0);
        occ = 0;
        do
        {
            bishop_table[sq][indice_alfil(sq, occ)] = bishop_attacks_on_the_fly(sq, occ);
            occ = (occ - bishop_masks[sq]) & bishop_masks[sq];
        } while (occ);
    }
    printf("info string Tablas de ataques inicializadas\n");
}

// --- Funciones de Acceso (API) ---

Bitboard get_pawn_attacks(int side, int sq) { return pawn_attacks[side][sq]; }
Bitboard get_knight_attacks(int sq) { return knight_attacks[sq]; }
Bitboard get_king_attacks(int sq) { return king_attacks[sq]; }
Bitboard get_bishop_attacks(int sq, Bitboard occ) { return bishop_table[sq][indice_alfil(sq, occ)]; }
Bitboard get_rook_attacks(int sq, Bitboard occ) { return rook_table[sq][indice_torre(sq, occ)]; }
Bitboard get_queen_attacks(int sq, Bitboard occ) { return get_bishop_attacks(sq, occ) | get_rook_attacks(sq, occ); }