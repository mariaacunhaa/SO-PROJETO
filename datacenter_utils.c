#include "datacenter_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "filesystem.h"


VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}


/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}


int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}


/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}


int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}


Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}


/**
 * Destroys the VM at position idx of the reservation: returns its resources to
 * the hosting server, removes it from the server's hosted list and from the
 * reservation, and frees it.
 */
static void vm_release(Reservation *reservation, size_t idx) {
	VM *vm = reservation->vms[idx];
	Server *server = vm->server;

	resources_add(&server->available, vm->type->required);

	for (size_t j = 0; j < server->num_hosted_vms; j++) {
		if (server->hosted_vms[j] == vm) {
			memmove(&server->hosted_vms[j],
							&server->hosted_vms[j + 1],
							(server->num_hosted_vms - j - 1) * sizeof(VM *));
			server->num_hosted_vms--;
			break;
		}
	}

	memmove(&reservation->vms[idx],
					&reservation->vms[idx + 1],
					(reservation->num_vms - idx - 1) * sizeof(VM *));
	reservation->num_vms--;
	reservation->vms[reservation->num_vms] = NULL;

	free(vm);
}


/**
 * Removes the reservation record from the Data Center (no VMs must be left).
 */
static void reservation_remove(DataCenter *dc, Reservation *reservation) {
	size_t idx = (size_t)(reservation - dc->reservations);

	if (idx >= dc->num_reservations)
		return;

	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));
	dc->num_reservations--;
}


void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	while (reservation->num_vms > 0)
		vm_release(reservation, reservation->num_vms - 1);

	reservation_remove(dc, reservation);
}


// configuração do ambiente e execução do processo filho da VM
void spawn_vm_child(VM *vm, const char *vm_dir, const char *exec_path,
										const char *cpulimit_str) {
	struct rlimit lim;

	// limitação do RAM convertendo GB para bytes
	lim.rlim_cur = (rlim_t)vm->type->required.ram << 30;
	lim.rlim_max = lim.rlim_cur;
	if (setrlimit(RLIMIT_AS, &lim) != 0)
		_exit(1);

	// limitação do tamanho máximo de ficheiro de escrita em GB
	lim.rlim_cur = (rlim_t)vm->type->required.disk << 30;
	lim.rlim_max = lim.rlim_cur;
	if (setrlimit(RLIMIT_FSIZE, &lim) != 0)
		_exit(1);

	// mudar para a diretoria isolada da VM no sistema de ficheiros temporários
	if (chdir(vm_dir) != 0)
		_exit(1);

	// preparar args e invocar o cpu limit via execvp para restringir uso de CPU
	char *args[] = {"cpulimit", "-q", "-f", "-l", (char *)cpulimit_str,
									"--", (char *)exec_path, NULL};

	execvp("cpulimit", args);
	// só se o exec falhar
	_exit(1); 
}



// Função auxiliar para fazer rollback das VMs já iniciadas até ao índice atual
static void rollback_vms(Reservation *res, size_t current_i) {
    for (size_t k = 0; k < current_i; k++) {
        if (res->vms[k]->pid > 0) {
            kill(res->vms[k]->pid, SIGKILL);
            waitpid(res->vms[k]->pid, NULL, 0);
            res->vms[k]->pid = 0;
            res->vms[k]->state = VM_STATE_RESERVED;
        }
    }
}

// percorre todas as VMs associadas a uma reserva e cria processo filho utilizando fork
int spawn_all_vms(DataCenter *dc, Reservation *res) {
    // cálculo do total de CPUs disponíveis no datacenter para calcular percentagem
	// de CPU exigida por cada VM
    double total_cpus = dc->servers[0].total.cpu * (double)dc->num_servers;

    for (size_t i = 0; i < res->num_vms; i++) {
        VM *vm = res->vms[i];

        char vm_dir[MAX_PATH_SIZE];
        char exec_path[MAX_PATH_SIZE];
        char cpulimit_str[32];

        // caminho da diretoria da VM
        int n = snprintf(vm_dir, sizeof(vm_dir), "/tmp/CloudIST/%s/%s", res->id, vm->id);
        if (n < 0 || (size_t)n >= sizeof(vm_dir)) {
            rollback_vms(res, i);
            return 1;
        }

        // resolve o caminho absoluto do executável para evitar falhas após o chdir do filho
        if (absolute_path(vm->type->exec_path, exec_path, sizeof(exec_path)) != 0) {
            fprintf(stderr, "Cannot resolve executable path of VM %s.\n", vm->id);
            rollback_vms(res, i);
            return 1;
        }

        // cálculo da percentagem de CPU exigida pela VM
        double cpu_pct = vm->type->required.cpu / total_cpus * 100.0;
        snprintf(cpulimit_str, sizeof(cpulimit_str), "%.2f", cpu_pct);

        // evita duplicação de output em buffer no processo filho após fork
        fflush(stdout); 
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            rollback_vms(res, i);
            return 1;
        }

        if (pid == 0) {
            // processo filho - chama a função de configuração e exec
            spawn_vm_child(vm, vm_dir, exec_path, cpulimit_str);
            // Se spawn_vm_child retornar por algum erro, o filho deve terminar
            exit(1); 
        }

        // processo pai - regista o PID do filho e atualiza estado da VM
        vm->pid = pid;
        vm->state = VM_STATE_RUNNING;
    }

    return 0;
}


/**
 * Finds the VM with the given pid and its reservation.
 */
static VM *find_vm_by_pid(DataCenter *dc, pid_t pid, Reservation **res_out,
													size_t *idx_out) {
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];
		for (size_t j = 0; j < res->num_vms; j++) {
			if (res->vms[j]->state == VM_STATE_RUNNING && res->vms[j]->pid == pid) {
				*res_out = res;
				*idx_out = j;
				return res->vms[j];
			}
		}
	}
	return NULL;
}


/**
 * Handles one finished VM process: destroys the VM (resources go back to its
 * server) and, if it was the last VM of its reservation, erases the reservation.
 */
static void handle_finished_vm(DataCenter *dc, pid_t pid) {
	Reservation *res;
	size_t idx;
	VM *vm = find_vm_by_pid(dc, pid, &res, &idx);
	if (vm == NULL)
		return;

	vm->state = VM_STATE_TERMINATED;
	vm_release(res, idx);

	if (res->num_vms == 0) {
		res->state = RES_STATE_FINISHED;
		reservation_remove(dc, res);
	}
}


// processa o fim de uma VM
void reap_finished_vms(DataCenter *dc) {
	for (;;) {
		pid_t pid = waitpid(-1, NULL, WNOHANG);

		if (pid < 0 && errno == EINTR)
			continue;
		if (pid <= 0) // nothing else finished (or no children)
			break;

		handle_finished_vm(dc, pid);
	}
}


static int any_vm_running(DataCenter *dc) {
	for (size_t i = 0; i < dc->num_reservations; i++)
		if (dc->reservations[i].state == RES_STATE_RUNNING)
			return 1;
	return 0;
}


// aguarda bloqueantemente até que todas as Vms em execução no data center terminem
// recolhendo os processos à medida que concluem
void wait_for_all_vms(DataCenter *dc) {
	while (any_vm_running(dc)) {
		// bloqueia até que alguma VM termine
		pid_t pid = waitpid(-1, NULL, 0);
		if (pid > 0)
			handle_finished_vm(dc, pid);
		else if (errno != EINTR)
			break;
	}
}


const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}


const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}