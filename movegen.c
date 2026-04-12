/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

 #include "common.h"

// --- Utilidades ---

// Imprime un movimiento en formato UCI (ej. "e2e4", "a7a8q")
void print_move(Move move)
{
    int source = GET_MOVE_SOURCE(move);
    int target = GET_MOVE_TARGET(move);
    int promoted = GET_MOVE_PROMOTED(move);
    
    printf("%c%d%c%d", (source % 8) + 'a', (source / 8) + 1, (target % 8) + 'a', (target / 8) + 1);
           
    if (promoted)
    {
        char p = ' ';
        switch(promoted)
        {
            case KNIGHT: p = 'n'; break;
            case BISHOP: p = 'b'; break;
            case ROOK:   p = 'r'; break;
            case QUEEN:  p = 'q'; break;
        }
        printf("%c", p);
    }
}

// Añade un movimiento a la lista (helper interno)
// Convertido a macro para forzar inlining y evitar overhead de llamada
#define ADD_MOVE(list, move) { \
    (list)->moves[(list)->count++] = (move); \
}

// --- Helpers para el generador ---

// Devuelve un bitboard con todas las piezas que atacan una casilla
static Bitboard attackers_of(const Position* pos, int sq, int by_side)
{
    int enemy_side = by_side ^ 1;
    Bitboard attackers = 0ULL;
    attackers |= (get_pawn_attacks(enemy_side, sq) & pos->pieces[by_side][PAWN]);
    attackers |= (get_knight_attacks(sq) & pos->pieces[by_side][KNIGHT]);
    attackers |= (get_king_attacks(sq) & pos->pieces[by_side][KING]);
    attackers |= (get_bishop_attacks(sq, pos->all) & (pos->pieces[by_side][BISHOP] | pos->pieces[by_side][QUEEN]));
    attackers |= (get_rook_attacks(sq, pos->all) & (pos->pieces[by_side][ROOK] | pos->pieces[by_side][QUEEN]));
    return attackers;
}

// Devuelve la línea entre dos casillas (sin incluirlas)
static Bitboard get_line_between(int sq1, int sq2)
{
    Bitboard b1 = 1ULL << sq1;
    Bitboard b2 = 1ULL << sq2;
    Bitboard attacks1 = get_queen_attacks(sq1, b2);
    Bitboard attacks2 = get_queen_attacks(sq2, b1);
    return attacks1 & attacks2;
}

// --- Generadores por tipo de pieza ---

