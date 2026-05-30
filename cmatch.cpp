#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <pthread.h>
#include <cstdint> 
#include <unistd.h> // Para sleep() de prueba
#include <cmath> // Para abs() y calculos de ELO
#include <ctime> // Para el tiempo de espera
#include <csignal> // Para manejar SIGINT
#include <cstdlib>
#include <limits>
using namespace std;

//  1. Configuracion
// Estructura para almacenar los parametros dinamicos del torneo
struct Config {
    int n_players = 0;
    int k_boards = 0;
    int k_elo = 32;
    int max_elo_diff = 0;
    int turn_delay_ms = 0;
    float reenter_probability = 0.0;
    string snapshot_path = "";
};

Config app_config; 

// Lee y parsea las variables de entorno desde un archivo fisico
bool cargarConfiguracion(const string& nombre_archivo) {
    ifstream archivo(nombre_archivo);
    if (!archivo.is_open()) return false;

    string linea;
    while (getline(archivo, linea)) {
        if (linea.empty()) continue;
        istringstream iss(linea);
        string clave, valor;

        if (getline(iss, clave, '=') && getline(iss, valor)) {
            if (clave == "N_PLAYERS") app_config.n_players = stoi(valor);
            else if (clave == "K_BOARDS") app_config.k_boards = stoi(valor);
            else if (clave == "K_ELO") app_config.k_elo = stoi(valor);
            else if (clave == "MAX_ELO_DIFF") app_config.max_elo_diff = stoi(valor);
            else if (clave == "TURN_DELAY_MS") app_config.turn_delay_ms = stoi(valor);
            else if (clave == "REENTER_PROBABILITY") app_config.reenter_probability = stof(valor);
            else if (clave == "SNAPSHOT_PATH") app_config.snapshot_path = valor;
        }
    }
    archivo.close();
    return true;
}

//  2. Estructuras compartidas (Estado del Torneo) 

enum PlayerState { WAITING, MATCHED, PLAYING, FINISHED };

// Representa a un hilo jugador y sus estadisticas persistentes
struct Player {
    int id;
    double elo = 1000.0; // Todos empiezan con 1000 de ELO
    int wins = 0;
    int losses = 0;
    int draws = 0;
    time_t wait_start_time;
    PlayerState state = WAITING;
    int opponent_id = -1;
    int assigned_board = -1;
};

// Representa a un hilo tablero y al estado visual de la partida
struct Board {
    int id;
    bool is_busy = false;
    int player1_id = -1;
    int player2_id = -1;
    
    // Variables para coordinar el juego
    bool game_ready = false; 
    bool game_finished = false;
    pthread_cond_t board_sync; // Despierta al tablero o a los jugadores

    vector<string> move_history;
    char grid[9] = {'1','2','3','4','5','6','7','8','9'};
};

// Contenedor global de la memoria compartida y mecanismos de sincronizacion
struct TournamentManager {
    vector<Player> players;
    vector<Board> boards;
    bool is_running = true; // Para el apagado elegante (SIGINT)

    // Mecanismos de sincronizacion POSIX
    pthread_mutex_t sync_mutex; // Protege las secciones criticas (Elos, estados)
    pthread_cond_t match_cond; // Alarma para buscar partidas
    pthread_cond_t board_cond; // Alarma para avisar que un tablero se desocupo
};

TournamentManager tournament;
pthread_t console_thread;

// 3. Control de ciclo de vida y presistencia (SNAPSHOT)

// Funcion que se dispara cuando presionas (Ctrl+C)
void handle_sigint(int signum) {
    (void)signum; // Evita warning de variable sin uso
    printf("\n\n[SIGINT] Apagando el torneo. Esperando a que terminen las partidas en curso...\n");
    
    // Cambiar la bandera global detendrá los bucles while() de todos los hilos
    tournament.is_running = false;
    
    // Despertar a todos los hilos que esten durmiendo para que vean que is_running es false y mueran
    pthread_cond_broadcast(&tournament.match_cond);
    pthread_cond_broadcast(&tournament.board_cond);
    for(Board& b : tournament.boards) {
        pthread_cond_broadcast(&b.board_sync);
    }

    // Aborta forzosamente la espera bloqueante de lectura de la consola
    pthread_cancel(console_thread);
}

