/*
 *  Gilipol
 *
 *  2026
 *  José Carlos Martínez Galán
 */

#include "common.h"
#include <time.h>
#include <ctype.h>
#include "tbprobe.h"

#ifdef _WIN32
#include <windows.h>
#define STRCASECMP _stricmp
#else
#include <sys/time.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#define STRCASECMP strcasecmp
#endif

static void resolver_junto_al_exe(char* ruta, size_t tam);
static void esperar_fin_busqueda(void);

Position g_pos;                 // Instancia global del tablero
SHARED_INT stop_search = 0;     // Parar la busqueda (GUI, limite de nodos o de tiempo)
SHARED_INT stop_gui = 0;        // Parada pedida por el GUI: la unica que acaba infinite/ponder (B13)
SHARED_INT is_searching = 0;    // Hay una busqueda activa
int opt_hash_mb = 32;           // Hash configurado: el bench lo restaura al acabar (B16)
static int posicion_valida = 1; // 0 tras un FEN invalido: no se busca hasta otra posicion

// opciones generales
int opt_threads = 1;
int opt_ponder = 1;
int opt_multi_pv = 1;
char opt_syzygy_path[1024] = "";
int opt_syzygy_probe_limit = 7;
int opt_syzygy_probe_depth = 1;
int opt_syzygy_50move_rule = 1;
char opt_nnue_path[1024] = RED_POR_DEFECTO;

// QS
int opt_qs_poda_delta =             91;
int opt_qs_fut_margin =             86;
int opt_qs_see =                    -66;
int opt_qs_improve =                0;
int opt_qs_stable =                 0;
int opt_qs_worsen =                 0;
//int opt_qs_lmp =                    10;

// Razoring
int opt_bus_razor_depth =           4;
int opt_bus_razor_margin1 =         110;
int opt_bus_razor_margin2 =         230;
int opt_bus_razor_margin3 =         420;
int opt_bus_razor_margin4 =         930;
int opt_bus_razor_margin5 =         1200;
int opt_bus_fullraz_base =          380;
int opt_bus_fullraz_mult =          260;
int opt_bus_fullraz_improve =       0;
int opt_bus_fullraz_worsen =        -30;

// Poda JC
int opt_bus_podajc_depth =          9;
int opt_bus_podajc_margin =         84;
int opt_bus_podajc_cero =           -46;
int opt_bus_podajc_ifworsen =       0;

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
int opt_bus_hp_depth =              8;
int opt_bus_hp_margin =             961;

// Continuation History Pruning
int opt_bus_chp_depth =             4;
int opt_bus_chp_margin =            2000;

// Futility Pruning
int opt_bus_fut_depth =             6;
int opt_bus_fut_base =              25;
int opt_bus_fut_margin =            40;

// IID + IIR
int opt_bus_iid_depthmin =          9;
int opt_bus_iid_improve =           60;
int opt_bus_iid_worsen =            40;
int opt_bus_iir_depth =             3;
int opt_bus_iir_pvred =             1;
int opt_bus_iir_cutred =            1;
int opt_bus_iir_allred =            1;
int opt_bus_iir_pv_stable =         0;
int opt_bus_iir_cut_improve =       0;
int opt_bus_iir_all_worsen =        -1;

// LMP
int opt_lmp_base =                  9;
int opt_lmp_depth =                 10;

// Null Move
int opt_bus_nm_cutyall =            1;
int opt_bus_nm_r =                  4;   // = BUS_NM_R (antes 0, fuera de [3,5]: I91)
int opt_bus_nm_imp =                0;
int opt_bus_nm_div =                229;
int opt_bus_nm_verif =              15;
int opt_bus_nm_ifworsen =           1;
int opt_bus_nm_willimprove =        20;
int opt_bus_nm_isstable =           25;
int opt_bus_nm_willworsen =         5;

// Clasificador tactico
int  opt_tc_threshold =             82;
int  opt_tc_dir_min_depth =          0;
char opt_tc_path[1024] =            TC_POR_DEFECTO;

// Correction history
int opt_corr_grain =                256;
int opt_corr_limit =                8173;
int opt_corr_peso =                 16;

// LMR
int opt_lmr_base_depth =            1;
int opt_lmr_base_move =             4;
int opt_lmr_check_depth =           -1;
int opt_lmr_cut_depth =             5;
int opt_lmr_mejo_depth =            -1;
int opt_lmr_goodmove_margin =       230;
int opt_lmr_eval_depth =            0;
int opt_lmr_ttd_depth =             -1;
int opt_lmr_ttcapt_depth =          1;
int opt_lmr_hist_menos2 =           8004;
int opt_lmr_hist_menos1 =           3432;
int opt_lmr_hist_mas1 =             -1936;
int opt_lmr_chist_mas1 =            -235;
int opt_lmr_chist_menos1 =          140;
int opt_lmr_tc_willimprove =        -1;
int opt_lmr_tc_willworsen =         1;
int opt_lmr_tc_isstable =           0;
int opt_lmr_tc_wllimpbeta =         -1;
int opt_lmr_tc_wilworalpha =        1;

