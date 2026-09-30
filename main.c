#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h" //para usar o list_conf_files
#include <fcntl.h> //para poder usar o open() e as flags do open, como o O_RDONLY

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 6) { //mudança feita para a linha de comandos aceitar o input_dir
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
    return 1;
  }
  	const char *input_dir = argv[5]; //input_dir aponta para o sexto elemento da linha de comandos, que é a diretoria com os ficheiros a ler

	if(!path_exists(input_dir)){ //Valida que input_dir existe e que é uma diretoria
		fprintf(stderr, "Diretoria de entrada inválida.\n"); 
		return 1;
	}

	char conf_files[MAX_CONF_FILES][MAX_STRING_SIZE]; //declaramos o array de .conf files
	size_t num_conf_files; //declaramos o número de .conf files
	
	if(list_conf_files(input_dir, conf_files, &num_conf_files) != 0){ //em caso de erro (a diretoria não abriu, ou havia mais .conf's do que MAX_CONF_FILES permite)
		fprintf(stderr, "Failed to list .conf files.\n");
		return 1;
	}
	
	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	for(size_t i = 0; i < num_conf_files; i++){ //ciclo for para abrir cada ficheiro e processá-lo
		char full_path[MAX_PATH_SIZE];
		snprintf(full_path, MAX_PATH_SIZE, "%s/%s", input_dir, conf_files[i]); //constrói a string (o caminho) para passar ao open()
		int fd = open(full_path, O_RDONLY); //para abrir cada ficheiro .conf
		if(fd == -1){ //caso de erro
			perror("open");
			continue; //passar à próxima iteração do for, não processa o ficheiro que não foi aberto
		}
		int done = 0; //vai ser reiniciado a 0 com cada iteração do for
	

	while(!done){ //tirou-se o while(1)
		switch (get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				if(datacenter_define_VM(&dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}

				printf("VM successfully defined!\n");

				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0};

				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_reserve(&dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}

				printf("Reservation made successfully!\n");

				break;
			}

			case CMD_EXECUTE:
				char id[MAX_STRING_SIZE];

				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_execute(&dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				printf("Finished reservation execution!\n");

				break;

			case CMD_LIST:
				if (datacenter_list(&dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}

				break;

			case CMD_WAIT:
				unsigned int delay;

				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait(delay);
				break;

			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				done = 1;
				break;
		}
	}
	close(fd); 
}

datacenter_destroy(&dc); //já estava antes, pomos fora, liberta a memória que foi alocada ao longo da execução do programa pelo datacenter
return 0;