// Guarda un volcado de memoria (dump) con las estadisticas actuales
void save_snapshot() {
    // Abrimos el archivo en modo binario
    ofstream out(app_config.snapshot_path, ios::binary);
    if (!out.is_open()) {
        cerr << "Error: No se pudo crear el archivo de snapshot " << app_config.snapshot_path << endl;
        return;
    }
    
    // Guardamos la cantidad de jugadores primero
    out.write(reinterpret_cast<const char*>(&app_config.n_players), sizeof(int));
    // Guardamos el arreglo completo de jugadores directamente de la memoria al disco
    out.write(reinterpret_cast<const char*>(tournament.players.data()), app_config.n_players * sizeof(Player));
    
    out.close();
    cout << "[SNAPSHOT] Estado del torneo guardado exitosamente en '" << app_config.snapshot_path << "'\n";
}

// Restaura los datos del torneo desde un volcado previo si existe
void load_snapshot() {
    ifstream in(app_config.snapshot_path, ios::binary);
    
    // Si el archivo no existe (primera vez que corremos el programa) no hacemos nada
    if (!in.is_open()) {
        cout << "[SNAPSHOT] No se encontro un archivo previo. Iniciando torneo con ELOs base (1000.0).\n";
        return;
    }

    int saved_n_players;
    in.read(reinterpret_cast<char*>(&saved_n_players), sizeof(int));

    // Validamos la integridad estructural contra la configuracion actual
    if (saved_n_players != app_config.n_players) {
        cout << "[SNAPSHOT] Advertencia: N_PLAYERS (" << app_config.n_players 
             << ") no coincide con el archivo (" << saved_n_players 
             << "). Iniciando desde cero.\n";
        in.close();
        return;
    }

    // Cargar toda la data de los jugadores desde el disco a la memoria RAM
    in.read(reinterpret_cast<char*>(tournament.players.data()), app_config.n_players * sizeof(Player));
    in.close();

    // Nos aseguramos de que todos entren limpios al nuevo torneo limpiando los estados
    for (int i = 0; i < app_config.n_players; ++i) {
        tournament.players[i].state = WAITING;
        tournament.players[i].opponent_id = -1;
        tournament.players[i].assigned_board = -1;
        // El Elo, wins, losses y draws se mantienen intactos
    }

    cout << "[SNAPSHOT] Estado restaurado exitosamente desde '" << app_config.snapshot_path << "'.\n";
}

// 4. Logica del juego y matematicas

// Verifica toda las combinaciones posibles ganadoras
bool check_win(const char* g, char m) {
    return ((g[0]==m && g[1]==m && g[2]==m) || (g[3]==m && g[4]==m && g[5]==m) || (g[6]==m && g[7]==m && g[8]==m) || // Filas
            (g[0]==m && g[3]==m && g[6]==m) || (g[1]==m && g[4]==m && g[7]==m) || (g[2]==m && g[5]==m && g[8]==m) || // Columnas
            (g[0]==m && g[4]==m && g[8]==m) || (g[2]==m && g[4]==m && g[6]==m));                                     // Diagonales
}

// Se simula la partida intectiva y se registra el historial
int play_tic_tac_toe(Board& b, int turn_delay_ms) {
    b.move_history.clear();
    // Limpiar el tablero visualmente antes de empezar
    for(int i=0; i<9; i++) b.grid[i] = '1' + i; 
    
    int empty_spots = 9;
    int current_player = 1; // 1 = P1 (X), 2 = P2 (O)
    char marker;

    while (empty_spots > 0 && tournament.is_running) {
        usleep(turn_delay_ms * 1000); 
        marker = (current_player == 1) ? 'X' : 'O';

        // Buscar una casilla que no este ocupada por X ni O
        int casilla;
        do {
            casilla = rand() % 9;
        } while (b.grid[casilla] == 'X' || b.grid[casilla] == 'O');

        // Marcar la jugada
        b.grid[casilla] = marker;
        string jugada = "P" + to_string(current_player) + " puso " + marker + " en " + to_string(casilla+1);
        b.move_history.push_back(jugada);

        // Verificar victoria
        if (check_win(b.grid, marker)) {
            return current_player; 
        }

        current_player = (current_player == 1) ? 2 : 1;
        empty_spots--;
    }
    return 0; // Empate
}

// Recalcula y aplica la formula del elo
void update_elo(Player& p1, Player& p2, int result, int k_elo) {
    double s1 = (result == 1) ? 1.0 : (result == 2) ? 0.0 : 0.5;
    double s2 = (result == 2) ? 1.0 : (result == 1) ? 0.0 : 0.5;

    double e1 = 1.0 / (1.0 + pow(10.0, (p2.elo - p1.elo) / 400.0));
    double e2 = 1.0 / (1.0 + pow(10.0, (p1.elo - p2.elo) / 400.0));

    p1.elo += k_elo * (s1 - e1);
    p2.elo += k_elo * (s2 - e2);
    
    if (result == 1) { p1.wins++; p2.losses++; }
    else if (result == 2) { p2.wins++; p1.losses++; }
    else { p1.draws++; p2.draws++; }
}

