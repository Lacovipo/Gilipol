/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

 #include "common.h"
#include <ctype.h>

// --- Zobrist Hashing ---
uint64_t piece_keys[2][6][64];
uint64_t enpassant_keys[64];
uint64_t castle_keys[16];
uint64_t side_key;

// --- Historial de la Partida (para detección de repeticiones) ---
uint64_t game_history[1024];
int game_history_count = 0;

int is_repetition(const Position* pos)
{
    // Optimización: Si el reloj de 50 movimientos está a 0, es una jugada irreversible
    // (captura o peón), por lo que no puede haber repetición de una posición anterior.
    if (pos->half_move_clock == 0)
        return 0;

    // 1. Comprobar repeticiones en la línea actual de búsqueda (ciclos)
    // Retrocedemos usando los punteros 'prev' hasta el límite del reloj de 50 movimientos
    const Position* curr = pos->prev;
    int moves_back = 1; // Ya estamos 1 atrás
    
    while (curr && moves_back <= pos->half_move_clock)
    {
        if (curr->hash == pos->hash)
            return 1;
            
        curr = curr->prev;
        moves_back++;
    }
    
    // 2. Comprobar historial de la partida (si llegamos a la raíz)
    // Si 'curr' es NULL, significa que hemos llegado al inicio de la búsqueda (root).
    // Ahora comprobamos el historial global de la partida.
    if (!curr)
    {
        // Validar que game_history_count no exceda el límite
        int max_index = (game_history_count < MAX_GAME_HISTORY) ? game_history_count : MAX_GAME_HISTORY;

        // Comprobamos hacia atrás en el historial global
        for (int i = max_index - 1; i >= 0; i--)
        {
            if (moves_back > pos->half_move_clock)
                break;

            if (game_history[i] == pos->hash)
                return 1;
                
            moves_back++;
        }
    }
    
    return 0;
}

// Generador de números pseudo-aleatorios (Lagged Fibonacci)
static uint32_t random_state[55] =
{
    1410651636UL, 3012776752UL, 3497475623UL, 2892145026UL, 1571949714UL, 3253082284UL, 3489895018UL, 387949491UL, 2597396737UL, 1981903553UL,
    3160251843UL, 129444464UL, 1851443344UL, 4156445905UL, 224604922UL, 1455067070UL, 3953493484UL, 1460937157UL, 2528362617UL, 317430674UL, 
    3229354360UL, 117491133UL, 832845075UL, 1961600170UL, 1321557429UL, 747750121UL, 545747446UL, 810476036UL, 503334515UL, 4088144633UL,
    2824216555UL, 3738252341UL, 3493754131UL, 3672533954UL, 29494241UL, 1180928407UL, 4213624418UL, 33062851UL, 3221315737UL, 1145213552UL,
    2957984897UL, 4078668503UL, 2262661702UL, 65478801UL, 2527208841UL, 1960622036UL, 315685891UL, 1196037864UL, 804614524UL, 1421733266UL,
    2017105031UL, 3882325900UL, 810735053UL, 384606609UL, 2393861397UL
};
static int random_index = 0;

uint32_t rand32()
{
    uint32_t r = random_state[random_index] + random_state[(random_index + 31) % 55];
    random_state[random_index] = r;
    random_index = (random_index + 1) % 55;
    return r;
}

uint64_t rand64()
{
    uint64_t r1 = rand32();
    uint64_t r2 = rand32();
    return (r1 << 32) | r2;
}

void init_zobrist()
{
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            for (int sq = 0; sq < 64; sq++)
                piece_keys[c][p][sq] = rand64();
                
    for (int sq = 0; sq < 64; sq++)
        enpassant_keys[sq] = rand64();
    for (int i = 0; i < 16; i++)
        castle_keys[i] = rand64();
    side_key = rand64();
}

// Calcula el hash desde cero (lento, sólo para parse_fen)
uint64_t compute_hash(const Position* pos)
{
    uint64_t hash = 0;
    for (int c = 0; c < 2; c++)
    {
        for (int p = 0; p < 6; p++)
        {
            Bitboard bb = pos->pieces[c][p];
            while (bb)
            {
                int sq = get_lsb_index(bb);
                hash ^= piece_keys[c][p][sq];
                POP_BIT(bb, sq);
            }
        }
    }
    if (pos->en_passant_sq != SQ_NONE)
        hash ^= enpassant_keys[pos->en_passant_sq];
    hash ^= castle_keys[pos->castling_rights];
    if (pos->side_to_move == BLACK)
        hash ^= side_key;
    return hash;
}