// ProbCut
int opt_probcut_depth =             4;
int opt_probcut_margin =            90;
int opt_probcut_reduction =         6;
int opt_probcut_willimprove =       20;
int opt_probcut_isstable =          40;
int opt_probcut_willworsen =        5;

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
    // OJO: GetTickCount64() tiene una granularidad de ~15,6 ms (medido en esta maquina:
    // 15,35 ms). Con presupuestos de 0,1-1 s por jugada eso es un error del 1,5 % al 15 %,
    // suficiente para desbaratar la gestion de tiempo y para envenenar cualquier ajuste de
    // parametros a control de tiempo rapido. QueryPerformanceCounter da 0,1 us.
    static LARGE_INTEGER frecuencia = {0};
    LARGE_INTEGER ahora;

    if (frecuencia.QuadPart == 0)
        QueryPerformanceFrequency(&frecuencia);

    QueryPerformanceCounter(&ahora);

    // div + mod en vez de (ahora * 1000) para no desbordar int64 con uptimes largos
    return (ahora.QuadPart / frecuencia.QuadPart) * 1000LL
         + ((ahora.QuadPart % frecuencia.QuadPart) * 1000LL) / frecuencia.QuadPart;
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
    printf("info string Gilipol (multihilo / SMP)\n");
#else
    printf("info string Gilipol (monohilo)\n");
#endif
    printf("info string CPU: %s\n", nombre_cpu());
    printf("info string Inicializando estructuras del motor...\n");
    init_bitboards();
    init_zobrist();
    init_tt();
    init_razor_margin();
    // Las redes van dentro del ejecutable: no hace falta ningun fichero al lado
    nnue_init_interna();
#if (USA_TC)
    nnue_tc_init_interna();
#else
    printf("info string Clasificador tactico desactivado (compilado con USA_TC=0)\n");
#endif
    // Inicializar el tablero con la posición inicial por defecto
    parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// Para la busqueda en curso, si la hay, y espera a que termine. Antes de tocar