// 5. Funciones de los hilos (Jugadores y Tableros)

// Rutina que controla el ciclo de vida de cada jugador
void* player_routine(void* arg) {
    int my_id = (int)(intptr_t)arg;
    Player& me = tournament.players[my_id];
    
    while (tournament.is_running) {
        
        pthread_mutex_lock(&tournament.sync_mutex);
        
        // Solo resetear el tiempo de espera si recien empieza a buscar
        if (me.state != WAITING) {
            me.state = WAITING;
            me.wait_start_time = time(nullptr);
        }
        
        bool match_found = false;
        
        while (!match_found && tournament.is_running) {
            int best_opponent_id = -1;
            time_t longest_wait = -1; // Cambiado a -1 para aceptar esperas de 0 segs
            
            // Fase de emparejamiento por diferencia de elo y tiempo de espera
            for (Player& other : tournament.players) {
                if (other.id != my_id && other.state == WAITING && other.opponent_id == -1) {

                    // Condicion 1: Rango de ELO permitido
                    if (abs(me.elo - other.elo) <= app_config.max_elo_diff) {

                        // Condicion 2: Prioridad por tiempo de espera
                        time_t current_wait = time(nullptr) - other.wait_start_time;
                        if (current_wait > longest_wait) {
                            longest_wait = current_wait;
                            best_opponent_id = other.id;
                        }
                    }
                }
            }
            
            if (best_opponent_id != -1) {
                // Fase de asignacion de recursos (Busqueda de un tablero libre)
                int available_board_id = -1;
                for (Board& board : tournament.boards) {
                    if (!board.is_busy) {
                        available_board_id = board.id;
                        break;
                    }
                }
                
                if (available_board_id != -1) {
                    // Match y recursos exitosos. Se bloquean los estados concurrentes
                    Player& opponent = tournament.players[best_opponent_id];
                    Board& assigned_board = tournament.boards[available_board_id];
                    
                    // Bloquear a los jugadores
                    me.state = MATCHED;
                    opponent.state = MATCHED;
                    me.opponent_id = opponent.id;
                    opponent.opponent_id = me.id;
                    me.assigned_board = assigned_board.id;
                    opponent.assigned_board = assigned_board.id;
                    
                    // Bloquear el tablero
                    assigned_board.is_busy = true;
                    assigned_board.player1_id = me.id;
                    assigned_board.player2_id = opponent.id;
                    assigned_board.game_finished = false;
                    match_found = true;
                    
                    printf("Match: Jugador %d vs Jugador %d en Tablero %d\n", me.id, opponent.id, assigned_board.id);
                    
                    // Despertar al tablero para que juegue
                    assigned_board.game_ready = true;
                    pthread_cond_signal(&assigned_board.board_sync);
                    
                    // El jugador que armo el match duerme hasta que el tablero termine
                    while (!assigned_board.game_finished && tournament.is_running) {
                        pthread_cond_wait(&assigned_board.board_sync, &tournament.sync_mutex);
                    }
                    
                    // Decide si reingresa al torneo
                    float rand_prob = (float)rand() / RAND_MAX;
                    if (rand_prob <= app_config.reenter_probability) {
                        me.state = WAITING;
                        me.opponent_id = -1;
                        me.assigned_board = -1;
                        me.wait_start_time = time(nullptr);
                    } else {
                        me.state = FINISHED;
                        break; 
                    }
                    
                } else {
                    // Hay rival pero no hay tableros. Dormir y esperar tablero.
                    pthread_cond_wait(&tournament.board_cond, &tournament.sync_mutex);
                }
                
            } else {
                // No hay rivales compatibles. Dormir y esperar nuevo jugador.
                pthread_cond_wait(&tournament.match_cond, &tournament.sync_mutex);
            }
        }
        
        pthread_mutex_unlock(&tournament.sync_mutex);
        
        if (me.state == FINISHED || !tournament.is_running) {
            break; // Sale del hilo completamente
        }
    }
    
    return nullptr;
}