static void generate_pawn_moves(const Position* pos, MoveList* list, int gen_type, Bitboard target_mask)
{
    int side = pos->side_to_move;
    int enemy = side ^ 1;
    Bitboard pawns = pos->pieces[side][PAWN];
    Bitboard enemy_pieces = pos->occupied[enemy];
    Bitboard occupancy = pos->all;

    if (side == WHITE)
    {
        // --- Capturas ---
        Bitboard promotions = 0xFF00000000000000ULL; // Rank 8
        Bitboard attacks_left = ((pawns << 7) & 0x7F7F7F7F7F7F7F7FULL) & enemy_pieces & target_mask;
        Bitboard attacks_right = ((pawns << 9) & 0xFEFEFEFEFEFEFEFEULL) & enemy_pieces & target_mask;

        while (attacks_left)
        {
            int target = pop_lsb(&attacks_left);
            int source = target - 7;
            if (GET_BIT(promotions, target))
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 1, 0, 0, 0));
            }
            else
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 1, 0, 0, 0));
            }
        }
        while (attacks_right)
        {
            int target = pop_lsb(&attacks_right);
            int source = target - 9;
            if (GET_BIT(promotions, target))
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 1, 0, 0, 0));
            }
            else
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 1, 0, 0, 0));
            }
        }

        if (gen_type == GEN_ALL)
        {
            // --- Movimientos simples ---
            Bitboard single_push = (pawns << 8) & ~occupancy & target_mask;
            while (single_push)
            {
                int target = pop_lsb(&single_push);
                int source = target - 8;
                if (GET_BIT(promotions, target))
                {
                    ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 0, 0, 0, 0));
                }
                else
                {
                    ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 0, 0, 0, 0));
                }
            }

            // --- Movimientos dobles (solo desde rank 2) ---
            Bitboard pawns_on_rank_2 = pawns & 0x000000000000FF00ULL;
            Bitboard double_push = ((pawns_on_rank_2 << 8) & ~occupancy);
            double_push = ((double_push << 8) & ~occupancy) & target_mask;
            while (double_push)
            {
                int target = pop_lsb(&double_push);
                ADD_MOVE(list, ENCODE_MOVE(target - 16, target, 0, 0, 1, 0, 0));
            }
        }
        // En Passant (siempre es una captura)
        if (pos->en_passant_sq != SQ_NONE && (target_mask & (1ULL << pos->en_passant_sq)))
        {
            Bitboard ep_attackers = get_pawn_attacks(BLACK, pos->en_passant_sq) & pawns;
            while (ep_attackers)
            {
                int source = pop_lsb(&ep_attackers);
                ADD_MOVE(list, ENCODE_MOVE(source, pos->en_passant_sq, 0, 1, 0, 1, 0));
            }
        }
    }
    else
    {
         // BLACK
        Bitboard promotions = 0x00000000000000FFULL;
        Bitboard attacks_left = ((pawns >> 9) & 0x7F7F7F7F7F7F7F7FULL) & enemy_pieces & target_mask;
        Bitboard attacks_right = ((pawns >> 7) & 0xFEFEFEFEFEFEFEFEULL) & enemy_pieces & target_mask;

        while (attacks_left)
        {
            int target = pop_lsb(&attacks_left);
            int source = target + 9;
            if (GET_BIT(promotions, target))
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 1, 0, 0, 0));
            }
            else
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 1, 0, 0, 0));
            }
        }
        while (attacks_right)
        {
            int target = pop_lsb(&attacks_right);
            int source = target + 7;
            if (GET_BIT(promotions, target))
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 1, 0, 0, 0));
                ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 1, 0, 0, 0));
            }
            else
            {
                ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 1, 0, 0, 0));
            }
        }

        if (gen_type == GEN_ALL)
        {
            // --- Movimientos simples ---
            Bitboard single_push = (pawns >> 8) & ~occupancy & target_mask;
            while (single_push)
            {
                int target = pop_lsb(&single_push);
                int source = target + 8;
                if (GET_BIT(promotions, target))
                {
                    ADD_MOVE(list, ENCODE_MOVE(source, target, QUEEN, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, ROOK, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, BISHOP, 0, 0, 0, 0));
                    ADD_MOVE(list, ENCODE_MOVE(source, target, KNIGHT, 0, 0, 0, 0));
                }
                else
                {
                    ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 0, 0, 0, 0));
                }
            }

            // --- Movimientos dobles (solo desde rank 7) ---
            Bitboard pawns_on_rank_7 = pawns & 0x00FF000000000000ULL;
            Bitboard double_push = ((pawns_on_rank_7 >> 8) & ~occupancy);
            double_push = ((double_push >> 8) & ~occupancy) & target_mask;
            while (double_push)
            {
                int target = pop_lsb(&double_push);
                ADD_MOVE(list, ENCODE_MOVE(target + 16, target, 0, 0, 1, 0, 0));
            }
        }
        if (pos->en_passant_sq != SQ_NONE && (target_mask & (1ULL << pos->en_passant_sq)))
        {
            Bitboard ep_attackers = get_pawn_attacks(WHITE, pos->en_passant_sq) & pawns;
            while (ep_attackers)
            {
                int source = pop_lsb(&ep_attackers);
                ADD_MOVE(list, ENCODE_MOVE(source, pos->en_passant_sq, 0, 1, 0, 1, 0));
            }
        }
    }
}

static void generate_piece_moves(const Position* pos, MoveList* list, int gen_type, Bitboard target_mask, int piece_type)
{
    int side = pos->side_to_move;
    int enemy = side ^ 1;
    Bitboard pieces = pos->pieces[side][piece_type];
    Bitboard own_pieces = pos->occupied[side];
    Bitboard enemy_pieces = pos->occupied[enemy];

    while (pieces)
    {
        int source = pop_lsb(&pieces);

        Bitboard attacks = 0ULL;
        switch(piece_type)
        {
            case KNIGHT: attacks = get_knight_attacks(source); break;
            case BISHOP: attacks = get_bishop_attacks(source, pos->all); break;
            case ROOK:   attacks = get_rook_attacks(source, pos->all); break;
            case QUEEN:  attacks = get_queen_attacks(source, pos->all); break;
            case KING:   attacks = get_king_attacks(source); break;
        }
        attacks &= target_mask;

        Bitboard captures = attacks & enemy_pieces;
        while (captures)
        {
            int target = pop_lsb(&captures);
            ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 1, 0, 0, 0));
        }

        if (gen_type == GEN_ALL)
        {
            Bitboard quiet_moves = attacks & ~own_pieces & ~enemy_pieces;
            while (quiet_moves)
            {
                int target = pop_lsb(&quiet_moves);
                ADD_MOVE(list, ENCODE_MOVE(source, target, 0, 0, 0, 0, 0));
            }
        }
    }
}

static void generate_castling(const Position* pos, MoveList* list)
{
    int side = pos->side_to_move;
    if (side == WHITE)
    {
        if ((pos->castling_rights & WHITE_OO) && !(pos->all & 0x60ULL) && !is_square_attacked(pos, SQ_E1, BLACK) && !is_square_attacked(pos, SQ_F1, BLACK) && !is_square_attacked(pos, SQ_G1, BLACK))
            ADD_MOVE(list, ENCODE_MOVE(SQ_E1, SQ_G1, 0, 0, 0, 0, 1));
        if ((pos->castling_rights & WHITE_OOO) && !(pos->all & 0xEULL) && !is_square_attacked(pos, SQ_E1, BLACK) && !is_square_attacked(pos, SQ_D1, BLACK) && !is_square_attacked(pos, SQ_C1, BLACK))
            ADD_MOVE(list, ENCODE_MOVE(SQ_E1, SQ_C1, 0, 0, 0, 0, 1));
    }
    else
    {
        if ((pos->castling_rights & BLACK_OO) && !(pos->all & 0x6000000000000000ULL) && !is_square_attacked(pos, SQ_E8, WHITE) && !is_square_attacked(pos, SQ_F8, WHITE) && !is_square_attacked(pos, SQ_G8, WHITE))
            ADD_MOVE(list, ENCODE_MOVE(SQ_E8, SQ_G8, 0, 0, 0, 0, 1));
        if ((pos->castling_rights & BLACK_OOO) && !(pos->all & 0xE00000000000000ULL) && !is_square_attacked(pos, SQ_E8, WHITE) && !is_square_attacked(pos, SQ_D8, WHITE) && !is_square_attacked(pos, SQ_C8, WHITE))
            ADD_MOVE(list, ENCODE_MOVE(SQ_E8, SQ_C8, 0, 0, 0, 0, 1));
    }
}

// --- Generadores principales ---

static void generate_evasions(const Position* pos, MoveList* list)
{
    int side = pos->side_to_move;
    int enemy = side ^ 1;
    int king_sq = get_lsb_index(pos->pieces[side][KING]);
    Bitboard checkers = attackers_of(pos, king_sq, enemy);
    
    // Generar movimientos de rey a casillas no ocupadas por piezas propias
    // IMPORTANTE: Hacer esto ANTES de comprobar checkers. El rey siempre debe poder intentar escapar.
    generate_piece_moves(pos, list, GEN_ALL, ~pos->occupied[side], KING);

    if (!checkers)
    {
        printf("ERROR CRITICO: is_square_attacked() es true pero attackers_of() es 0!\n");
        printf("Side: %d, KingSq: %d\n", side, king_sq);
        print_board(pos);
        ATOMIC_STORE(stop_search, 1);
        return;
    }

    // Si es jaque doble, solo los movimientos de rey son válidos
    if (count_bits(checkers) > 1)
        return;

    // Si es jaque simple, también se puede capturar o bloquear
    int checker_sq = get_lsb_index(checkers);
    Bitboard target_mask = (1ULL << checker_sq) | get_line_between(king_sq, checker_sq);

    // FIX: Permitir captura al paso si el atacante es el peón que acaba de mover
    Bitboard pawn_target_mask = target_mask;
    if (pos->en_passant_sq != SQ_NONE)
    {
        int ep_pawn_sq = (side == WHITE) ? pos->en_passant_sq - 8 : pos->en_passant_sq + 8;
        if (checker_sq == ep_pawn_sq)
        {
            SET_BIT(pawn_target_mask, pos->en_passant_sq);
        }
    }

    generate_pawn_moves(pos, list, GEN_ALL, pawn_target_mask);
    generate_piece_moves(pos, list, GEN_ALL, target_mask, KNIGHT);
    generate_piece_moves(pos, list, GEN_ALL, target_mask, BISHOP);
    generate_piece_moves(pos, list, GEN_ALL, target_mask, ROOK);
    generate_piece_moves(pos, list, GEN_ALL, target_mask, QUEEN);
}

static void generate_pseudo_legal(const Position* pos, MoveList* list, int gen_type)
{
    Bitboard all_squares = ~0ULL;
    generate_pawn_moves(pos, list, gen_type, all_squares);
    generate_piece_moves(pos, list, gen_type, all_squares, KNIGHT);
    generate_piece_moves(pos, list, gen_type, all_squares, BISHOP);
    generate_piece_moves(pos, list, gen_type, all_squares, ROOK);
    generate_piece_moves(pos, list, gen_type, all_squares, QUEEN);
    generate_piece_moves(pos, list, gen_type, all_squares, KING);
    if (gen_type == GEN_ALL)
    {
        generate_castling(pos, list);
    }
}

// Detecta si un movimiento dará jaque (sin realizarlo con make_move)
int gives_check(const Position* pos, Move move)
{
    int side = pos->side_to_move;
    int enemy = side ^ 1;
    Bitboard king_bb = pos->pieces[enemy][KING];
    int king_sq = get_lsb_index(king_bb);

    // Si no hay rey (no debería pasar), no hay jaque
    if (king_sq == -1)
        return 0;

    int from = GET_MOVE_SOURCE(move);
    int to = GET_MOVE_TARGET(move);
    
    // Identificar pieza que se mueve
    int type = -1;
    if (GET_BIT(pos->pieces[side][PAWN], from))
        type = PAWN;
    else if (GET_BIT(pos->pieces[side][KNIGHT], from))
        type = KNIGHT;
    else if (GET_BIT(pos->pieces[side][BISHOP], from))
        type = BISHOP;
    else if (GET_BIT(pos->pieces[side][ROOK], from))
        type = ROOK;
    else if (GET_BIT(pos->pieces[side][QUEEN], from))
        type = QUEEN;
    else if (GET_BIT(pos->pieces[side][KING], from))
        type = KING;
    
    // La promoción cambia el tipo de pieza atacante
    int promo = GET_MOVE_PROMOTED(move);
    if (promo)
        type = promo;

    // Calcular ocupación post-movimiento (para sliders y descubiertos)
    Bitboard occ = pos->all;
    POP_BIT(occ, from);
    SET_BIT(occ, to);
    
    // Caso En Passant: quitar el peón capturado de la ocupación
    if (GET_MOVE_ENPASSANT(move))
    {
        int cap_sq = (side == WHITE) ? to - 8 : to + 8;
        POP_BIT(occ, cap_sq);
    }

    // 1. Jaque Directo (desde la casilla de destino 'to')
    if (type == KNIGHT)
    {
        if (get_knight_attacks(to) & king_bb)
            return 1;
    }
    else if (type == PAWN)
    {
        if (get_pawn_attacks(side, to) & king_bb)
            return 1;
    }
    else if (type != KING) // Sliders (Alfil, Torre, Dama)
    {
        if (type == BISHOP || type == QUEEN)
            if (get_bishop_attacks(to, occ) & king_bb)
                return 1;
        if (type == ROOK || type == QUEEN)
            if (get_rook_attacks(to, occ) & king_bb)
                return 1;
    }

    // 2. Jaque Descubierto
    // Pre-filtro: si ninguna casilla que se vacía está alineada con el rey enemigo,
    // no puede haber jaque descubierto -> salir sin lookups.
    // Enroque: el rey (from) no es la pieza que da jaque; la torre se maneja dentro del
    // bloque de enroque más abajo, así que nunca aplicamos el pre-filtro aquí.
    if (!GET_MOVE_CASTLING(move))
    {
        int df = (from & 7) - (king_sq & 7);
        int dr = (from >> 3) - (king_sq >> 3);
        int from_aligned = (df == 0 || dr == 0 || df == dr || df == -dr);

        // En passant también vacía cap_sq, que puede abrir una línea distinta a 'from'
        int cap_aligned = 0;
        if (!from_aligned && GET_MOVE_ENPASSANT(move))
        {
            int cap_sq = (side == WHITE) ? to - 8 : to + 8;
            int df2 = (cap_sq & 7) - (king_sq & 7);
            int dr2 = (cap_sq >> 3) - (king_sq >> 3);
            cap_aligned = (df2 == 0 || dr2 == 0 || df2 == dr2 || df2 == -dr2);
        }

        if (!from_aligned && !cap_aligned)
            return 0;
    }

    // Verificamos si algún slider nuestro ataca al rey a través de los huecos dejados.
    // 'occ' ya tiene 'from' vacío.
    Bitboard bishops = pos->pieces[side][BISHOP] | pos->pieces[side][QUEEN];
    Bitboard rooks   = pos->pieces[side][ROOK]   | pos->pieces[side][QUEEN];
    
    // Excluir la pieza que se mueve de los candidatos a descubrir (ya revisada en directo)
    POP_BIT(bishops, from);
    POP_BIT(rooks, from);
    
    // Caso especial Enroque: El rey no da jaque, pero la torre salta
    if (GET_MOVE_CASTLING(move))
    {
        int r_from, r_to;
        if (to == SQ_G1)
        {
            r_from = SQ_H1;
            r_to = SQ_F1;
        }
        else if (to == SQ_C1)
        {
            r_from = SQ_A1;
            r_to = SQ_D1;
        }
        else if (to == SQ_G8)
        {
            r_from = SQ_H8;
            r_to = SQ_F8;
        }
        else if (to == SQ_C8)
        {
            r_from = SQ_A8;
            r_to = SQ_D8;
        }
        else
            return 0;

        // Actualizar ocupación de la torre
        POP_BIT(occ, r_from);
        SET_BIT(occ, r_to);
        
        // Verificar jaque directo de la torre
        if (get_rook_attacks(r_to, occ) & king_bb)
            return 1;
        
        // Quitar la torre vieja de los candidatos a descubrir
        POP_BIT(rooks, r_from);
    }
    
    // Rayos X desde el rey hacia nuestros sliders
    if (bishops && (get_bishop_attacks(king_sq, occ) & bishops))
        return 1;
    if (rooks && (get_rook_attacks(king_sq, occ) & rooks))
        return 1;

    return 0;
}