// Parsea una cadena FEN y rellena la estructura Position
void parse_fen(Position* pos, const char* fen)
{
    if (fen == NULL)
        return;

    // Limpiar la posición actual
    memset(pos, 0, sizeof(Position));
    pos->en_passant_sq = SQ_NONE;
    
    int rank = 7;
    int file = 0;
    
    // 1. Colocación de piezas
    while ((rank >= 0) && *fen)
    {
        int count = 1;
        if (isdigit(*fen))
        {
            count = *fen - '0';
            file += count;
        }
        else if (*fen == '/')
        {
            rank--;
            file = 0;
        }
        else
        {
            int piece = -1;
            int color = (isupper(*fen)) ? WHITE : BLACK;
            char p = tolower(*fen);
            
            switch (p)
            {
                case 'p': piece = PAWN; break;
                case 'n': piece = KNIGHT; break;
                case 'b': piece = BISHOP; break;
                case 'r': piece = ROOK; break;
                case 'q': piece = QUEEN; break;
                case 'k': piece = KING; break;
            }
            
            if (piece != -1)
            {
                int sq = rank * 8 + file;
                pos->pieces[color][piece] |= (1ULL << sq);
                pos->occupied[color] |= (1ULL << sq);
                pos->all |= (1ULL << sq);
                file++;
            }
        }
        fen++;
        if (*fen == ' ')
            break; // Fin de la sección de piezas
    }
    
    // Avanzar al siguiente campo
    while (*fen == ' ')
        fen++;
    
    // 2. Color activo
    pos->side_to_move = (*fen == 'w') ? WHITE : BLACK;
    fen++;
    
    // 3. Derechos de enroque
    while (*fen == ' ')
        fen++;
    pos->castling_rights = 0;
    if (*fen != '-')
    {
        while (*fen != ' ' && *fen != '\0')
        {
            switch (*fen)
            {
                case 'K': pos->castling_rights |= WHITE_OO; break;
                case 'Q': pos->castling_rights |= WHITE_OOO; break;
                case 'k': pos->castling_rights |= BLACK_OO; break;
                case 'q': pos->castling_rights |= BLACK_OOO; break;
            }
            fen++;
        }
    }
    else
        fen++;
    
    // 4. Casilla al paso (En passant)
    while (*fen == ' ')
        fen++;
    if (*fen != '-')
    {
        int f = fen[0] - 'a';
        int r = fen[1] - '1';
        pos->en_passant_sq = r * 8 + f;
        fen += 2;
    }
    else
    {
        pos->en_passant_sq = SQ_NONE;
        fen++;
    }
    
    // 5. Reloj de 50 movimientos
    while (*fen == ' ')
        fen++;
    if (*fen)
        pos->half_move_clock = atoi(fen);
    
    // 6. Número de jugada completa
    while (*fen && *fen != ' ')
        fen++; // Saltar reloj anterior
    while (*fen == ' ')
        fen++;
    if (*fen)
        pos->full_move_number = atoi(fen);
    
    // Calcular hash inicial
    pos->hash = compute_hash(pos);

    // Validaciones básicas
    int white_kings = count_bits(pos->pieces[WHITE][KING]);
    int black_kings = count_bits(pos->pieces[BLACK][KING]);

    if (white_kings != 1 || black_kings != 1)
        printf("Error: FEN inválido - debe haber exactamente 1 rey por bando (White: %d, Black: %d)\n", white_kings, black_kings);

    // Verificar que no hay peones en rank 1 u 8
    if (pos->pieces[WHITE][PAWN] & 0xFF000000000000FFULL)
        printf("Warning: FEN tiene peones blancos en rank 1 u 8\n");
    if (pos->pieces[BLACK][PAWN] & 0xFF000000000000FFULL)
        printf("Warning: FEN tiene peones negros en rank 1 u 8\n");

    // Verificar que el rey enemigo no está en jaque
    int enemy = pos->side_to_move ^ 1;
    int enemy_king_sq = get_lsb_index(pos->pieces[enemy][KING]);
    if (enemy_king_sq >= 0 && is_square_attacked(pos, enemy_king_sq, pos->side_to_move))
        printf("Warning: FEN inválido - el rey del bando que no mueve está en jaque\n");

    // Inicializar acumulador NNUE
    nnue_refresh_accumulator(pos);
}