// Rutina de procesamiento concurrente de partidas para cada tablero
void* board_routine(void* arg) {
    int my_id = (int)(intptr_t)arg;
    Board& me = tournament.boards[my_id];
    
    while (tournament.is_running) {
        pthread_mutex_lock(&tournament.sync_mutex);
        
        // El tablero duerme hasta recibir una pareja de jugadores
        while (!me.game_ready && tournament.is_running) {
            pthread_cond_wait(&me.board_sync, &tournament.sync_mutex);
        }
        
        if (!tournament.is_running) {
            pthread_mutex_unlock(&tournament.sync_mutex);
            break;
        }

        Player& p1 = tournament.players[me.player1_id];
        Player& p2 = tournament.players[me.player2_id];
        
        // Se libera el candado global durante la ejecucion (Para no bloquear a otros mientras jugamos)
        pthread_mutex_unlock(&tournament.sync_mutex);
        
        int result = play_tic_tac_toe(me, app_config.turn_delay_ms);   
        
        // Se retoma el candado exclusivamente para actualizar vairables criticas
        pthread_mutex_lock(&tournament.sync_mutex);
        
        // Se actualiza los ELOs de forma segura
        update_elo(p1, p2, result, app_config.k_elo);
        
        // Registro (log) del final de la partida
        ofstream log_file("game_logs.txt", ios::app); 
        if (log_file.is_open()) {
            log_file << "Partida Finalizada: Tablero " << me.id << " | P1(" << p1.id << ") vs P2(" << p2.id << ")\n";
            if (result == 1) {
                log_file << "Resultado: Gano Jugador " << p1.id << " (X)\n";
            } else if (result == 2) {
                log_file << "Resultado: Gano Jugador " << p2.id << " (O)\n";
            } else {
                log_file << "Resultado: Empate\n";
            }
            log_file << "  " << me.grid[0] << " | " << me.grid[1] << " | " << me.grid[2] << "\n";
            log_file << " ---+---+---\n";
            log_file << "  " << me.grid[3] << " | " << me.grid[4] << " | " << me.grid[5] << "\n";
            log_file << " ---+---+---\n";
            log_file << "  " << me.grid[6] << " | " << me.grid[7] << " | " << me.grid[8] << "\n";
            log_file << "----------------------------------------\n";
            log_file.close();
        }
        
         printf("Tablero %d termino. P1(%d) ELO: %.1f | P2(%d) ELO: %.1f\n", 
               me.id, p1.id, p1.elo, p2.id, p2.elo);
        
        // Limpiar el tablero para la siguiente ronda 
        me.is_busy = false;
        me.game_ready = false;
        me.game_finished = true;
        me.player1_id = -1;
        me.player2_id = -1;
        
        // Despertar a los jugadores que estaban esperando el resultado
        pthread_cond_broadcast(&me.board_sync);
        // Avisar a la cola global que hay un tablero libre
        pthread_cond_broadcast(&tournament.board_cond);
        
        pthread_mutex_unlock(&tournament.sync_mutex);
    }
    
    return nullptr;
}

// 6. Funciones de monitoreo (Consola interactiva)

void player_stats(int player_id) {
    if (player_id >= 0 && player_id < app_config.n_players) {
        pthread_mutex_lock(&tournament.sync_mutex);
        Player& p = tournament.players[player_id];
        printf("\n[STATS] Jugador %d -> Wins: %d | Losses: %d | Draws: %d | ELO: %.1f\n\n",
               p.id, p.wins, p.losses, p.draws, p.elo);
        pthread_mutex_unlock(&tournament.sync_mutex);
    } else {
        cout << "ID de jugador invalido.\n";
    }
}

void current_matches() {
    pthread_mutex_lock(&tournament.sync_mutex);
    int count = 0;
    for (Board& b : tournament.boards) {
        if (b.is_busy) count++;
    }
    printf("\n[MATCHES] Partidas activas: %d\n", count);
    for (Board& b : tournament.boards) {
        if (b.is_busy) {
            printf(" - Tablero %d: Jugador %d vs Jugador %d\n", b.id, b.player1_id, b.player2_id);
        }
    }
    printf("\n");
    pthread_mutex_unlock(&tournament.sync_mutex);
}