// g_pos, la TT, la red o las heuristicas: si no, el hilo de busqueda lee
// estructuras que se estan cambiando debajo (I12, I24, I25).
static void esperar_fin_busqueda(void)
{
    if (!ATOMIC_LOAD_ACQ(is_searching))
        return;
    ATOMIC_STORE(stop_gui, 1);
    ATOMIC_STORE(stop_search, 1);
    while (ATOMIC_LOAD_ACQ(is_searching))
    {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}

// Si un fichero de red no esta en el directorio de trabajo y la ruta es relativa,
// probar junto al ejecutable. Sin esto, lanzado desde otra carpeta el motor jugaba
// con eval 0 sin decir nada (I09).
static void resolver_junto_al_exe(char* ruta, size_t tam)
{
    if (!ruta[0])
        return;
    FILE* f = fopen(ruta, "rb");
    if (f)
    {
        fclose(f);
        return;
    }
    if (ruta[0] == '/' || ruta[0] == '\\' || (ruta[0] && ruta[1] == ':'))
        return;
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    char* barra = strrchr(exe, '\\');
    if (!barra)
        return;
    barra[1] = 0;
    char candidata[MAX_PATH + 512];
    snprintf(candidata, sizeof(candidata), "%s%s", exe, ruta);
    f = fopen(candidata, "rb");
    if (f)
    {
        fclose(f);
        strncpy(ruta, candidata, tam - 1);
        ruta[tam - 1] = 0;
    }
#else
    (void)tam;
#endif
}

// Prepara el tablero para una nueva partida (limpia historial, etc.)
void engine_new_game()
{
    //printf("Info: Nueva partida preparada.\n");
    clear_tt();
    clear_search_heuristics(); // Limpiar continuation history y otras heurísticas
    game_history_count = 0;
    posicion_valida = parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// Helper para convertir string a Move (necesario para aplicar movimientos de la GUI)
Move parse_move_string(Position* pos, char* move_str)
{
    // Una jugada UCI tiene 4 o 5 caracteres y casillas validas (I26).
    size_t len = strlen(move_str);
    if (len < 4 || len > 5
        || move_str[0] < 'a' || move_str[0] > 'h' || move_str[2] < 'a' || move_str[2] > 'h'
        || move_str[1] < '1' || move_str[1] > '8' || move_str[3] < '1' || move_str[3] > '8')
        return MOVE_NONE;

    MoveList moves;
    generate_moves(pos, &moves, GEN_ALL);
    
    int source = (move_str[0] - 'a') + ((move_str[1] - '1') * 8);
    int target = (move_str[2] - 'a') + ((move_str[3] - '1') * 8);
    int promoted = 0;
    
    if (len > 4)
    {
        switch (tolower((unsigned char)move_str[4]))   // algun GUI la manda en mayuscula (B21)
        {
            case 'n': promoted = KNIGHT; break;
            case 'b': promoted = BISHOP; break;
            case 'r': promoted = ROOK; break;
            case 'q': promoted = QUEEN; break;
            default: return MOVE_NONE;                  // quinto caracter que no es pieza
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
        posicion_valida = parse_fen(&g_pos, fen);
    else
        posicion_valida = parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    if (!posicion_valida)
    {
        // parse_fen no ha tocado g_pos, pero no se busca: el GUI espera otra posicion
        printf("info string Error: posicion no valida; 'go' respondera bestmove 0000\n");
        fflush(stdout);
        g_pos.prev = NULL;
        return;
    }
    
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
            if (move == MOVE_NONE)
            {
                printf("info string Error: jugada %s no reconocida; se ignora el resto\n", token);
                fflush(stdout);
                break;
            }
            {
                Position antes;
                copy_position(&antes, &g_pos);
                // OJO: hay que mirar lo que devuelve make_move. Es pseudo-legal: si la
                // jugada deja al rey en jaque devuelve 0 y g_pos queda inconsistente.
                // Antes se ignoraba y el motor seguia jugando sobre un tablero corrupto.
                if (!make_move(&g_pos, move))
                {
                    copy_position(&g_pos, &antes);   // make_move pudo dejarla a medias (I23)
                    printf("info string Error: la jugada %s es ilegal; se ignora el resto\n", token);
                    fflush(stdout);
                    break;
                }
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
    // Recalcular el acumulador: tras cientos de actualizaciones incrementales en float
    // arrastra deriva, y asi la eval no depende de por donde se llego a la posicion
    nnue_refresh_accumulator(&g_pos);
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
    // Si ya hay una búsqueda activa (la GUI interrumpe sin un stop previo), pedimos parar.
    if (ATOMIC_LOAD_ACQ(is_searching))
    {
        ATOMIC_STORE(stop_gui, 1);
        ATOMIC_STORE(stop_search, 1);
    }

    // Esperar a que la búsqueda anterior termine completamente y de forma segura
    while (ATOMIC_LOAD_ACQ(is_searching))
    {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }

    if (!posicion_valida)
    {
        printf("info string Error: la posicion no es valida; no se busca\n");
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    // Crear una copia de los límites para pasar al hilo
    SearchLimits* limits_ptr = malloc(sizeof(SearchLimits));
    if (!limits_ptr)
    {
        printf("info string Error: sin memoria para lanzar la busqueda\n");
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }
    *limits_ptr = limits;

    // El estado de control se fija AQUI, en el hilo UCI, antes de crear el de busqueda:
    // asi un 'ponderhit' o un 'stop' que llegue enseguida siempre lo encuentra (B1)
    ATOMIC_STORE(stop_search, 0);
    ATOMIC_STORE(stop_gui, 0);
    preparar_busqueda(&g_pos, limits);
    ATOMIC_STORE(is_searching, 1);

#ifdef _WIN32
    // 8 MB de pila (ver 1.4): con la de por defecto, 1 MB, una búsqueda profunda
    // podía desbordarla.
    HANDLE hThread = CreateThread(NULL, 8 << 20, search_thread, limits_ptr, 0, NULL);
    if (hThread != NULL)
    {
        CloseHandle(hThread); // Cerrar handle inmediatamente para evitar leak
    }
    else
    {
        free(limits_ptr);
        ATOMIC_STORE(is_searching, 0);
        printf("info string Error: no se pudo crear el hilo de busqueda\n");
        printf("bestmove 0000\n");
        fflush(stdout);
    }
#else
    pthread_t thread_id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    if (pthread_create(&thread_id, &attr, search_thread, limits_ptr) == 0)
    {
        pthread_detach(thread_id); // Detach para liberar recursos automáticamente
    }
    else
    {
        free(limits_ptr);
        ATOMIC_STORE(is_searching, 0);
        printf("info string Error: no se pudo crear el hilo de busqueda\n");
        printf("bestmove 0000\n");
        fflush(stdout);
    }
#endif
}

// Detiene la búsqueda inmediatamente si está corriendo.
void engine_stop()
{
    printf("info string Deteniendo busqueda\n");
    fflush(stdout);
    ATOMIC_STORE(stop_gui, 1);
    ATOMIC_STORE(stop_search, 1);
}

// Imprime el tablero actual (útil para debug, no estrictamente UCI).
void engine_print()
{
    print_board(&g_pos);
}

long long nodes;
int salida_busqueda = 1;

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

// perft con divide: una linea 'e2e4: N' por jugada legal y 'Nodes searched: N'.
// Es el formato de Stockfish, el que entienden el Juez y la mayoria de herramientas.
void perft_test(int depth)
{
    esperar_fin_busqueda();
    if (!posicion_valida)
    {
        printf("info string Error: la posicion no es valida; no hay perft\n");
        fflush(stdout);
        return;
    }
    if (depth < 1)
        depth = 1;
    if (depth > 30)
    {
        // Rechazar, no recortar: 'perft 1000' desbordaba la pila, y un perft 30 no
        // terminaria nunca (B23)
        printf("info string perft: profundidad %d fuera de rango (1-30); se ignora\n", depth);
        fflush(stdout);
        return;
    }

    long long total = 0;
    long long t0 = get_time_ms();

    MoveList raiz;
    generate_moves(&g_pos, &raiz, GEN_ALL);
    Position backup;
    for (int i = 0; i < raiz.count; i++)
    {
        copy_position(&backup, &g_pos);
        if (!make_move(&g_pos, raiz.moves[i]))
        {
            copy_position(&g_pos, &backup);
            continue;
        }
        nodes = 0;
        perft_driver(depth - 1);
        copy_position(&g_pos, &backup);

        print_move(raiz.moves[i]);
        printf(": %lld\n", nodes);
        total += nodes;
    }

    long long ms = get_time_ms() - t0;
    printf("\nNodes searched: %lld\n", total);
    printf("info string perft %d: %lld nodos en %lld ms\n", depth, total, ms);
    fflush(stdout);
}

/*
 *
 * bench - banco de pruebas reproducible
 *
 * Recorre un conjunto fijo de posiciones a profundidad fija y suma los nodos.
 * El total es DETERMINISTA, y sirve para dos cosas:
 *   1) Verificar que un cambio de codigo es 'non-functional': si el recuento de
 *      nodos no varia, el refactor no ha alterado la busqueda.
 *   2) Medir nps de forma comparable entre versiones y entre maquinas.
 *
 * Para que el numero sea reproducible se neutraliza todo lo que introduce estado
 * o dependencia del entorno: hash fijo y limpia, heuristicas limpias, sin limite
 * de tiempo, y Syzygy desactivado durante el bench.
 *
 * OJO: compilado con -DUSE_SMP y Threads > 1 el recuento NO es reproducible
 * (Lazy SMP es no determinista por diseno).
 *
 */

static const char* bench_fens[] =
{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "2r1r1k1/pp1bppbp/3p1np1/q3P3/2P2P2/1P2B3/P1N1B1PP/2RQ1RK1 b - - 1 16",
    "r1bqk2r/pp2bppp/2p5/3pP3/P2Q1P2/2N1B3/1PP3PP/R4RK1 b kq - 0 14",
    "r2qnrnk/p2b2b1/1p1p2pp/2pPpp2/1PP1P3/PRNBB3/3QNPPP/5RK1 w - - 1 22",
    "3r1rk1/p3qp1p/2bb2p1/2p5/3P4/1P6/PBQN1PPP/2R2RK1 b - - 4 19",
    "2q1rr1k/3bbnnp/p2p1pp1/2pPp3/PpP1P1P1/1P2BNNP/2BQ1PRK/7R b - - 0 1",
    "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1",
    "3rr1k1/pp3pp1/1qn2np1/8/3p4/PP1R1P2/2P1NQPP/R1B3K1 b - - 0 1",
    "r1bq1rk1/pp2ppbp/2np2p1/2p5/4P3/2P2N1P/PPBP1PP1/RNBQR1K1 b - - 0 9",
    "r1bqk2r/pp3ppp/2p1pn2/8/1b1P4/2NB1N2/PP3PPP/R1BQK2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/k7/3p4/p2P1p2/P2P1P2/8/8/K7 w - - 0 1",
    "8/8/1p1r1k2/p1pPN1p1/P3KnP1/1P6/8/3R4 b - - 0 1",
    "6k1/6p1/6Pp/ppp5/3pn2P/1P3K2/1PP2P2/3N4 b - - 0 1",
    "3b4/5kp1/1p1p1p1p/pP1PpP1P/P1P1P3/3KN3/8/8 w - - 0 1",
    "2K5/p7/7P/5pR1/8/5k2/r7/8 w - - 0 1",
    "8/6pk/1p6/8/PP3p1p/5P2/4KP1q/3Q4 w - - 0 1",
    "7k/3p2pp/4q3/8/4Q3/5Kp1/P6b/8 w - - 0 1",
    "4b3/p3kp2/6p1/3pP2p/2pP1P2/4K1P1/P3N2P/8 w - - 0 32",
    "6k1/5pp1/7p/8/8/1P3P2/5PKP/8 w - - 0 1",
    "8/8/3P3k/8/1p6/8/1P6/1K3n2 b - - 0 1",
    "5k2/7R/4P2p/5K2/p1r2P1p/8/8/8 b - - 0 1",
};

// El bench busca en un hilo con 8 MB de pila, como 'go': el hilo principal solo tiene
// 2 MB y una busqueda honda puede necesitar mas (I28).
#ifdef _WIN32
static DWORD WINAPI hilo_bench(LPVOID p)
{
    search_position(&g_pos, *(SearchLimits*)p);
    return 0;
}
#else
static void* hilo_bench(void* p)
{
    search_position(&g_pos, *(SearchLimits*)p);
    return NULL;
}
#endif

static void buscar_y_esperar(SearchLimits limits)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 8 << 20, hilo_bench, &limits, 0, NULL);
    if (h != NULL)
    {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        return;
    }
#else
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    if (pthread_create(&t, &attr, hilo_bench, &limits) == 0)
    {
        pthread_join(t, NULL);
        return;
    }
#endif
    search_position(&g_pos, limits);   // sin hilo: en la pila del principal
}

void run_bench(int depth, int hash_mb)
{
    if (depth < 1)
        depth = 12;
    if (hash_mb < 1)
        hash_mb = 16;

    // 'go' lanza la busqueda en su propio hilo. Si sigue viva, bench no puede
    // arrancar: comparten g_pos, la tabla hash y las heuristicas, y el recuento
    // de nodos dejaria de ser reproducible.
    esperar_fin_busqueda();

    const int num_fens = (int)(sizeof(bench_fens) / sizeof(bench_fens[0]));

    // Syzygy fuera: el bench no debe depender de si hay tablas instaladas.
    const int syzygy_limit_backup = opt_syzygy_probe_limit;
    opt_syzygy_probe_limit = 0;

    resize_tt(hash_mb);

    printf("info string bench: %d posiciones, profundidad %d, hash %d MB\n", num_fens, depth, hash_mb);
    fflush(stdout);

    long long total_nodes = 0;
    long long t0 = get_time_ms();

    for (int i = 0; i < num_fens; i++)
    {
        // Estado limpio: ninguna posicion debe heredar informacion de la anterior.
        clear_tt();
        clear_search_heuristics();

        game_history_count = 0;
        if (!parse_fen(&g_pos, bench_fens[i]))
        {
            printf("info string ERROR: FEN %d del bench invalido: %s\n", i + 1, bench_fens[i]);
            fflush(stdout);
            continue;
        }
        g_pos.prev = NULL;
        game_history[game_history_count++] = g_pos.hash;

        SearchLimits limits;
        memset(&limits, 0, sizeof(SearchLimits));
        limits.depth = depth;

        ATOMIC_STORE(stop_search, 0);
        ATOMIC_STORE(stop_gui, 0);
        salida_busqueda = 0;               // ni info ni bestmove por posicion (I109)
        preparar_busqueda(&g_pos, limits);
        buscar_y_esperar(limits);          // sincrono, en un hilo con 8 MB de pila (I28)
        salida_busqueda = 1;

        total_nodes += last_search_nodes;
        printf("info string bench %2d/%d  nodos %10lld  acumulado %12lld\n",
               i + 1, num_fens, last_search_nodes, total_nodes);
        fflush(stdout);
    }

    long long elapsed = get_time_ms() - t0;
    if (elapsed <= 0)
        elapsed = 1;

    opt_syzygy_probe_limit = syzygy_limit_backup;

    // Dejar el tablero en un estado predecible, y la TT con el Hash configurado (B16)
    game_history_count = 0;
    posicion_valida = parse_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    g_pos.prev = NULL;
    resize_tt(opt_hash_mb);

    printf("\n===========================\n");
    printf("Total time (ms) : %lld\n", elapsed);
    printf("Nodes searched  : %lld\n", total_nodes);
    printf("Nodes/second    : %lld\n", total_nodes * 1000 / elapsed);
    fflush(stdout);
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

    char *token = strtok(params, " \t");

    // 'go perft N' (dialecto de Stockfish): perft sincrono, no es una busqueda.
    if (token && strcmp(token, "perft") == 0)
    {
        token = strtok(NULL, " \t");
        perft_test(token ? atoi(token) : 1);
        return;
    }

    while (token != NULL)
    {
        if (strcmp(token, "wtime") == 0)
        {
            token = strtok(NULL, " \t");
            if (token)
            {
                limits.wtime = atoi(token);
                limits.reloj = true;     // aunque valga 0 o menos: hay reloj (B12)
            }
        }
        else if (strcmp(token, "btime") == 0)
        {
            token = strtok(NULL, " \t");
            if (token)
            {
                limits.btime = atoi(token);
                limits.reloj = true;
            }
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
        else if (strcmp(token, "nodes") == 0)
        {
            token = strtok(NULL, " ");
            if (token)
                limits.nodes = atoll(token);
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
        // Saltar "fen" y los espacios. Sin FEN detras no se toca la posicion (I19).
        fen = strstr(params, "fen") + 3;
        while (*fen == ' ')
            fen++;
        if (*fen == '\0')
        {
            printf("info string Error: 'position fen' sin FEN; se ignora\n");
            fflush(stdout);
            return;
        }
    }
    else
    {
        printf("info string Error: 'position' sin startpos ni fen; se ignora\n");
        fflush(stdout);
        return;
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
    if (!params)                      // 'setoption' a secas (I06)
        return;
    char* name_ptr = strstr(params, "name");
    if (!name_ptr)
        return;
    name_ptr += 4; // Skip "name" (los espacios se saltan abajo; +5 leia tras el '\0')
    while (*name_ptr == ' ')
        name_ptr++;

    // 'value' se busca DESPUES del nombre: con 'setoption value 1 name Hash' quedaba
    // antes y la resta de punteros daba la vuelta a un size_t (B22)
    char* value_ptr = strstr(name_ptr, " value");
    if (value_ptr)
        value_ptr++;
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
        
        value_ptr += 5; // Skip "value" (los espacios se saltan abajo)
        while (*value_ptr == ' ')
            value_ptr++;
        strncpy(value, value_ptr, sizeof(value) - 1);
        value[strcspn(value, "\n")] = 0;
        value[strcspn(value, "\r")] = 0;
        size_t vlen = strlen(value);
        while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t'))
            value[--vlen] = 0;
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
    // Todas las opciones llevan valor salvo SyzygyPath, que vacio significa 'sin tablas'.
    // Antes, 'setoption name Hash' dejaba la TT en 1 MB y 'setoption name EvalFile'
    // tiraba la red (B22).
    if (!value[0] && STRCASECMP(name, "SyzygyPath") != 0)
    {
        printf("info string setoption %s sin value: se ignora\n", name);
        return;
    }

    if (STRCASECMP(name, "Hash") == 0)
    {
        opt_hash_mb = clamp_int(atoi(value), 1, 16384);
        resize_tt(opt_hash_mb);
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
        char anterior[sizeof(opt_nnue_path)];
        memcpy(anterior, opt_nnue_path, sizeof(anterior));
        strncpy(opt_nnue_path, value, sizeof(opt_nnue_path) - 1);
        opt_nnue_path[sizeof(opt_nnue_path) - 1] = 0;
        // El nombre por defecto (o vacio) es la red interna: muchos GUI reenvian el
        // valor por defecto de todas las opciones y no debe buscarse en disco.
        int ok;
        if (opt_nnue_path[0] == 0 || STRCASECMP(opt_nnue_path, "<empty>") == 0
            || STRCASECMP(opt_nnue_path, RED_POR_DEFECTO) == 0)
        {
            strcpy(opt_nnue_path, RED_POR_DEFECTO);
            ok = nnue_init_interna();
        }
        else
        {
            resolver_junto_al_exe(opt_nnue_path, sizeof(opt_nnue_path));
            ok = nnue_init(opt_nnue_path);
        }
        if (ok)
        {
            nnue_refresh_accumulator(&g_pos);   // la posicion actual, con la red nueva (I13)
            clear_tt();                         // las evals guardadas eran de la red vieja
        }
        else
        {
            memcpy(opt_nnue_path, anterior, sizeof(anterior));   // se queda la red que habia (B15)
        }
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
#if (OPTIMIZAR_CORR)
    else if (STRCASECMP(name, "CorrGrain") == 0)
    {
        opt_corr_grain = clamp_int(atoi(value), 16, 4096);   // divisor: nunca 0 (B32)
    }
    else if (STRCASECMP(name, "CorrLimit") == 0)
    {
        opt_corr_limit = clamp_int(atoi(value), 256, 32767); // la tabla es int16_t
    }
    else if (STRCASECMP(name, "CorrPeso") == 0)
    {
        opt_corr_peso = clamp_int(atoi(value), 1, 128);
    }
#endif
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
    else if (STRCASECMP(name, "QSImprove") == 0)
    {
        opt_qs_improve = atoi(value);
    }
    else if (STRCASECMP(name, "QSStable") == 0)
    {
        opt_qs_stable = atoi(value);
    }
    else if (STRCASECMP(name, "QSWorsen") == 0)
    {
        opt_qs_worsen = atoi(value);
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
    else if (STRCASECMP(name, "BusNMIfWorsen") == 0)
    {
        opt_bus_nm_ifworsen = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMWillImprove") == 0)
    {
        opt_bus_nm_willimprove = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMIsStable") == 0)
    {
        opt_bus_nm_isstable = atoi(value);
    }
    else if (STRCASECMP(name, "BusNMWillWorsen") == 0)
    {
        opt_bus_nm_willworsen = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIDDepthMin") == 0)
    {
        opt_bus_iid_depthmin = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIDImprove") == 0)
    {
        opt_bus_iid_improve = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIDWorsen") == 0)
    {
        opt_bus_iid_worsen = atoi(value);
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
    else if (STRCASECMP(name, "BusPodaJCIfWorsen") == 0)
    {
        opt_bus_podajc_ifworsen = atoi(value);
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
    else if (STRCASECMP(name, "BusIIRPVStable") == 0)
    {
        opt_bus_iir_pv_stable = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRCutImprove") == 0)
    {
        opt_bus_iir_cut_improve = atoi(value);
    }
    else if (STRCASECMP(name, "BusIIRAllWorsen") == 0)
    {
        opt_bus_iir_all_worsen = atoi(value);
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
    else if (STRCASECMP(name, "BusFullrazImprove") == 0)
    {
        opt_bus_fullraz_improve = atoi(value);
    }
    else if (STRCASECMP(name, "BusFullrazWorsen") == 0)
    {
        opt_bus_fullraz_worsen = atoi(value);
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
    else if (STRCASECMP(name, "ProbcutWillImprove") == 0)
    {
        opt_probcut_willimprove = atoi(value);
    }
    else if (STRCASECMP(name, "ProbcutIsStable") == 0)
    {
        opt_probcut_isstable = atoi(value);
    }
    else if (STRCASECMP(name, "ProbcutWillWorsen") == 0)
    {
        opt_probcut_willworsen = atoi(value);
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
    else if (STRCASECMP(name, "LMRTCWillImprove") == 0)
    {
        opt_lmr_tc_willimprove = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTCWillWorsen") == 0)
    {
        opt_lmr_tc_willworsen = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTCIsStable") == 0)
    {
        opt_lmr_tc_isstable = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTCWllImprBeta") == 0)
    {
        opt_lmr_tc_wllimpbeta = atoi(value);
    }
    else if (STRCASECMP(name, "LMRTCWilWorsenAlpha") == 0)
    {
        opt_lmr_tc_wilworalpha = atoi(value);
    }
    else if (STRCASECMP(name, "TCThreshold") == 0)
    {
        opt_tc_threshold = atoi(value);
    }
    else if (STRCASECMP(name, "TCDirMinDepth") == 0)
    {
        opt_tc_dir_min_depth = atoi(value);
    }
    else if (STRCASECMP(name, "TCFile") == 0)
    {
        char anterior[sizeof(opt_tc_path)];
        memcpy(anterior, opt_tc_path, sizeof(anterior));
        strncpy(opt_tc_path, value, sizeof(opt_tc_path) - 1);
        opt_tc_path[sizeof(opt_tc_path) - 1] = 0;
        const int interno = opt_tc_path[0] == 0 || STRCASECMP(opt_tc_path, "<empty>") == 0
                         || STRCASECMP(opt_tc_path, TC_POR_DEFECTO) == 0;
        if (interno)
            strcpy(opt_tc_path, TC_POR_DEFECTO);
        else
            resolver_junto_al_exe(opt_tc_path, sizeof(opt_tc_path));
        if (!USA_TC)
            printf("info string TCFile ignorado: compilado con USA_TC=0\n");
        else if (interno ? nnue_tc_init_interna() : nnue_tc_init(opt_tc_path))
            clear_tt();
        else
            memcpy(opt_tc_path, anterior, sizeof(anterior));
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

int main(int argc, char* argv[])
{
    // 64 KB: un 'position startpos moves ...' de una partida larga no cabia en 4 KB
    // y se cortaba sin avisar (I33).
    static char line[65536];
    
    // Inicialización básica
    engine_init();

    // 'Gilipol bench [prof] [hash]' por linea de ordenes: lo que usan OpenBench y
    // cualquier flujo automatico para leer la firma de nodos (I108).
    if (argc > 1 && strcmp(argv[1], "bench") == 0)
    {
        int d = (argc > 2) ? atoi(argv[2]) : 12;
        int h = (argc > 3) ? atoi(argv[3]) : 16;
        run_bench(d, h);
        return 0;
    }

    // Bucle principal: leer stdin línea por línea
    while (fgets(line, sizeof(line), stdin))
    {
        // Eliminar el salto de línea al final
        line[strcspn(line, "\r\n")] = 0;   // tambien el CR de un fin de linea Windows
        
        // Saltar líneas vacías
        if (strlen(line) == 0)
            continue;

        // Extraer el comando principal (primer token)
        char* command = strtok(line, " \t");
        if (command == NULL)          // linea de solo espacios: antes, strcmp(NULL) (I05)
            continue;
        
        // Puntero al resto de los parámetros (si existen)
        char* params = strtok(NULL, ""); 

        if (strcmp(command, "uci") == 0)
        {
            printf("id name Gilipol 3.20\n");
            printf("id author Jose Carlos Martinez Galan\n");
            printf("option name Hash type spin default 32 min 1 max 16384\n");
            #ifdef USE_SMP
                printf("option name Threads type spin default 1 min 1 max 128\n");
            #endif
            printf("option name Ponder type check default true\n");
            printf("option name MultiPV type spin default 1 min 1 max 1\n");
            printf("option name SyzygyPath type string default <empty>\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
            // El nombre relativo, no la ruta resuelta: un GUI que guarde la ruta absoluta
            // la reenviaria aunque luego se mueva la carpeta del motor (B15)
            printf("option name EvalFile type string default %s\n", RED_POR_DEFECTO);
            #if (OPTIMIZAR_CORR)
                printf("option name CorrGrain type spin default %d min 16 max 4096\n", opt_corr_grain);
                printf("option name CorrLimit type spin default %d min 256 max 32767\n", opt_corr_limit);
                printf("option name CorrPeso type spin default %d min 1 max 128\n", opt_corr_peso);
            #endif

            #if (OPTIMIZAR_QS)
                printf("option name QSPodaDelta type spin default %d min 0 max 150\n", opt_qs_poda_delta);
                printf("option name QSFutMargin type spin default %d min 0 max 200\n", opt_qs_fut_margin);
                printf("option name QSSEE type spin default %d min -100 max 10\n", opt_qs_see);
                printf("option name QSImprove type spin default %d min -100 max 100\n", opt_qs_improve);
                printf("option name QSStable type spin default %d min -100 max 100\n", opt_qs_stable);
                printf("option name QSWorsen type spin default %d min -100 max 100\n", opt_qs_worsen);
                //printf("option name QSLMP type spin default %d min 2 max 10\n", opt_qs_lmp);
            #endif

            #if (OPTIMIZAR_RAZOR)
                printf("option name BusRazorDepth type spin default %d min 1 max 15\n", opt_bus_razor_depth);
                printf("option name BusRazorMargin1 type spin default %d min 0 max 250\n", opt_bus_razor_margin1);
                printf("option name BusRazorMargin2 type spin default %d min 200 max 600\n", opt_bus_razor_margin2);
                printf("option name BusRazorMargin3 type spin default %d min 300 max 800\n", opt_bus_razor_margin3);
                printf("option name BusRazorMargin4 type spin default %d min 600 max 1200\n", opt_bus_razor_margin4);
                printf("option name BusRazorMargin5 type spin default %d min 800 max 2000\n", opt_bus_razor_margin5);
                printf("option name BusFullrazBase type spin default %d min 0 max 2000\n", opt_bus_fullraz_base);
                printf("option name BusFullrazMult type spin default %d min 0 max 1000\n", opt_bus_fullraz_mult);
                printf("option name BusFullrazImprove type spin default %d min -200 max 200\n", opt_bus_fullraz_improve);
                printf("option name BusFullrazWorsen type spin default %d min -200 max 200\n", opt_bus_fullraz_worsen);
            #endif

            #if (OPTIMIZAR_PODAJC)
                printf("option name BusPodaJCDepth type spin default %d min 3 max 15\n", opt_bus_podajc_depth);
                printf("option name BusPodaJCMargin type spin default %d min 0 max 200\n", opt_bus_podajc_margin);
                printf("option name BusPodaJCCero type spin default %d min -100 max 50\n", opt_bus_podajc_cero);
                printf("option name BusPodaJCIfWorsen type spin default %d min 0 max 1\n", opt_bus_podajc_ifworsen);
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
                printf("option name BusHPMargin type spin default %d min 0 max 16000\n", opt_bus_hp_margin);
            #endif

            #if (OPTIMIZAR_CHP)
                printf("option name BusCHPDepth type spin default %d min 1 max 15\n", opt_bus_chp_depth);
                printf("option name BusCHPMargin type spin default %d min 0 max 5000\n", opt_bus_chp_margin);
            #endif

            #if (OPTIMIZAR_FP)
                printf("option name BusFutDepth type spin default %d min 1 max 10\n", opt_bus_fut_depth);
                printf("option name BusFutBase type spin default %d min 0 max 500\n", opt_bus_fut_base);
                printf("option name BusFutMargin type spin default %d min 0 max 200\n", opt_bus_fut_margin);
            #endif
            
            #if (OPTIMIZAR_IIDR)
                printf("option name BusIIDDepthMin type spin default %d min 6 max 20\n", opt_bus_iid_depthmin);
                printf("option name BusIIDImprove type spin default %d min -100 max 100\n", opt_bus_iid_improve);
                printf("option name BusIIDWorsen type spin default %d min -100 max 100\n", opt_bus_iid_worsen);
                printf("option name BusIIRDepth type spin default %d min 2 max 6\n", opt_bus_iir_depth);
                printf("option name BusIIRPVRed type spin default %d min 0 max 2\n", opt_bus_iir_pvred);
                printf("option name BusIIRCutRed type spin default %d min 0 max 2\n", opt_bus_iir_cutred);
                printf("option name BusIIRAllRed type spin default %d min 0 max 1\n", opt_bus_iir_allred);
                printf("option name BusIIRPVStable type spin default %d min -1 max 1\n", opt_bus_iir_pv_stable);
                printf("option name BusIIRCutImprove type spin default %d min -1 max 1\n", opt_bus_iir_cut_improve);
                printf("option name BusIIRAllWorsen type spin default %d min -1 max 1\n", opt_bus_iir_all_worsen);
            #endif

            #if (OPTIMIZAR_LMP)
                printf("option name LMPBase type spin default %d min 1 max 10\n", opt_lmp_base);
                printf("option name LMPDepth type spin default %d min 1 max 10\n", opt_lmp_depth);
            #endif

            #if (OPTIMIZAR_NM)
                printf("option name BusNMCutYAll type spin default %d min 0 max 1\n", opt_bus_nm_cutyall);
                printf("option name BusNMR type spin default %d min 3 max 5\n", opt_bus_nm_r);
                printf("option name BusNMImp type spin default %d min 0 max 1\n", opt_bus_nm_imp);
                printf("option name BusNMDiv type spin default %d min 180 max 280\n", opt_bus_nm_div);
                printf("option name BusNMVerif type spin default %d min 8 max 20\n", opt_bus_nm_verif);
                //printf("option name BusNMIfWorsen type spin default %d min 0 max 1\n", opt_bus_nm_ifworsen);
                printf("option name BusNMWillImprove type spin default %d min 0 max 100\n", opt_bus_nm_willimprove);
                printf("option name BusNMIsStable type spin default %d min 0 max 100\n", opt_bus_nm_isstable);
                printf("option name BusNMWillWorsen type spin default %d min 0 max 100\n", opt_bus_nm_willworsen);
            #endif

            #if (OPTIMIZAR_TC)
                printf("option name TCThreshold type spin default %d min 60 max 95\n", opt_tc_threshold);
                printf("option name TCDirMinDepth type spin default %d min 0 max 14\n", opt_tc_dir_min_depth);
            #endif

            #if (OPTIMIZAR_LMR)
                printf("option name LMRBaseDepth type spin default %d min 1 max 4\n", opt_lmr_base_depth);
                printf("option name LMRBaseMove type spin default %d min 2 max 4\n", opt_lmr_base_move);
                printf("option name LMRCheckDepth type spin default %d min -6 max 6\n", opt_lmr_check_depth);
                printf("option name LMRCutDepth type spin default %d min -6 max 6\n", opt_lmr_cut_depth);
                printf("option name LMRMejoDepth type spin default %d min -6 max 6\n", opt_lmr_mejo_depth);
                printf("option name LMRGoodMoveMargin type spin default %d min 100 max 300\n", opt_lmr_goodmove_margin);
                printf("option name LMREvalDepth type spin default %d min -6 max 6\n", opt_lmr_eval_depth);
                printf("option name LMRTTDDepth type spin default %d min -6 max 6\n", opt_lmr_ttd_depth);
                printf("option name LMRTTCaptDepth type spin default %d min -6 max 6\n", opt_lmr_ttcapt_depth);
                printf("option name LMRTCWillImprove type spin default %d min -6 max 6\n", opt_lmr_tc_willimprove);
                printf("option name LMRTCWillWorsen type spin default %d min -6 max 6\n", opt_lmr_tc_willworsen);
                printf("option name LMRTCIsStable type spin default %d min -6 max 6\n", opt_lmr_tc_isstable);
                printf("option name LMRTCWllImprBeta type spin default %d min -6 max 6\n", opt_lmr_tc_wllimpbeta);
                printf("option name LMRTCWilWorsenAlpha type spin default %d min -6 max 6\n", opt_lmr_tc_wilworalpha);
            #endif

            #if (OPTIMIZAR_LMR_HIST)
                printf("option name LMRHistMenos2 type spin default %d min 0 max 32000\n", opt_lmr_hist_menos2);
                printf("option name LMRHistMenos1 type spin default %d min -16000 max 32000\n", opt_lmr_hist_menos1);
                printf("option name LMRHistMas1 type spin default %d min -32000 max 16000\n", opt_lmr_hist_mas1);
                printf("option name LMRCHistMas1 type spin default %d min -16000 max 8000\n", opt_lmr_chist_mas1);
                printf("option name LMRCHistMenos1 type spin default %d min -8000 max 16000\n", opt_lmr_chist_menos1);
            #endif

            #if (OPTIMIZAR_PBCT)
                printf("option name ProbcutDepth type spin default %d min 3 max 9\n", opt_probcut_depth);
                printf("option name ProbcutMargin type spin default %d min 50 max 250\n", opt_probcut_margin);
                printf("option name ProbcutReduction type spin default %d min 3 max 6\n", opt_probcut_reduction);
                printf("option name ProbcutWillImprove type spin default %d min 0 max 100\n", opt_probcut_willimprove);
                printf("option name ProbcutIsStable type spin default %d min 0 max 100\n", opt_probcut_isstable);
                printf("option name ProbcutWillWorsen type spin default %d min 0 max 100\n", opt_probcut_willworsen);
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
            esperar_fin_busqueda();
            engine_new_game();        
        }
        else if (strcmp(command, "position") == 0)
        {
            esperar_fin_busqueda();
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
            esperar_fin_busqueda();
            parse_option(params);
        }
        else if (strcmp(command, "stop") == 0)
        {
            engine_stop();        
        }
        else if (strcmp(command, "quit") == 0)
        {
            esperar_fin_busqueda();
            break; // Salir del bucle y terminar el programa        
        }
        else if (strcmp(command, "d") == 0)
        {
            // Comando no estándar pero útil para debug
            engine_print();
        }
        else if (strcmp(command, "bench") == 0)
        {
            // bench [profundidad] [hash_mb]   (por defecto: 12 y 16)
            int bench_depth = 12;
            int bench_hash = 16;
            if (params)
            {
                char* tok = strtok(params, " ");
                if (tok)
                {
                    bench_depth = atoi(tok);
                    tok = strtok(NULL, " ");
                    if (tok)
                        bench_hash = atoi(tok);
                }
            }
            run_bench(bench_depth, bench_hash);
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

    // EOF sin 'quit': una busqueda con limite se deja terminar (un 'go depth N' por
    // tuberia debe dar su bestmove); una infinite/ponder se para. Antes el proceso
    // salia y mataba la busqueda a medias (B18).
    while (ATOMIC_LOAD_ACQ(is_searching) && !busqueda_espera_stop())
    {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    esperar_fin_busqueda();
    return 0;
}
