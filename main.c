#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"
#include "filesystem.h" // para usar o list_conf_files
#include <fcntl.h> // para poder usar o open() e as flags do open, como o O_RDONLY
#include <pthread.h>

// Mutex global para proteger o estado do datacenter
pthread_mutex_t dc_mutex = PTHREAD_MUTEX_INITIALIZER;

// Estrutura para controlar a fila partilhada de ficheiros entre as threads
typedef struct {
    char conf_files[MAX_CONF_FILES][MAX_STRING_SIZE];
    size_t num_conf_files;
    size_t current_index;
    const char *input_dir;
    DataCenter *dc;
    pthread_mutex_t *mutex;
    pthread_mutex_t queue_mutex;
} ThreadPoolQueue;

// Função executada por cada worker thread para processar os ficheiros da fila
void *process_conf_file(void *arg) {
    ThreadPoolQueue *queue = (ThreadPoolQueue *)arg;
    char current_file[MAX_STRING_SIZE];

    while (1) {
        // Secção crítica para retirar o próximo ficheiro da fila de forma segura
        pthread_mutex_lock(&queue->queue_mutex);
        if (queue->current_index >= queue->num_conf_files) {
            pthread_mutex_unlock(&queue->queue_mutex);
            break; // Fila vazia, a thread termina
        }
        strcpy(current_file, queue->conf_files[queue->current_index]);
        queue->current_index++;
        pthread_mutex_unlock(&queue->queue_mutex);

        // Construir o caminho completo para abrir o ficheiro .conf
        char full_path[MAX_PATH_SIZE];
        snprintf(full_path, MAX_PATH_SIZE, "%s/%s", queue->input_dir, current_file);

        int fd = open(full_path, O_RDONLY);
        if (fd == -1) {
            perror("open conf file");
            continue;
        }

        int done = 0;
        while (!done) {
            switch (get_next_command(fd)) {
                case CMD_DEFINE: {
                    VMType vmtype;
                    if (parse_define(fd, &vmtype) != 0) {
                        fprintf(stderr, "Invalid define command in %s.\n", current_file);
                        break;
                    }

                    pthread_mutex_lock(queue->mutex);
                    if (datacenter_define_VM(queue->dc, &vmtype) != 0)
                        fprintf(stderr, "Failed to define VM.\n");
                    else
                        printf("VM successfully defined!\n");
                    pthread_mutex_unlock(queue->mutex);
                    break;
                }
                case CMD_RESERVE: {
                    Reservation reservation = {0};
                    if (parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS) == 0) {
                        fprintf(stderr, "Invalid reserve command in %s.\n", current_file);
                        break;
                    }

                    pthread_mutex_lock(queue->mutex);
                    reap_finished_vms(queue->dc); // liberta recursos de VMs que já acabaram
                    if (datacenter_reserve(queue->dc, &reservation) != 0)
                        fprintf(stderr, "Failed to reserve VMs.\n");
                    else
                        printf("Reservation made successfully!\n");
                    pthread_mutex_unlock(queue->mutex);
                    break;
                }
                case CMD_EXECUTE: {
                    char id[MAX_STRING_SIZE];
                    if (parse_execute(fd, id) != 0) {
                        fprintf(stderr, "Invalid execute command in %s.\n", current_file);
                        break;
                    }

                    pthread_mutex_lock(queue->mutex);
                    reap_finished_vms(queue->dc);
                    if (datacenter_execute(queue->dc, id) != 0)
                        fprintf(stderr, "Failed to execute reservation.\n");
                    else
                        printf("Finished reservation execution!\n");
                    pthread_mutex_unlock(queue->mutex);
                    break;
                }
                case CMD_LIST:
                    pthread_mutex_lock(queue->mutex);
                    reap_finished_vms(queue->dc);
                    if (datacenter_list(queue->dc) != 0)
                        fprintf(stderr, "Failed to list VMs.\n");
                    pthread_mutex_unlock(queue->mutex);
                    break;
                case CMD_WAIT: {
                    unsigned int delay;
                    if (parse_wait(fd, &delay) != 0) {
                        fprintf(stderr, "Invalid wait command in %s.\n", current_file);
                        break;
                    }
                    datacenter_wait(delay); // dorme sem segurar o mutex

                    pthread_mutex_lock(queue->mutex);
                    reap_finished_vms(queue->dc); // destrói as VMs que acabaram durante a espera
                    pthread_mutex_unlock(queue->mutex);
                    break;
                }
                case CMD_INVALID:
                    fprintf(stderr, "Invalid Command in %s.\n", current_file);
                    break;
                case CMD_HELP:
                case CMD_EMPTY:
                    break;
                case EOC:
                    done = 1;
                    break;
            }
        }
        close(fd);
    }
    return NULL;
}


int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <host_cores> <input_dir> <max_threads>\n", argv[0]);
        return 1;
    }
    const char *input_dir = argv[6];

    // Valida que input_dir existe e é uma diretoria
    if (!path_exists(input_dir)) {
        fprintf(stderr, "Diretoria de entrada inválida.\n");
        return 1;
    }

    static char conf_files[MAX_CONF_FILES][MAX_STRING_SIZE];
    size_t num_conf_files;

    // Obtém a lista de ficheiros .conf ordenados alfabéticamente
    if (list_conf_files(input_dir, conf_files, &num_conf_files) != 0) {
        fprintf(stderr, "Failed to list .conf files.\n");
        return 1;
    }

    size_t servers;
    size_t ram;
    size_t disk;
    double cpu;
    size_t host_cores;
    size_t max_threads;

    if (parse_size_t_arg(argv[1], &servers) != 0 ||
        parse_size_t_arg(argv[2], &ram) != 0 ||
        parse_size_t_arg(argv[3], &disk) != 0 ||
        parse_double_arg(argv[4], &cpu) != 0 ||
        parse_size_t_arg(argv[5], &host_cores) != 0 ||
        parse_size_t_arg(argv[7], &max_threads) != 0 ||
        max_threads == 0) {
        fprintf(stderr, "Invalid command line arguments.\n");
        return 1;
    }
    (void)host_cores; // ainda não usado (ver enunciado atualizado)

    DataCenter dc;
    datacenter_init(&dc);

    Resources resources = {
        .ram = ram,
        .disk = disk,
        .cpu = cpu
    };

    if (datacenter_configure(&dc, servers, &resources) != 0) {
        fprintf(stderr, "Failed to configure Data Center.\n");
        datacenter_destroy(&dc);
        return 1;
    }

    // Configurar a fila para as worker threads
    static ThreadPoolQueue queue;
    queue.num_conf_files = num_conf_files;
    queue.current_index = 0;
    queue.input_dir = input_dir;
    queue.dc = &dc;
    queue.mutex = &dc_mutex;
    pthread_mutex_init(&queue.queue_mutex, NULL);
    for (size_t i = 0; i < num_conf_files; i++) {
        strcpy(queue.conf_files[i], conf_files[i]);
    }

    // Criar as worker threads com base no argumento <max_threads>
    pthread_t *threads = malloc(max_threads * sizeof(pthread_t));
    if (!threads) {
        perror("malloc threads");
        datacenter_destroy(&dc);
        return 1;
    }

    size_t created = 0;
    for (size_t i = 0; i < max_threads; i++) {
        if (pthread_create(&threads[i], NULL, process_conf_file, &queue) != 0) {
            perror("pthread_create");
            break;
        }
        created++;
    }

    // Aguardar que todas as worker threads concluam o processamento
    for (size_t i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }

    // Espera que as VMs ainda em execução terminem (e sejam destruídas)
    wait_for_all_vms(&dc);

    // Libertar recursos e destruir mutexes
    free(threads);
    pthread_mutex_destroy(&queue.queue_mutex);
    pthread_mutex_destroy(&dc_mutex);

    datacenter_destroy(&dc);
    return 0;
}