void match_status(int game_id) {
    if (game_id >= 0 && game_id < app_config.k_boards) {
        pthread_mutex_lock(&tournament.sync_mutex);
        Board& b = tournament.boards[game_id];
        if (b.is_busy) {
            printf("\n[STATUS] Tablero %d [ACTIVO]: Jugador %d (X) vs Jugador %d (O)\n", b.id, b.player1_id, b.player2_id);
            
            // Dibujar el tablero 3x3
            printf("\n  %c | %c | %c \n", b.grid[0], b.grid[1], b.grid[2]);
            printf(" ---+---+---\n");
            printf("  %c | %c | %c \n", b.grid[3], b.grid[4], b.grid[5]);
            printf(" ---+---+---\n");
            printf("  %c | %c | %c \n\n", b.grid[6], b.grid[7], b.grid[8]);
            
            printf("Ultimas jugadas:\n");
            // Mostrar solo las ultimas 3 jugadas para no saturar la pantalla
            int start_idx = (b.move_history.size() > 3) ? b.move_history.size() - 3 : 0;
            for (size_t i = start_idx; i < b.move_history.size(); ++i) {
                printf("  > %s\n", b.move_history[i].c_str());
            }
            printf("\n");
        } else {
            printf("\n[STATUS] Tablero %d [LIBRE]\n\n", b.id);
        }
        pthread_mutex_unlock(&tournament.sync_mutex);
    } else {
        cout << "ID de tablero invalido.\n";
    }
}

// Rutina dedicada a procesar I/O para evitar bloquear la simulacion base
void* console_routine(void* arg) {
    string command;
    (void)arg; 

    while (tournament.is_running) {
        cin >> command;

        // Proteccion contra el estado de fallo (error en cin)
        if (cin.fail()) {
            cin.clear(); 
            cin.ignore(numeric_limits<streamsize>::max(), '\n'); 
            continue; // Vuelve al inicio del bucle a esperar
        }

        if (command == "player_stats") {
            int pid; 
            if (cin >> pid) {
                player_stats(pid);
            } else {
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                cout << "Error: Debes ingresar un numero de jugador.\n";
            }
        }
        else if (command == "current_matches") {
            current_matches();
        }
        else if (command == "match_status") {
            int bid; 
            if (cin >> bid) {
                match_status(bid);
            } else {
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                cout << "Error: Debes ingresar un numero de tablero.\n";
            }
        }
        else if (command == "exit") {
            handle_sigint(SIGINT);
        }
    }
    return nullptr;
}

// 7. Funcion Principal

int main() {
srand(time(NULL));

    //1. Carga de configuracion dinamica
    if (!cargarConfiguracion(".env")) {
        cerr << "Fallo al cargar el .env" << endl;
        return 1;
    }
    
    //2. Registro de señales del sistema operativo
    signal(SIGINT, handle_sigint);    
    cout << "Iniciando Torneo con " << app_config.n_players << " jugadores y " 
         << app_config.k_boards << " tableros..." << endl << endl;

    // 3. Inicializacion de estructuras e intento de restauracion de memoria
    tournament.players.resize(app_config.n_players);
    for (int i = 0; i < app_config.n_players; ++i) {
        tournament.players[i].id = i;
    }

    tournament.boards.resize(app_config.k_boards);
    for (int i = 0; i < app_config.k_boards; ++i) {
        tournament.boards[i].id = i;
    }

    load_snapshot();
    cout << endl;

    // 4. Inicializar Mutex y Variables de Condicion
    pthread_mutex_init(&tournament.sync_mutex, nullptr);
    pthread_cond_init(&tournament.match_cond, nullptr);
    pthread_cond_init(&tournament.board_cond, nullptr);

    // 5. Creacion y ejecucion de hilos 
    vector<pthread_t> player_threads(app_config.n_players);
    vector<pthread_t> board_threads(app_config.k_boards);

    // Lanzar hilos de Tableros
    for (int i = 0; i < app_config.k_boards; ++i) {
        pthread_create(&board_threads[i], nullptr, board_routine, (void*)(intptr_t)i);
    }

    // Lanzar hilos de Jugadores
    for (int i = 0; i < app_config.n_players; ++i) {
        pthread_create(&player_threads[i], nullptr, player_routine, (void*)(intptr_t)i);
    }
    pthread_create(&console_thread, nullptr, console_routine, nullptr);
    
    // 6. Esperar a que todos los hilos terminen (Join)
    for (int i = 0; i < app_config.n_players; ++i) {
        pthread_join(player_threads[i], nullptr);
    }
    for (int i = 0; i < app_config.k_boards; ++i) {
        pthread_join(board_threads[i], nullptr);
    }
    pthread_join(console_thread, nullptr);
    // 7. Destruir mutex y condiciones (Limpieza de memoria)
    pthread_mutex_destroy(&tournament.sync_mutex);
    pthread_cond_destroy(&tournament.match_cond);
    pthread_cond_destroy(&tournament.board_cond);

    save_snapshot();
    cout << "\nTorneo finalizado limpiamente." << endl;
    return 0;
}