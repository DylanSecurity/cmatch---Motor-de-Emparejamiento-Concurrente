# cmatch---Motor-de-Emparejamiento-Concurrente
`cmatch` es un sistema de simulación multihilo escrito en C++ que empareja jugadores concurrentemente para jugar partidas de Tic-Tac-Toe, utilizando el sistema de puntuación ELO. Toda la concurrencia está gestionada nativamente a través de POSIX Threads (`pthread`), garantizando sincronización sin *busy-waiting*.

## Configuración (.env)
Antes de ejecutar, asegúrese de tener un archivo .env en la misma ruta con los siguientes parámetros mínimos:
