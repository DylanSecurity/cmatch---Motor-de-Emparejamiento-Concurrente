# cmatch---Motor-de-Emparejamiento-Concurrente
`cmatch` es un sistema de simulación multihilo escrito en C++ que empareja jugadores concurrentemente para jugar partidas de Tic-Tac-Toe, utilizando el sistema de puntuación ELO. Toda la concurrencia está gestionada nativamente a través de POSIX Threads (`pthread`), garantizando sincronización sin *busy-waiting*.

## Configuración (.env)
Antes de ejecutar, asegúrese de tener un archivo .env en la misma ruta con los siguientes parámetros mínimos:
```env
   N_PLAYERS=100
   K_BOARDS=20
   MAX_ELO_DIFF=50
   TURN_DELAY_MS=50
   REENTER_PROBABILITY=0.7
   K_ELO=30
   SNAPSHOT_PATH=dump.bin
   ```
## Ejecución
Para iniciar el simulador:
```env
   ./cmatch
   ```
## Consola de Monitoreo (Ejemplos Útiles)
Mientras el programa se ejecuta en segundo plano, la consola interactiva permite monitorear el estado en vivo. A continuación, ejemplos de los comandos disponibles:
1. **Ver estadísticas de un jugador específico:**
   Muestra victorias, derrotas, empates y ELO actual.
   ```bash
   player_stats 4 (Puede ser de cualquier id de jugador disponible)
   ```
2. **Ver todas las partidas activas:**
   Lista cuántos tableros están en uso y qué jugadores se están enfrentando en este milisegundo.
   ```bash
   current_matches
   ```
3. **Ver el estado detallado de un tablero:**
   Dibuja la grilla del Tic-Tac-Toe en tiempo real y muestra el historial de movimientos de esa partida.
   ```bash
   match_status 2 (Puede ser de cualquier match que este disponible en el momento)
   ```
## Apagado Seguro (Graceful Shutdown)
Para finalizar la ejecución de forma segura, presione Ctrl + C (SIGINT).
El programa evitará crear nuevos emparejamientos, esperará a que las partidas en curso finalicen y guardará el estado de los jugadores en el archivo dump.bin. Al volver a ejecutar el programa, los ELOs se restaurarán automáticamente.