// Devuelve 1 si la casilla 'sq' está siendo atacada por el bando 'side'
int is_square_attacked(const Position* pos, int sq, int side)
{
    // 1. Ataques de peones enemigos
    // Usamos la simetría: si un peón del color opuesto estuviera en 'sq', ¿atacaría a algún peón del bando 'side'?
    // side es el atacante. Queremos saber si un peón de 'side' ataca 'sq'.
    // Eso equivale a ver si un peón del color opuesto en 'sq' atacaría a alguna pieza de 'side'.
    int enemy = (side == WHITE) ? BLACK : WHITE;
    if (get_pawn_attacks(enemy, sq) & pos->pieces[side][PAWN])
        return 1;

    // 2. Caballos
    if (get_knight_attacks(sq) & pos->pieces[side][KNIGHT])
        return 1;

    // 3. Rey
    if (get_king_attacks(sq) & pos->pieces[side][KING])
        return 1;

    // 4. Alfiles y Damas (Diagonales)
    Bitboard bishop_queen = pos->pieces[side][BISHOP] | pos->pieces[side][QUEEN];
    if (bishop_queen)
    { 
        if (get_bishop_attacks(sq, pos->all) & bishop_queen)
            return 1;
    }

    // 5. Torres y Damas (Ortogonales)
    Bitboard rook_queen = pos->pieces[side][ROOK] | pos->pieces[side][QUEEN];
    if (rook_queen)
    {
        if (get_rook_attacks(sq, pos->all) & rook_queen)
            return 1;
    }

    return 0;
}