// Imprime el tablero en ASCII (útil para debug)
void print_board(const Position* pos)
{
    printf("\n");
    for (int rank = 7; rank >= 0; rank--)
    {
        printf(" %d ", rank + 1);
        for (int file = 0; file < 8; file++)
        {
            int sq = rank * 8 + file;
            char c = '.';
            
            for (int color = 0; color < COLOR_NB; color++)
            {
                for (int piece = 0; piece < PIECE_TYPE_NB; piece++)
                {
                    if (pos->pieces[color][piece] & (1ULL << sq))
                    {
                        char p = '?';
                        switch (piece)
                        {
                            case PAWN: p = 'p'; break;
                            case KNIGHT: p = 'n'; break;
                            case BISHOP: p = 'b'; break;
                            case ROOK: p = 'r'; break;
                            case QUEEN: p = 'q'; break;
                            case KING: p = 'k'; break;
                        }
                        c = (color == WHITE) ? toupper(p) : p;
                    }
                }
            }
            printf(" %c", c);
        }
        printf("\n");
    }
    printf("    a b c d e f g h\n\n");
    printf("Side: %s, Castling: %d, EP: %d\n", (pos->side_to_move == WHITE ? "White" : "Black"), pos->castling_rights, pos->en_passant_sq);
}

// Copia una posición a otra (para guardar estado antes de make_move)
void copy_position(Position* dest, const Position* src)
{
    memcpy(dest, src, sizeof(Position));
}

// Array constante para actualizar derechos de enroque rápidamente.
// Si una pieza se mueve DESDE o HACIA una casilla, hacemos AND con este array.
// La mayoría son 15 (1111), esquinas y reyes tienen bits apagados.
const int castling_rights_mask[64] =
{
    13, 15, 15, 15, 12, 15, 15, 14, // Rank 1 (White) - A1=13(no OOO), E1=12(no OO/OOO), H1=14(no OO)
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    7,  15, 15, 15, 3,  15, 15, 11  // Rank 8 (Black) - A8=7, E8=3, H8=11
};

// Helper para actualizar acumulador NNUE
static inline void accum_update(Accumulator* acc, int feature, int sign)
{
    for (int i = 0; i < HIDDEN1; i++)
        acc->values[i] += sign * nnue_weights.W1[feature][i];
}

