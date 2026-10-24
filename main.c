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

    //declaração das variáveis para guardar os argumentos de linha de comando 
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
