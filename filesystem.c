#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "constants.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h> //biblioteca com as funções para interagir com diretorias - no caso tem o opendir()
#include <sys/stat.h> // para o stat()
#include <errno.h> //para usar errno e EEXIST

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

int names_compare(const void *a, const void *b){ //função de comparar nomes
  return strcmp((const char *)a, (const char*)b);
}


int list_conf_files(const char *input_dir, char conf_files[][MAX_STRING_SIZE], size_t *num_conf_files ){ //função para listar os ficheiros .conf
  DIR *dir = opendir(input_dir); //função para abrir o input_dir
  if(dir == NULL){
    perror("opendir");
    return 1; //retorna 1 caso falhe
  }
  
  *num_conf_files = 0; //começa a 0 por causa do loop -> o primeiro .conf vai estar na posição 0 do array, e depois o seguinte na posicao 1, etc.., a lógica normal de arrays

  struct dirent *entry; //função para o processamento das entradas
  while((entry = readdir(dir))!=NULL){
    if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0){
      continue; //não copia . nem ..
    }

    size_t len = strlen(entry -> d_name); //o tamanho do nome do ficheiro atual
    size_t ext_len = strlen(".conf"); //o tamanho de .conf
    if(len > ext_len && strcmp(entry -> d_name + (len - ext_len), ".conf") == 0){ //se for .conf
      
      if(*num_conf_files >= MAX_CONF_FILES){ //verificar se ainda há espaço, se a pasta tiver mais .conf's do que MAX_CONF_FILES, escrevemos para fora dos limites do array - crash
        fprintf(stderr, "Too many .conf files.\n");
        closedir(dir);
        return 1;
      }

      strncpy(conf_files[*num_conf_files], entry -> d_name, MAX_STRING_SIZE - 1); //adiciona ao array
      conf_files[*num_conf_files][MAX_STRING_SIZE - 1] = '\0'; //para pôr cada string a terminar em \0
      
      (*num_conf_files)++; //incrementa o valor que está a ser apontado
      
      printf("%s\n", entry -> d_name); //imprime apenas os .conf depois da filtragem feita acima
    }
  }

  closedir(dir); //fecha o diretorio
  qsort(conf_files, *num_conf_files, MAX_STRING_SIZE, names_compare); //para organzizar por ordem alfabética
  return 0;
}

int copy_directory_recursively(const char *source, const char *dest){
  DIR *dir = opendir(source);
  if(dir == NULL){
    perror("opendir"); //caso de erro
    return 1;
  }
  struct dirent *entry; //não queremos ler . nem ..
  while((entry = readdir(dir)) != NULL){
    if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0){
      continue;
    }
    char source_path[MAX_PATH_SIZE]; //precisamos do caminho completo tanto para a origem como para o destino
    char dest_path[MAX_PATH_SIZE];
    snprintf(source_path, MAX_PATH_SIZE, "%s/%s", source, entry -> d_name);
    snprintf(dest_path, MAX_PATH_SIZE, "%s/%s", dest, entry -> d_name);

    struct stat st; //para descobrir se a entrada é ficheiro ou subdiretoria - usamos stat sobre source_path
    if(stat(source_path, &st) != 0){
      perror("stat");
      closedir(dir);
      return 1;
    }
    if(S_ISDIR(st.st_mode)){ //no caso de ser uma subdiretoria - criar destino e cópia recursiva
      if(mkdir(dest_path, 0755) != 0 && errno != EEXIST){
        perror("mkdir");
        closedir(dir);
        return 1;
      }
      if(copy_directory_recursively(source_path, dest_path) != 0){ //aqui a recursividade - chamamos a função de novo para entrar na pasta criada com o makedir para criar uma pasta ou copiar o conteúdo do ficheiro (bloco do else if)
        closedir(dir);
        return 1;
      }
    }
    else if (S_ISREG(st.st_mode)){ //no caso de ser um ficheiro - copia o seu conteúdo byte a byte
      
      int source_fd = open(source_path, O_RDONLY); //abrimos o ficheiro, com apenas permissão de leitura
      if(source_fd == -1){ //no caso de dar erro (o caminho não existe)
        perror("open (source)");
        closedir(dir);
        return 1;
      }

      int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(dest_fd == -1){ //no caso de dar erro (o caminho não existe, ou dest_path não existe, ou falta de permissões de escrita na pasta, ou o disco estar cheio)
          perror("open (dest)"); 
          close(source_fd); //limpa-se tudo o que já se tinha aberto 
          closedir(dir);
          return 1;
        }
      
      char buf[10000]; //array temporário onde vamos guardar os bytes lidos antes de os escrever
      ssize_t n; //guarda quantos bytes foram lidos em cada iteração
      while((n = read(source_fd, buf, sizeof(buf))) > 0){ //copia de source_fd para buff, e n fica com o valor de quantos bytes conseguiu ler
        if(write(dest_fd, buf, n) != n){ //escreve para o ficheiro de destino os n bytes que foram lidos
          perror("write"); //e no caso de não se conseguir escrever os n bytes (devido a disco cheio ou assim), tratamos como erro e fechamos tudo
          close(source_fd);
          close(dest_fd);
          closedir(dir);
          return 1;
        }
      }
      close(source_fd); //pronto, no final fechamos os ficheiros que abrimos
      close(dest_fd);
    }
  }
  closedir(dir); //fechamos a origem
  return 0;
}