// Realiza un movimiento en el tablero.
// Retorna 0 si el movimiento es ilegal (deja al rey en jaque), 1 si es legal.
int make_move(Position* pos, Move move)
{
    // 1. Guardar estado para restaurar si es ilegal (usamos copia externa en perft, pero aquí necesitamos verificar legalidad sobre el tablero modificado).
    // En este diseño "Copy-Make", el llamador es responsable de tener una copia segura.
    // Esta función MODIFICA 'pos'. Si retorna 0, 'pos' queda en estado indeterminado (sucio), por lo que el llamador debe descartarlo y usar su copia.

    int source = GET_MOVE_SOURCE(move);
    int target = GET_MOVE_TARGET(move);
    int promoted = GET_MOVE_PROMOTED(move);
    int type = -1; // Tipo de pieza que se mueve
    int side = pos->side_to_move;
    int enemy = (side == WHITE) ? BLACK : WHITE;
    int old_castling = pos->castling_rights;

    // --- NNUE: Actualizar Turno ---
    // Si mueven blancas, quitamos feature 768 (White to move).
    // Si mueven negras, añadimos feature 768 (porque el siguiente turno será White).
    if (side == WHITE)
        accum_update(&pos->accumulator, 768, -1);
    else
        accum_update(&pos->accumulator, 768, 1);

    // Identificar qué pieza se mueve
    for (int p = PAWN; p <= KING; p++)
    {
        if (GET_BIT(pos->pieces[side][p], source))
        {
            type = p;
            break;
        }
    }

    if (type == -1)
    {
        printf("Error: No piece at source sq %d\n", source);
        return 0;
    }

    // --- NNUE: Quitar pieza origen ---
    accum_update(&pos->accumulator, (side * 6 + type) * 64 + source, -1);

    // --- Actualizar Bitboards (Movimiento básico) ---
    POP_BIT(pos->pieces[side][type], source);
    SET_BIT(pos->pieces[side][type], target);
    POP_BIT(pos->occupied[side], source);
    SET_BIT(pos->occupied[side], target);
    POP_BIT(pos->all, source);
    SET_BIT(pos->all, target);
    pos->hash ^= piece_keys[side][type][source]; // Hash: Quitar pieza origen
    pos->hash ^= piece_keys[side][type][target]; // Hash: Poner pieza destino
    
    // --- Manejo de Capturas ---
    if (GET_MOVE_CAPTURE(move))
    {
        // En Passant
        if (GET_MOVE_ENPASSANT(move))
        {
            int ep_pawn_sq = (side == WHITE) ? target - 8 : target + 8;
            POP_BIT(pos->pieces[enemy][PAWN], ep_pawn_sq);
            POP_BIT(pos->occupied[enemy], ep_pawn_sq);
            POP_BIT(pos->all, ep_pawn_sq);
            pos->hash ^= piece_keys[enemy][PAWN][ep_pawn_sq];
            
            // --- NNUE: Quitar peón capturado (EP) ---
            accum_update(&pos->accumulator, (enemy * 6 + PAWN) * 64 + ep_pawn_sq, -1);
        }
        else
        {
            // Captura normal: buscar qué pieza enemiga estaba en target
            for (int p = PAWN; p <= KING; p++)
            {
                if (GET_BIT(pos->pieces[enemy][p], target))
                {
                    POP_BIT(pos->pieces[enemy][p], target);
                    POP_BIT(pos->occupied[enemy], target);
                    pos->hash ^= piece_keys[enemy][p][target];
                    
                    // --- NNUE: Quitar pieza capturada ---
                    accum_update(&pos->accumulator, (enemy * 6 + p) * 64 + target, -1);
                    break;
                }
            }
        }
    }

    // --- Manejo de Promociones ---
    if (promoted)
    {
        // Quitar el peón que acabamos de poner en target
        POP_BIT(pos->pieces[side][PAWN], target);
        // Poner la pieza promovida
        SET_BIT(pos->pieces[side][promoted], target);
        pos->hash ^= piece_keys[side][PAWN][target];     // Hash: Quitar peón (ya movido)
        pos->hash ^= piece_keys[side][promoted][target]; // Hash: Poner dama/torre/etc
        
        // --- NNUE: Añadir pieza promovida ---
        accum_update(&pos->accumulator, (side * 6 + promoted) * 64 + target, 1);
    }
    else
    {
        // --- NNUE: Añadir pieza movida (si no es promoción) ---
        accum_update(&pos->accumulator, (side * 6 + type) * 64 + target, 1);
    }

    // --- Manejo de Enroques ---
    if (GET_MOVE_CASTLING(move))
    {
        switch (target)
        {
            case SQ_G1: // White OO
                POP_BIT(pos->pieces[WHITE][ROOK], SQ_H1);
                SET_BIT(pos->pieces[WHITE][ROOK], SQ_F1);
                POP_BIT(pos->occupied[WHITE], SQ_H1);
                SET_BIT(pos->occupied[WHITE], SQ_F1);
                POP_BIT(pos->all, SQ_H1);
                SET_BIT(pos->all, SQ_F1);
                pos->hash ^= piece_keys[WHITE][ROOK][SQ_H1];
                pos->hash ^= piece_keys[WHITE][ROOK][SQ_F1];
                // --- NNUE: Mover Torre ---
                accum_update(&pos->accumulator, (WHITE * 6 + ROOK) * 64 + SQ_H1, -1);
                accum_update(&pos->accumulator, (WHITE * 6 + ROOK) * 64 + SQ_F1, 1);
                break;
            case SQ_C1: // White OOO
                POP_BIT(pos->pieces[WHITE][ROOK], SQ_A1);
                SET_BIT(pos->pieces[WHITE][ROOK], SQ_D1);
                POP_BIT(pos->occupied[WHITE], SQ_A1);
                SET_BIT(pos->occupied[WHITE], SQ_D1);
                POP_BIT(pos->all, SQ_A1);
                SET_BIT(pos->all, SQ_D1);
                pos->hash ^= piece_keys[WHITE][ROOK][SQ_A1];
                pos->hash ^= piece_keys[WHITE][ROOK][SQ_D1];
                // --- NNUE: Mover Torre ---
                accum_update(&pos->accumulator, (WHITE * 6 + ROOK) * 64 + SQ_A1, -1);
                accum_update(&pos->accumulator, (WHITE * 6 + ROOK) * 64 + SQ_D1, 1);
                break;
            case SQ_G8: // Black OO
                POP_BIT(pos->pieces[BLACK][ROOK], SQ_H8);
                SET_BIT(pos->pieces[BLACK][ROOK], SQ_F8);
                POP_BIT(pos->occupied[BLACK], SQ_H8);
                SET_BIT(pos->occupied[BLACK], SQ_F8);
                POP_BIT(pos->all, SQ_H8);
                SET_BIT(pos->all, SQ_F8);
                pos->hash ^= piece_keys[BLACK][ROOK][SQ_H8];
                pos->hash ^= piece_keys[BLACK][ROOK][SQ_F8];
                // --- NNUE: Mover Torre ---
                accum_update(&pos->accumulator, (BLACK * 6 + ROOK) * 64 + SQ_H8, -1);
                accum_update(&pos->accumulator, (BLACK * 6 + ROOK) * 64 + SQ_F8, 1);
                break;
            case SQ_C8: // Black OOO
                POP_BIT(pos->pieces[BLACK][ROOK], SQ_A8);
                SET_BIT(pos->pieces[BLACK][ROOK], SQ_D8);
                POP_BIT(pos->occupied[BLACK], SQ_A8);
                SET_BIT(pos->occupied[BLACK], SQ_D8);
                POP_BIT(pos->all, SQ_A8);
                SET_BIT(pos->all, SQ_D8);
                pos->hash ^= piece_keys[BLACK][ROOK][SQ_A8];
                pos->hash ^= piece_keys[BLACK][ROOK][SQ_D8];
                // --- NNUE: Mover Torre ---
                accum_update(&pos->accumulator, (BLACK * 6 + ROOK) * 64 + SQ_A8, -1);
                accum_update(&pos->accumulator, (BLACK * 6 + ROOK) * 64 + SQ_D8, 1);
                break;
        }
    }

    // --- Actualizar Estado del Tablero ---

    // Actualizar derechos de enroque
    pos->hash ^= castle_keys[pos->castling_rights]; // Hash: Quitar derechos viejos
    pos->castling_rights &= castling_rights_mask[source];
    pos->castling_rights &= castling_rights_mask[target];
    pos->hash ^= castle_keys[pos->castling_rights]; // Hash: Poner derechos nuevos

    // --- NNUE: Actualizar derechos de enroque ---
    int rights_diff = old_castling ^ pos->castling_rights;
    if (rights_diff)
    {
        if (rights_diff & WHITE_OO)
            accum_update(&pos->accumulator, 769, -1);
        if (rights_diff & WHITE_OOO)
            accum_update(&pos->accumulator, 770, -1);
        if (rights_diff & BLACK_OO)
            accum_update(&pos->accumulator, 771, -1);
        if (rights_diff & BLACK_OOO)
            accum_update(&pos->accumulator, 772, -1);
    }

    // Actualizar En Passant
    if (pos->en_passant_sq != SQ_NONE)
        pos->hash ^= enpassant_keys[pos->en_passant_sq]; // Hash: Quitar EP viejo
    pos->en_passant_sq = SQ_NONE; // Resetear por defecto
    if (GET_MOVE_DOUBLE(move))
    {
        if (side == WHITE)
            pos->en_passant_sq = target - 8;
        else
            pos->en_passant_sq = target + 8;
        pos->hash ^= enpassant_keys[pos->en_passant_sq]; // Hash: Poner EP nuevo
    }

    // Actualizar reloj de 50 movimientos
    if (GET_MOVE_CAPTURE(move) || type == PAWN)
        pos->half_move_clock = 0;
    else
        pos->half_move_clock++;

    // Actualizar número de jugada completa
    if (side == BLACK)
        pos->full_move_number++;

    // Cambiar turno
    pos->side_to_move ^= 1;
    pos->hash ^= side_key;

    // --- Verificación de Legalidad ---
    // Si el rey del bando que movió (side) está en jaque, el movimiento es ilegal.
    // Nota: 'side' es el que acaba de mover.
    
    // Buscar dónde está el rey
    int king_sq = get_lsb_index(pos->pieces[side][KING]);
    
    if (is_square_attacked(pos, king_sq, enemy))
    {
        // El movimiento es ilegal, el llamador debe restaurar la copia
        return 0;
    }

    return 1;
}

// Realiza un "Movimiento Nulo" (pasar el turno).
// Se usa para la heurística Null Move Pruning en la búsqueda.
void make_null_move(Position* pos)
{
    // Cambiar turno
    pos->side_to_move ^= 1;
    pos->hash ^= side_key;

    // --- NNUE: Actualizar Turno ---
    // Si era WHITE, ahora es BLACK. Quitamos 768.
    if (pos->side_to_move == BLACK)
        accum_update(&pos->accumulator, 768, -1);
    else
        accum_update(&pos->accumulator, 768, 1);
    
    // Resetear En Passant si lo había (un movimiento nulo pierde la oportunidad)
    if (pos->en_passant_sq != SQ_NONE)
    {
        pos->hash ^= enpassant_keys[pos->en_passant_sq];
        pos->en_passant_sq = SQ_NONE;
    }
}