/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

#include "common.h"
#include <time.h>
#include "tbprobe.h"

#ifdef _WIN32
#include <windows.h>
#define STRCASECMP _stricmp
#else
#include <sys/time.h>
#include <pthread.h>
#include <strings.h>
#define STRCASECMP strcasecmp
#endif

Position g_pos;                 // Instancia global del tablero
ATOMIC_INT stop_search = 0;     // Variable global para detener la búsqueda

// opciones generales
int opt_threads = 1;
int opt_ponder = 1;
int opt_multi_pv = 1;
char opt_syzygy_path[1024] = "";
int opt_syzygy_probe_limit = 7;
int opt_syzygy_probe_depth = 1;
int opt_syzygy_50move_rule = 1;
char opt_nnue_path[1024] = "net4.bin";

// QS
int opt_qs_poda_delta =             98;
int opt_qs_fut_margin =             70;
//int opt_qs_lmp =                    10;
int opt_qs_see =                    -85;

// Razoring
int opt_bus_razor_depth =           4;
int opt_bus_razor_margin1 =         75;
int opt_bus_razor_margin2 =         130;
int opt_bus_razor_margin3 =         315;
int opt_bus_razor_margin4 =         500;
int opt_bus_razor_margin5 =         700;
int opt_bus_fullraz_base =          350;
int opt_bus_fullraz_mult =          260;

// Poda JC
int opt_bus_podajc_depth =          10;
int opt_bus_podajc_margin =         50;
int opt_bus_podajc_cero =           -30;

// SEE Pruning con hist
int opt_bus_seep_depth =            5;
int opt_bus_seep_margin =           50;
int opt_bus_seep_div =              60;

// SEE Pruning Only Depth
int opt_bus_seepod_depth =          4;
int opt_bus_seepod_margin =         10;

// SEE de capturas (noisy)
int opt_bus_seep_noisy_depth =      3;
int opt_bus_seep_noisy_margin =     53;
int opt_bus_seep_noisy_hist_div =   225;
int opt_bus_seep_noisy_depth2 =     16;
int opt_bus_seep_noisy_margin2 =    169;

// Capture Futility Pruning
int opt_cfp_depth =                 4;
int opt_cfp_margin =                50;
int opt_cfp_mult =                  100;

// History Pruning
int opt_bus_hp_depth =              9;
int opt_bus_hp_margin =             0;

// Continuation History Pruning
int opt_bus_chp_depth =             4;
int opt_bus_chp_margin =            2000;

// Futility Pruning
int opt_bus_fut_depth =             6;
int opt_bus_fut_base =              25;
int opt_bus_fut_margin =            40;

// IID + IIR
int opt_bus_iid_depthmin =          10;
int opt_bus_iir_depth =             4;
int opt_bus_iir_pvred =             2;
int opt_bus_iir_cutred =            2;
int opt_bus_iir_allred =            0;

// LMP
int opt_lmp_base =                  9;
int opt_lmp_depth =                 10;

// Null Move
int opt_bus_nm_cutyall =            1;
int opt_bus_nm_r =                  4;
int opt_bus_nm_imp =                1;
int opt_bus_nm_div =                197;
int opt_bus_nm_verif =              8;

// LMR
int opt_lmr_base_depth =            2;
int opt_lmr_base_move =             4;
int opt_lmr_check_depth =           -3;
int opt_lmr_cut_depth =             3;
int opt_lmr_mejo_depth =            -3;
int opt_lmr_goodmove_margin =       230;
int opt_lmr_eval_depth =            4;
int opt_lmr_ttd_depth =             -1;
int opt_lmr_ttcapt_depth =          1;
int opt_lmr_hist_menos2 =           7974;
int opt_lmr_hist_menos1 =           3424;
int opt_lmr_hist_mas1 =             -2252;
int opt_lmr_chist_mas1 =            -240;
int opt_lmr_chist_menos1 =          135;

// ProbCut
int opt_probcut_depth =             7;
int opt_probcut_margin =            100;
int opt_probcut_reduction =         5;

// Singluar extension
int opt_ext_limit_multiplier =      5;
int opt_se_min_depth =              9;
int opt_se_tt_depth_margin =        1;
int opt_se_margin_base =            2;
int opt_se_margin_scale =           1;
int opt_se_double_margin =          58;
int opt_se_triple_margin =          143;

// Poda Hash Beta
int opt_poda_hash_beta_prof_dif =   14;
int opt_poda_hash_beta_base =       195;
int opt_poda_hash_beta_mult_dif =   14;
int opt_poda_hash_beta_mult_prof =  50;