// --- Generador de Movimientos ---

void generate_moves(const Position* pos, MoveList* list, int gen_type)
{
    list->count = 0;
    int side = pos->side_to_move;
    int king_sq = get_lsb_index(pos->pieces[side][KING]);

    if (is_square_attacked(pos, king_sq, side ^ 1))
        generate_evasions(pos, list);
    else
        generate_pseudo_legal(pos, list, gen_type);
}

// Wrapper para generar solo capturas (para Quiescence Search)
void generate_captures(const Position* pos, MoveList* list)
{
    generate_moves(pos, list, GEN_CAPTURES);
}

// --- Static Exchange Evaluation (SEE) ---

static const int see_values[] = { 100, 320, 330, 500, 900, 20000 };

int see(const Position* pos, Move move, int threshold)
{
    // 1. Identificar pieza capturada y su valor
    int victim_val = 0;
    
    if (GET_MOVE_CAPTURE(move))
    {
        if (GET_MOVE_ENPASSANT(move))
            victim_val = see_values[PAWN];
        else
        {
            int target = GET_MOVE_TARGET(move);
            int enemy = pos->side_to_move ^ 1;
            // Buscar la pieza víctima
            if (GET_BIT(pos->pieces[enemy][PAWN], target))
                victim_val = see_values[PAWN];
            else if (GET_BIT(pos->pieces[enemy][KNIGHT], target))
                victim_val = see_values[KNIGHT];
            else if (GET_BIT(pos->pieces[enemy][BISHOP], target))
                victim_val = see_values[BISHOP];
            else if (GET_BIT(pos->pieces[enemy][ROOK], target))
                victim_val = see_values[ROOK];
            else if (GET_BIT(pos->pieces[enemy][QUEEN], target))
                victim_val = see_values[QUEEN];
            else if (GET_BIT(pos->pieces[enemy][KING], target))
                victim_val = see_values[KING];
        }
    }
    
    // Manejo de promoción: el atacante "cambia" de valor para futuras recapturas
    if (GET_MOVE_PROMOTED(move))
    {
        // Bonus inmediato por promocionar (ganamos la diferencia de material)
        victim_val += see_values[GET_MOVE_PROMOTED(move)] - see_values[PAWN];
    }

    // Balance inicial: valor ganado - umbral
    int balance = victim_val - threshold;
    
    // Si incluso ganando la pieza "gratis" no llegamos al umbral, devolvemos false (0)
    if (balance < 0)
        return 0;

    // 2. Identificar pieza atacante inicial
    int from = GET_MOVE_SOURCE(move);
    int to = GET_MOVE_TARGET(move);
    int attacker_type = -1;
    int side = pos->side_to_move;
    
    if (GET_BIT(pos->pieces[side][PAWN], from))
        attacker_type = PAWN;
    else if (GET_BIT(pos->pieces[side][KNIGHT], from))
        attacker_type = KNIGHT;
    else if (GET_BIT(pos->pieces[side][BISHOP], from))
        attacker_type = BISHOP;
    else if (GET_BIT(pos->pieces[side][ROOK], from))
        attacker_type = ROOK;
    else if (GET_BIT(pos->pieces[side][QUEEN], from))
        attacker_type = QUEEN;
    else if (GET_BIT(pos->pieces[side][KING], from))
        attacker_type = KING;

    // Manejo de promoción: el atacante "cambia" de valor para futuras recapturas
    if (GET_MOVE_PROMOTED(move))
        attacker_type = GET_MOVE_PROMOTED(move);

    // Primer paso: "Hacemos" el movimiento.
    // Restamos el valor del atacante al balance.
    // Si balance < 0, significa que el valor del atacante es mayor que la ganancia acumulada hasta ahora (victim - threshold).
    // Esto implica que el intercambio es favorable para nosotros (ganamos más de lo que arriesgamos al exponer el atacante).
    balance = see_values[attacker_type] - balance;
    
    if (balance < 0)
        return 1;

    // 3. Simular ocupación
    Bitboard occupancy = pos->all;
    occupancy &= ~(1ULL << from); // Quitar atacante de origen
    if (GET_MOVE_ENPASSANT(move))
    {
        int ep_sq = (side == WHITE) ? to - 8 : to + 8;
        occupancy &= ~(1ULL << ep_sq); // Quitar peón comido al paso
    }
    occupancy |= (1ULL << to); // Poner atacante en destino
    
    // Sliders para Rayos X (pre-calculados)
    Bitboard bishops = pos->pieces[WHITE][BISHOP] | pos->pieces[BLACK][BISHOP] | pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN];
    Bitboard rooks   = pos->pieces[WHITE][ROOK]   | pos->pieces[BLACK][ROOK]   | pos->pieces[WHITE][QUEEN] | pos->pieces[BLACK][QUEEN];

    // Calcular atacantes iniciales usando la ocupación simulada
    // (Corregido para usar occupancy en lugar de pos->all para sliders)
    Bitboard attackers = 0ULL;

    // Peones, Caballos, Reyes (independientes de ocupación)
    attackers |= (get_pawn_attacks(BLACK, to) & pos->pieces[WHITE][PAWN]);
    attackers |= (get_pawn_attacks(WHITE, to) & pos->pieces[BLACK][PAWN]);
    attackers |= (get_knight_attacks(to) & (pos->pieces[WHITE][KNIGHT] | pos->pieces[BLACK][KNIGHT]));
    attackers |= (get_king_attacks(to) & (pos->pieces[WHITE][KING] | pos->pieces[BLACK][KING]));

    // Sliders (usando occupancy simulada para ver a través de 'from')
    attackers |= (get_bishop_attacks(to, occupancy) & bishops);
    attackers |= (get_rook_attacks(to, occupancy) & rooks);
    
    // Quitar el que acaba de mover (sigue en pos->pieces[from])
    attackers &= ~(1ULL << from);
    
    int stm = side ^ 1; // Siguiente turno
    
    // 4. Bucle de intercambios
    while (1)
    {
        Bitboard my_attackers = attackers & pos->occupied[stm] & occupancy;
        if (!my_attackers)
            break;
        
        // Encontrar LVA (Least Valuable Attacker)
        int lva_sq = -1;
        int lva_type = -1;
        
        for (int p = PAWN; p <= KING; p++)
        {
            if (my_attackers & pos->pieces[stm][p])
            {
                lva_type = p;
                lva_sq = get_lsb_index(my_attackers & pos->pieces[stm][p]);
                break;
            }
        }
        
        if (lva_sq == -1)
            break;
        
        occupancy &= ~(1ULL << lva_sq); // Quitar pieza que recaptura

        // Actualizar balance
        balance = see_values[lva_type] - balance;
        
        // Si balance < 0, el bando actual (stm) ha hecho una captura buena (gana material o refuta el threshold).
        // Si stm != side (es el oponente), significa que ha refutado nuestro threshold -> return 0.
        // Si stm == side (somos nosotros), significa que hemos recuperado -> return 1.
        if (balance < 0)
            return (stm == side);
        
        // Rayos X: Añadir sliders descubiertos detrás de la pieza movida
        attackers |= (get_bishop_attacks(to, occupancy) & bishops);
        attackers |= (get_rook_attacks(to, occupancy) & rooks);
        attackers &= occupancy; // Eliminar piezas ya capturadas
        
        stm ^= 1;
    }
    
    // Si se acaban los atacantes, el bando al que le tocaba pierde la opción de capturar.
    // Ganamos si el último en mover fuimos nosotros (stm != side).
    return (stm != side);
}