// Poda Hash Alfa
int opt_poda_hash_alfa_prof_dif =   0;
int opt_poda_hash_alfa_base =       99;
int opt_poda_hash_alfa_mult_dif =   111;
int opt_poda_hash_alfa_mult_prof =  -10;



long long get_time_ms()
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
#endif
}

// Inicializa las tablas hash, bitboards, etc. al arrancar el programa.
void engine_init()
{
#ifdef USE_SMP
    printf("Info: Gilipol (Multi-thread / SMP)\n");
#else
    printf("Info: Gilipol (Single-thread)\n");
#endif
    printf("Info: Inicializando estructuras del motor...\n");
    init_bitboards();
    init_zobrist();
    init_tt();
    init_razor_margin();
    nnue_init(opt_nnue_path);
    // Inicializar el tablero con la posición inicial por defecto
    parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// Prepara el tablero para una nueva partida (limpia historial, etc.)
void engine_new_game()
{
    //printf("Info: Nueva partida preparada.\n");
    clear_tt();
    clear_search_heuristics(); // Limpiar continuation history y otras heurísticas
    game_history_count = 0;
    parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// Helper para convertir string a Move (necesario para aplicar movimientos de la GUI)
Move parse_move_string(Position* pos, char* move_str)
{
    MoveList moves;
    generate_moves(pos, &moves, GEN_ALL);
    
    int source = (move_str[0] - 'a') + ((move_str[1] - '1') * 8);
    int target = (move_str[2] - 'a') + ((move_str[3] - '1') * 8);
    int promoted = 0;
    
    if (strlen(move_str) > 4)
    {
        switch (move_str[4])
        {
            case 'n': promoted = KNIGHT; break;
            case 'b': promoted = BISHOP; break;
            case 'r': promoted = ROOK; break;
            case 'q': promoted = QUEEN; break;
        }
    }
    
    for (int i = 0; i < moves.count; i++)
    {
        Move move = moves.moves[i];
        if (GET_MOVE_SOURCE(move) == source
            && GET_MOVE_TARGET(move) == target
            && GET_MOVE_PROMOTED(move) == promoted)
        {
            return move;
        }
    }
    return MOVE_NONE;
}

// Configura el tablero.
// fen: Cadena FEN (o NULL si es startpos).
// moves: Cadena con la lista de movimientos a aplicar después del FEN (o NULL).
void engine_set_position(const char* fen, char* moves)
{
    game_history_count = 0; // Resetear historial
    if (fen)
        parse_fen(&g_pos, fen);
    else
        parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    
    // Añadir posición inicial al historial
    if (game_history_count < MAX_GAME_HISTORY)
        game_history[game_history_count++] = g_pos.hash;
    else
        printf("info string Warning: Game history full (%d positions), cannot add initial position\n", MAX_GAME_HISTORY);

    if (moves)
    {
        char* token = strtok(moves, " ");
        while (token != NULL)
        {
            Move move = parse_move_string(&g_pos, token);
            if (move != MOVE_NONE)
            {
                make_move(&g_pos, move);
                // Añadir al historial
                if (game_history_count < MAX_GAME_HISTORY)
                {
                    game_history[game_history_count++] = g_pos.hash;
                }
                else
                {
                    printf("info string Warning: Game history full (%d positions), skipping hash storage\n", MAX_GAME_HISTORY);
                }
            }
            token = strtok(NULL, " ");
        }
    }
    g_pos.prev = NULL; // La raíz no tiene previo en la búsqueda
}

// Wrapper para el hilo de búsqueda
#ifdef _WIN32
DWORD WINAPI search_thread(LPVOID lpParam)
{
    SearchLimits* limits = (SearchLimits*)lpParam;
    search_position(&g_pos, *limits);
    free(limits);
    return 0;
}
#else
void* search_thread(void* lpParam)
{
    SearchLimits* limits = (SearchLimits*)lpParam;
    search_position(&g_pos, *limits);
    free(limits);
    return NULL;
}
#endif

/*
 *
 * engine_go
 * 
 */
void engine_go(SearchLimits limits)
{
    ATOMIC_STORE(stop_search, 0);

    // Crear una copia de los límites para pasar al hilo
    SearchLimits* limits_ptr = malloc(sizeof(SearchLimits));
    *limits_ptr = limits;

#ifdef _WIN32
    HANDLE hThread = CreateThread(NULL, 0, search_thread, limits_ptr, 0, NULL);
    if (hThread != NULL)
        CloseHandle(hThread); // Cerrar handle inmediatamente para evitar leak
#else
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, search_thread, limits_ptr);
    pthread_detach(thread_id); // Detach para liberar recursos automáticamente
#endif
}

// Detiene la búsqueda inmediatamente si está corriendo.
void engine_stop()
{
    printf("Info: Deteniendo búsqueda.\n");
    fflush(stdout);
    ATOMIC_STORE(stop_search, 1);
}

// Imprime el tablero actual (útil para debug, no estrictamente UCI).
void engine_print()
{
    print_board(&g_pos);
}

long long nodes;

// Función recursiva para contar nodos
void perft_driver(int depth)
{
    if (depth == 0)
    {
        nodes++;
        return;
    }

    MoveList move_list;
    generate_moves(&g_pos, &move_list, GEN_ALL);

    Position backup; // Copia de seguridad

    for (int i = 0; i < move_list.count; i++)
    {
        copy_position(&backup, &g_pos); // Guardar estado
        
        if (!make_move(&g_pos, move_list.moves[i]))
        {
            // Movimiento ilegal (rey en jaque), deshacer (restaurar copia) y continuar
            copy_position(&g_pos, &backup);
            continue;
        }
        
        perft_driver(depth - 1);
        
        // Deshacer movimiento (restaurar copia)
        copy_position(&g_pos, &backup);
    }
}

void perft_test(int depth)
{
    printf("\nInfo: Iniciando Perft Test a profundidad %d...\n", depth);
    nodes = 0;
    clock_t start = clock();
    perft_driver(depth);
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Info: Nodos: %lld, Tiempo: %.3fs, NPS: %.0f\n", nodes, time_spent, nodes / time_spent);
}

/*
 *
 * parse_go (parsea el comando 'go' y llena la estructura SearchLimits)
 * 
 */
void parse_go(char* params)
{
    SearchLimits limits;
    memset(&limits, 0, sizeof(SearchLimits)); // Inicializar a 0

    if (!params)
    {
        limits.infinite = true;
        engine_go(limits);
        return;
    }

    char *token = strtok(params, " ");
    while (token != NULL)
    {
        if (strcmp(token, "wtime") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.wtime = atoi(token);
        }
        else if (strcmp(token, "btime") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.btime = atoi(token);
        }
        else if (strcmp(token, "winc") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.winc = atoi(token);
        }
        else if (strcmp(token, "binc") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.binc = atoi(token);
        }
        else if (strcmp(token, "movestogo") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.movestogo = atoi(token);
        }
        else if (strcmp(token, "depth") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.depth = atoi(token);
        }
        else if (strcmp(token, "movetime") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.movetime = atoi(token);
        }
        else if (strcmp(token, "infinite") == 0)
        {
            limits.infinite = true;
        }
        else if (strcmp(token, "ponder") == 0)
        {
            limits.ponder = true;
        }
        token = strtok(NULL, " ");
    }

    engine_go(limits);
}

/*
 *
 * parse_position (position startpos moves e2e4 e7e5; position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 moves e2e4)
 * 
 */
void parse_position(char* params)
{
    if (!params)
        return;

    char* fen = NULL;
    char* moves = NULL;

    // Buscar si hay movimientos
    char* moves_ptr = strstr(params, "moves");
    
    if (moves_ptr)
    {
        *moves_ptr = '\0'; // Cortar el string aquí temporalmente
        moves = moves_ptr + 5; // Saltar la palabra "moves"
        while (*moves == ' ')
            moves++; // Saltar espacios en blanco
    }

    // Determinar si es startpos o fen
    if (strstr(params, "startpos"))
    {
        // posición inicial estándar
        fen = NULL;
    }
    else if (strstr(params, "fen"))
    {
        // Saltar "fen "
        fen = params + 4;
    }

    engine_set_position(fen, moves);
}

/*
 *
 * parse_option
 * 
 */
void parse_option(char* params)
{
    //
    // Determinar qué opción es
    //
    char* name_ptr = strstr(params, "name");
    if (!name_ptr)
        return;
    name_ptr += 5; // Skip "name "
    while (*name_ptr == ' ')
        name_ptr++;

    char* value_ptr = strstr(params, "value");
    char name[256] = {0};
    char value[1024] = {0};

    if (value_ptr)
    {
        size_t name_len = value_ptr - name_ptr;
        while (name_len > 0 && name_ptr[name_len - 1] == ' ')
            name_len--;
        if (name_len >= sizeof(name))
            name_len = sizeof(name) - 1;
        strncpy(name, name_ptr, name_len);
        
        value_ptr += 6; // Skip "value "
        while (*value_ptr == ' ')
            value_ptr++;
        strncpy(value, value_ptr, sizeof(value) - 1);
        value[strcspn(value, "\n")] = 0;
        value[strcspn(value, "\r")] = 0;
    }
    else
    {
        strncpy(name, name_ptr, sizeof(name) - 1);
        name[strcspn(name, "\n")] = 0;
        name[strcspn(name, "\r")] = 0;
    }

    //
    // Hash: implementada
    //
    if (STRCASECMP(name, "Hash") == 0)
    {
        resize_tt(atoi(value));
    }

    //
    // Threads: solo en compilación SMP
    //
#ifdef USE_SMP
    else if (STRCASECMP(name, "Threads") == 0)
    {
        opt_threads = atoi(value);
        if (opt_threads < 1)
            opt_threads = 1;
    }
#endif

    else if (STRCASECMP(name, "Ponder") == 0)
    {
        opt_ponder = (STRCASECMP(value, "true") == 0);
    }
    else if (STRCASECMP(name, "MultiPV") == 0)
    {
        // MultiPV: no tengo la menor intención de implementarlo
        opt_multi_pv = atoi(value);
        if (opt_multi_pv < 1)
            opt_multi_pv = 1;
    }
    else if (STRCASECMP(name, "SyzygyPath") == 0)
    {
        strncpy(opt_syzygy_path, value, sizeof(opt_syzygy_path) - 1);
        tb_init(opt_syzygy_path);
    }
    else if (STRCASECMP(name, "EvalFile") == 0)
    {
        strncpy(opt_nnue_path, value, sizeof(opt_nnue_path) - 1);
        nnue_init(opt_nnue_path);
    }
    else if (STRCASECMP(name, "SyzygyProbeDepth") == 0)
    {
        opt_syzygy_probe_depth = atoi(value);
    }
    else if (STRCASECMP(name, "Syzygy50MoveRule") == 0)
    {
        opt_syzygy_50move_rule = (STRCASECMP(value, "true") == 0);
    }
    else if (STRCASECMP(name, "SyzygyProbeLimit") == 0)
    {
        opt_syzygy_probe_limit = atoi(value);
    }
    else if (STRCASECMP(name, "QSPodaDelta") == 0)
    {
        int val = atoi(value);
        opt_qs_poda_delta = val;
    }
    else if (STRCASECMP(name, "QSFutMargin") == 0)
    {
        opt_qs_fut_margin = atoi(value);
    }
    //else if (STRCASECMP(name, "QSLMP") == 0)
    //{
    //    opt_qs_lmp = atoi(value);
    //}
    else if (STRCASECMP(name, "QSSEE") == 0)
    {
        opt_qs_see = atoi(value);
    }
    else if (STRCASECMP(name, "ExtensionLimitMultiplier") == 0)
    {
        int val = atoi(value);
        opt_ext_limit_multiplier = val;
    }
    else if (STRCASECMP(name, "LMPBase") == 0)
    {
        int val = atoi(value);
        opt_lmp_base = val;
    }
    else if (STRCASECMP(name, "LMPDepth") == 0)
    {
        int val = atoi(value);
        opt_lmp_depth = val;
    }
    else if (STRCASECMP(name, "LMRGoodMoveMargin") == 0)
    {
        int val = atoi(value);
        opt_lmr_goodmove_margin = val;
    }
    else if (STRCASECMP(name, "BusNMR") == 0)
    {
        opt_bus_nm_r = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMVerif") == 0)
    {
        opt_bus_nm_verif = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMCutYAll") == 0)
    {
        opt_bus_nm_cutyall = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMDiv") == 0)
    {
        opt_bus_nm_div = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMImp") == 0)
    {
        opt_bus_nm_imp = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIDDepthMin") == 0)
    {
        opt_bus_iid_depthmin = atoi(value);
    }
    else if (STRCASECMP(name, "BusPodaJCDepth") == 0)
    {
        opt_bus_podajc_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusPodaJCMargin") == 0)
    {
        opt_bus_podajc_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusPodaJCCero") == 0)
    {
        opt_bus_podajc_cero = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRDepth") == 0)
    {
        opt_bus_iir_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRPVRed") == 0)
    {
        opt_bus_iir_pvred = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRCutRed") == 0)
    {
        opt_bus_iir_cutred = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRAllRed") == 0)
    {
        opt_bus_iir_allred = atoi(value);
    }
    else if (STRCASECMP(name, "BusRazorDepth") == 0)
    {
        opt_bus_razor_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusRazorMargin1") == 0)
    {
        opt_bus_razor_margin1 = atoi(value);
        init_razor_margin();
    }
    else if (STRCASECMP(name, "BusRazorMargin2") == 0)
    {
        opt_bus_razor_margin2 = atoi(value);
        init_razor_margin();
    }
    else if (STRCASECMP(name, "BusRazorMargin3") == 0)
    {
        opt_bus_razor_margin3 = atoi(value);
        init_razor_margin();
    }
    else if (STRCASECMP(name, "BusRazorMargin4") == 0)
    {
        opt_bus_razor_margin4 = atoi(value);
        init_razor_margin();
    }
    else if (STRCASECMP(name, "BusRazorMargin5") == 0)
    {
        opt_bus_razor_margin5 = atoi(value);
        init_razor_margin();
    }
    else if (STRCASECMP(name, "BusFullrazBase") == 0)
    {
        opt_bus_fullraz_base = atoi(value);
    }
    else if (STRCASECMP(name, "BusFullrazMult") == 0)
    {
        opt_bus_fullraz_mult = atoi(value);
    }
    else if (STRCASECMP(name, "LMRBaseDepth") == 0)
    {
        opt_lmr_base_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRBaseMove") == 0)
    {
        opt_lmr_base_move = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashBetaProfDif") == 0)
    {
        opt_poda_hash_beta_prof_dif = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashBetaBase") == 0)
    {
        opt_poda_hash_beta_base = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashBetaMultDif") == 0)
    {
        opt_poda_hash_beta_mult_dif = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashBetaMultProf") == 0)
    {
        opt_poda_hash_beta_mult_prof = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashAlfaProfDif") == 0)
    {
        opt_poda_hash_alfa_prof_dif = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashAlfaBase") == 0)
    {
        opt_poda_hash_alfa_base = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashAlfaMultDif") == 0)
    {
        opt_poda_hash_alfa_mult_dif = atoi(value);
    }
    else if (STRCASECMP(name, "PodaHashAlfaMultProf") == 0)
    {
        opt_poda_hash_alfa_mult_prof = atoi(value);
    }
    else if (STRCASECMP(name, "BusFutDepth") == 0)
    {
        opt_bus_fut_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusFutBase") == 0)
    {
        opt_bus_fut_base = atoi(value);
    }
    else if (STRCASECMP(name, "BusFutMargin") == 0)
    {
        opt_bus_fut_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusHPDepth") == 0)
    {
        opt_bus_hp_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusHPMargin") == 0)
    {
        opt_bus_hp_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusCHPDepth") == 0)
    {
        opt_bus_chp_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusCHPMargin") == 0)
    {
        opt_bus_chp_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepDepth") == 0)
    {
        opt_bus_seep_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepMargin") == 0)
    {
        opt_bus_seep_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepDiv") == 0)
    {
        opt_bus_seep_div = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepodDepth") == 0)
    {
        opt_bus_seepod_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepodMargin") == 0)
    {
        opt_bus_seepod_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepNoisyDepth") == 0)
    {
        opt_bus_seep_noisy_depth = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepNoisyMargin") == 0)
    {
        opt_bus_seep_noisy_margin = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepNoisyHistDiv") == 0)
    {
        opt_bus_seep_noisy_hist_div = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepNoisyDepth2") == 0)
    {
        opt_bus_seep_noisy_depth2 = atoi(value);
    }
    else if (STRCASECMP(name, "BusSeepNoisyMargin2") == 0)
    {
        opt_bus_seep_noisy_margin2 = atoi(value);
    }
    else if (STRCASECMP(name, "CFPDepth") == 0)
    {
        opt_cfp_depth = atoi(value);
    }
    else if (STRCASECMP(name, "CFPMargin") == 0)
    {
        opt_cfp_margin = atoi(value);
    }
    else if (STRCASECMP(name, "CFPMult") == 0)
    {
        opt_cfp_mult = atoi(value);
    }
    else if (STRCASECMP(name, "ProbcutDepth") == 0)
    {
        opt_probcut_depth = atoi(value);
    }
    else if (STRCASECMP(name, "ProbcutMargin") == 0)
    {
        opt_probcut_margin = atoi(value);
    }
    else if (STRCASECMP(name, "ProbcutReduction") == 0)
    {
        opt_probcut_reduction = atoi(value);
    }
    else if (STRCASECMP(name, "LMRCutDepth") == 0)
    {
        opt_lmr_cut_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRCheckDepth") == 0)
    {
        opt_lmr_check_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRMejoDepth") == 0)
    {
        opt_lmr_mejo_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMREvalDepth") == 0)
    {
        opt_lmr_eval_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTTDDepth") == 0)
    {
        opt_lmr_ttd_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTTCaptDepth") == 0)
    {
        opt_lmr_ttcapt_depth = atoi(value);
    }
    else if (STRCASECMP(name, "LMRHistMenos2") == 0)
    {
        opt_lmr_hist_menos2 = atoi(value);
    }
    else if (STRCASECMP(name, "LMRHistMenos1") == 0)
    {
        opt_lmr_hist_menos1 = atoi(value);
    }
    else if (STRCASECMP(name, "LMRHistMas1") == 0)
    {
        opt_lmr_hist_mas1 = atoi(value);
    }
    else if (STRCASECMP(name, "LMRCHistMas1") == 0)
    {
        opt_lmr_chist_mas1 = atoi(value);
    }
    else if (STRCASECMP(name, "LMRCHistMenos1") == 0)
    {
        opt_lmr_chist_menos1 = atoi(value);
    }
	else if (STRCASECMP(name, "SEMinDepth") == 0)
    {
        opt_se_min_depth = atoi(value);
    }
    else if (STRCASECMP(name, "SETTDepthMargin") == 0)
    {
        opt_se_tt_depth_margin = atoi(value);
    }
    else if (STRCASECMP(name, "SEMarginBase") == 0)
    {
        opt_se_margin_base = atoi(value);
    }
    else if (STRCASECMP(name, "SEMarginScale") == 0)
    {
        opt_se_margin_scale = atoi(value);
    }
    else if (STRCASECMP(name, "SEDoubleMargin") == 0)
    {
        opt_se_double_margin = atoi(value);
    }
    else if (STRCASECMP(name, "SETripleMargin") == 0)
    {
        opt_se_triple_margin = atoi(value);
    }
}

int main()
{
    char line[4096]; // Buffer para leer líneas de la entrada estándar
    
    // Inicialización básica
    engine_init();

    // Bucle principal: leer stdin línea por línea
    while (fgets(line, sizeof(line), stdin))
    {
        // Eliminar el salto de línea al final
        line[strcspn(line, "\n")] = 0;
        
        // Saltar líneas vacías
        if (strlen(line) == 0)
            continue;

        // Extraer el comando principal (primer token)
        char* command = strtok(line, " ");
        
        // Puntero al resto de los parámetros (si existen)
        char* params = strtok(NULL, ""); 

        if (strcmp(command, "uci") == 0)
        {
            printf("id name Gilipol 1.02 Full Razoring\n");
            printf("id author Jose Carlos Martinez Galan\n");
            printf("option name Hash type spin default 32 min 1 max 1024\n");
            #ifdef USE_SMP
                printf("option name Threads type spin default 1 min 1 max 128\n");
            #endif
            printf("option name Ponder type check default true\n");
            printf("option name MultiPV type spin default 1 min 1 max 1\n");
            printf("option name SyzygyPath type string default <empty>\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
            printf("option name EvalFile type string default %s\n", opt_nnue_path);

            #if (OPTIMIZAR_QS)
                printf("option name QSPodaDelta type spin default %d min 0 max 150\n", opt_qs_poda_delta);
                printf("option name QSFutMargin type spin default %d min 0 max 200\n", opt_qs_fut_margin);
                //printf("option name QSLMP type spin default %d min 2 max 10\n", opt_qs_lmp);
                printf("option name QSSEE type spin default %d min -100 max 10\n", opt_qs_see);
            #endif

            #if (OPTIMIZAR_RAZOR)
                printf("option name BusRazorDepth type spin default %d min 1 max 15\n", opt_bus_razor_depth);           // 4
                printf("option name BusRazorMargin1 type spin default %d min 0 max 200\n", opt_bus_razor_margin1);      // 75
                printf("option name BusRazorMargin2 type spin default %d min 20 max 400\n", opt_bus_razor_margin2);     // 130
                printf("option name BusRazorMargin3 type spin default %d min 40 max 600\n", opt_bus_razor_margin3);     // 315
                printf("option name BusRazorMargin4 type spin default %d min 60 max 1000\n", opt_bus_razor_margin4);    // 500
                printf("option name BusRazorMargin5 type spin default %d min 500 max 1200\n", opt_bus_razor_margin5);
                printf("option name BusFullrazBase type spin default %d min 0 max 2000\n", opt_bus_fullraz_base);       // 350
                printf("option name BusFullrazMult type spin default %d min 0 max 1000\n", opt_bus_fullraz_mult);       // 260
            #endif

            #if (OPTIMIZAR_PODAJC)
                printf("option name BusPodaJCDepth type spin default %d min 3 max 15\n", opt_bus_podajc_depth);
                printf("option name BusPodaJCMargin type spin default %d min 50 max 200\n", opt_bus_podajc_margin);
                printf("option name BusPodaJCCero type spin default %d min -100 max 100\n", opt_bus_podajc_cero);
            #endif

            #if (OPTIMIZAR_SEEP)
                printf("option name BusSeepDepth type spin default %d min 1 max 15\n", opt_bus_seep_depth);
                printf("option name BusSeepMargin type spin default %d min 0 max 200\n", opt_bus_seep_margin);
                printf("option name BusSeepDiv type spin default %d min 1 max 200\n", opt_bus_seep_div);
            #endif

            #if (OPTIMIZAR_SEEPOD)
                printf("option name BusSeepodDepth type spin default %d min 1 max 15\n", opt_bus_seepod_depth);
                printf("option name BusSeepodMargin type spin default %d min 0 max 100\n", opt_bus_seepod_margin);
            #endif

            #if (OPTIMIZAR_CSEEP)
                printf("option name BusSeepNoisyDepth type spin default %d min 1 max 20\n", opt_bus_seep_noisy_depth);
                printf("option name BusSeepNoisyMargin type spin default %d min 1 max 100\n", opt_bus_seep_noisy_margin);
                printf("option name BusSeepNoisyHistDiv type spin default %d min 1 max 300\n", opt_bus_seep_noisy_hist_div);
                printf("option name BusSeepNoisyDepth2 type spin default %d min 1 max 20\n", opt_bus_seep_noisy_depth2);
                printf("option name BusSeepNoisyMargin2 type spin default %d min 1 max 500\n", opt_bus_seep_noisy_margin2);
            #endif

            #if (OPTIMIZAR_CFP)
                printf("option name CFPDepth type spin default %d min 1 max 15\n", opt_cfp_depth);
                printf("option name CFPMargin type spin default %d min 0 max 100\n", opt_cfp_margin);
                printf("option name CFPMult type spin default %d min 0 max 200\n", opt_cfp_mult);
            #endif

            #if (OPTIMIZAR_HP)
                printf("option name BusHPDepth type spin default %d min 0 max 15\n", opt_bus_hp_depth);
                printf("option name BusHPMargin type spin default %d min 0 max 100\n", opt_bus_hp_margin);
            #endif

            #if (OPTIMIZAR_CHP)
                printf("option name BusCHPDepth type spin default %d min 1 max 15\n", opt_bus_chp_depth);
                printf("option name BusCHPMargin type spin default %d min 0 max 5000\n", opt_bus_chp_margin);
            #endif

            #if (OPTIMIZAR_FP)
7                printf("option name BusFutDepth type spin default %d min 1 max 10\n", opt_bus_fut_depth);
                printf("option name BusFutBase type spin default %d min 0 max 500\n", opt_bus_fut_base);
                printf("option name BusFutMargin type spin default %d min 0 max 200\n", opt_bus_fut_margin);
            #endif
            
            #if (OPTIMIZAR_IIDR)
                printf("option name BusIIDDepthMin type spin default %d min 6 max 20\n", opt_bus_iid_depthmin);
                printf("option name BusIIRDepth type spin default %d min 2 max 6\n", opt_bus_iir_depth);
                printf("option name BusIIRPVRed type spin default %d min 0 max 2\n", opt_bus_iir_pvred);
                printf("option name BusIIRCutRed type spin default %d min 0 max 2\n", opt_bus_iir_cutred);
                printf("option name BusIIRAllRed type spin default %d min 0 max 1\n", opt_bus_iir_allred);
            #endif

            #if (OPTIMIZAR_LMP)
                printf("option name LMPBase type spin default %d min 1 max 10\n", opt_lmp_base);
                printf("option name LMPDepth type spin default %d min 1 max 10\n", opt_lmp_depth);
            #endif

            #if (OPTIMIZAR_NM)
                printf("option name BusNMCutYAll type spin default %d min 0 max 1\n", opt_bus_nm_cutyall);
                printf("option name BusNMR type spin default %d min 1 max 5\n", opt_bus_nm_r);
                printf("option name BusNMImp type spin default %d min 0 max 1\n", opt_bus_nm_imp);
                printf("option name BusNMDiv type spin default %d min 50 max 200\n", opt_bus_nm_div);
                printf("option name BusNMVerif type spin default %d min 8 max 20\n", opt_bus_nm_verif);
            #endif

            #if (OPTIMIZAR_LMR)
                printf("option name LMRBaseDepth type spin default %d min 1 max 4\n", opt_lmr_base_depth);
                printf("option name LMRBaseMove type spin default %d min 2 max 4\n", opt_lmr_base_move);
                printf("option name LMRCheckDepth type spin default %d min -6 max 0\n", opt_lmr_check_depth);
                printf("option name LMRCutDepth type spin default %d min 0 max 6\n", opt_lmr_cut_depth);
                printf("option name LMRMejoDepth type spin default %d min -6 max 0\n", opt_lmr_mejo_depth);
                printf("option name LMRGoodMoveMargin type spin default %d min 100 max 300\n", opt_lmr_goodmove_margin);
                printf("option name LMREvalDepth type spin default %d min 0 max 6\n", opt_lmr_eval_depth);
                printf("option name LMRTTDDepth type spin default %d min -6 max 0\n", opt_lmr_ttd_depth);
                printf("option name LMRTTCaptDepth type spin default %d min 0 max 6\n", opt_lmr_ttcapt_depth);
                printf("option name LMRHistMenos2 type spin default %d min 6000 max 10000\n", opt_lmr_hist_menos2);
                printf("option name LMRHistMenos1 type spin default %d min 2500 max 4500\n", opt_lmr_hist_menos1);
                printf("option name LMRHistMas1 type spin default %d min -3500 max -1500\n", opt_lmr_hist_mas1);
                printf("option name LMRCHistMas1 type spin default %d min -400 max -100\n", opt_lmr_chist_mas1);
                printf("option name LMRCHistMenos1 type spin default %d min 0 max 300\n", opt_lmr_chist_menos1);
            #endif

            #if (OPTIMIZAR_PBCT)
                printf("option name ProbcutDepth type spin default %d min 3 max 7\n", opt_probcut_depth);
                printf("option name ProbcutMargin type spin default %d min 100 max 300\n", opt_probcut_margin);
                printf("option name ProbcutReduction type spin default %d min 3 max 5\n", opt_probcut_reduction);
            #endif

            #if (OPTIMIZAR_SE)
                printf("option name ExtensionLimitMultiplier type spin default %d min 1 max 30\n", opt_ext_limit_multiplier);
                printf("option name SEMinDepth type spin default %d min 1 max 10\n", opt_se_min_depth);
                printf("option name SETTDepthMargin type spin default %d min 1 max 5\n", opt_se_tt_depth_margin);
                printf("option name SEMarginBase type spin default %d min 0 max 50\n", opt_se_margin_base);
                printf("option name SEMarginScale type spin default %d min 0 max 6\n", opt_se_margin_scale);
                printf("option name SEDoubleMargin type spin default %d min 0 max 100\n", opt_se_double_margin);
                printf("option name SETripleMargin type spin default %d min 50 max 200\n", opt_se_triple_margin);
            #endif

            #if (OPTIMIZAR_PHB)
                printf("option name PodaHashBetaProfDif type spin default %d min 1 max 20\n", opt_poda_hash_beta_prof_dif);
                printf("option name PodaHashBetaBase type spin default %d min 0 max 300\n", opt_poda_hash_beta_base);
                printf("option name PodaHashBetaMultDif type spin default %d min -10 max 50\n", opt_poda_hash_beta_mult_dif);
                printf("option name PodaHashBetaMultProf type spin default %d min -10 max 80\n", opt_poda_hash_beta_mult_prof);
            #endif

            #if (OPTIMIZAR_PHA)
                printf("option name PodaHashAlfaProfDif type spin default %d min 0 max 20\n", opt_poda_hash_alfa_prof_dif);
                printf("option name PodaHashAlfaBase type spin default %d min 0 max 500\n", opt_poda_hash_alfa_base);
                printf("option name PodaHashAlfaMultDif type spin default %d min -200 max 200\n", opt_poda_hash_alfa_mult_dif);
                printf("option name PodaHashAlfaMultProf type spin default %d min -200 max 200\n", opt_poda_hash_alfa_mult_prof);
            #endif
            
            printf("uciok\n");
        }
        else if (strcmp(command, "isready") == 0)
        {
            printf("readyok\n");        
        }
        else if (strcmp(command, "ucinewgame") == 0)
        {
            engine_new_game();        
        }
        else if (strcmp(command, "position") == 0)
        {
            parse_position(params);
        
        }
        else if (strcmp(command, "go") == 0)
        {
            parse_go(params);        
        }
        else if (strcmp(command, "ponderhit") == 0)
        {
            engine_ponderhit();
        }
        else if (strcmp(command, "setoption") == 0)
        {
            parse_option(params);
        }
        else if (strcmp(command, "stop") == 0)
        {
            engine_stop();        
        }
        else if (strcmp(command, "quit") == 0)
        {
            break; // Salir del bucle y terminar el programa        
        }
        else if (strcmp(command, "d") == 0)
        {
            // Comando no estándar pero útil para debug
            engine_print();
        }
        else if (strcmp(command, "perft") == 0)
        {
            int depth = 1;
            if (params)
                depth = atoi(params);
            perft_test(depth);
        }
        else if (strcmp(command, "eval") == 0)
        {
            int score = nnue_evaluate(&g_pos);
            printf("Eval: %d\n", score);
        }
        
        // Es importante hacer flush de stdout para asegurar que la GUI reciba los comandos
        fflush(stdout);
    }

    return 0;
}
