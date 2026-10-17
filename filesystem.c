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
//biblioteca com as funções para interagir com diretorias-tem o opendir()
#include <dirent.h> 
// para o stat()
#include <sys/stat.h> 
//para usar errno e EEXIST
#include <errno.h> 


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


//função que comparar nomes
int names_compare(const void *a, const void *b){ 
  return strcmp((const char *)a, (const char*)b);
}


//função para listar os ficheiros .conf
int list_conf_files(const char *input_dir, char conf_files[][MAX_STRING_SIZE], 
  size_t *num_conf_files ){
  //função para abrir o input_dir
  DIR *dir = opendir(input_dir); 
  if(dir == NULL){
    perror("opendir");
    //retorna 1 caso falhe
    return 1; 
  }
  //começa a 0 por causa do loop -> o primeiro .conf vai estar na posição 0
  //do array, e depois o seguinte na posicao 1, etc.., a lógica normal de array
  *num_conf_files = 0; 

  //função para o processamento das entradas
  struct dirent *entry; 
  while((entry = readdir(dir))!=NULL){
    if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0){
      //não copia . nem ..
      continue; 
    }

    //o tamanho do nome do ficheiro atual
    size_t len = strlen(entry -> d_name); 
    //o tamanho de .conf
    size_t ext_len = strlen(".conf"); 
    //se for .conf
    if(len > ext_len && strcmp(entry -> d_name + (len - ext_len), ".conf") == 0){ 
      
      //verificar se ainda há espaço, se a pasta tiver mais .conf's do que 
      //MAX_CONF_FILES, escrevemos para fora dos limites do array - crash
      if(*num_conf_files >= MAX_CONF_FILES){ 
        fprintf(stderr, "Too many .conf files.\n");
        closedir(dir);
        return 1;
      }

      //adiciona ao array
      strncpy(conf_files[*num_conf_files], entry -> d_name, MAX_STRING_SIZE - 1); 
      //para pôr cada string a terminar em \0
      conf_files[*num_conf_files][MAX_STRING_SIZE - 1] = '\0'; 
      
      //incrementa o valor que está a ser apontado
      (*num_conf_files)++; 
      
      //imprime apenas os .conf depois da filtragem feita acima
      printf("%s\n", entry -> d_name); 
    }
  }

  //fecha o diretorio
  closedir(dir); 
  //para organzizar por ordem alfabética
  qsort(conf_files, *num_conf_files, MAX_STRING_SIZE, names_compare); 
  return 0;
}


int copy_directory_recursively(const char *source, const char *dest){
  DIR *dir = opendir(source);
  if(dir == NULL){
    //caso de erro
    perror("opendir"); 
    return 1;
  }
  //não queremos ler . nem ..
  struct dirent *entry; 
  while((entry = readdir(dir)) != NULL){
    if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0){
      continue;
    }
    //precisamos do caminho completo tanto para a origem como para o destino
    char source_path[MAX_PATH_SIZE]; 
    char dest_path[MAX_PATH_SIZE];
    snprintf(source_path, MAX_PATH_SIZE, "%s/%s", source, entry -> d_name);
    snprintf(dest_path, MAX_PATH_SIZE, "%s/%s", dest, entry -> d_name);

    //para descobrir se a entrada é ficheiro ou subdiretoria 
    // - usamos stat sobre source_path
    struct stat st; 
    if(stat(source_path, &st) != 0){
      perror("stat");
      closedir(dir);
      return 1;
    }
    //no caso de ser uma subdiretoria - criar destino e cópia recursiva
    if(S_ISDIR(st.st_mode)){ 
      if(mkdir(dest_path, 0755) != 0 && errno != EEXIST){
        perror("mkdir");
        closedir(dir);
        return 1;
      }
      //aqui a recursividade - chamamos a função de novo para entrar 
      //na pasta criada com o makedir para criar uma pasta
      //ou copiar o conteúdo do ficheiro (bloco do else if)
      if(copy_directory_recursively(source_path, dest_path) != 0){ 
        closedir(dir);
        return 1;
      }
    }
    //no caso de ser um ficheiro - copia o seu conteúdo byte a byte
    else if (S_ISREG(st.st_mode)){ 
      //abrimos o ficheiro, com apenas permissão de leitura
      int source_fd = open(source_path, O_RDONLY); 
      //no caso de dar erro (o caminho não existe)
      if(source_fd == -1){ 
        perror("open (source)");
        closedir(dir);
        return 1;
      }

      int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        //no caso de dar erro (o caminho não existe, ou dest_path não existe, 
        // ou falta de permissões de escrita na pasta, ou o disco estar cheio)
        if(dest_fd == -1){ 
          perror("open (dest)"); 
          //limpa-se tudo o que já se tinha aberto 
          close(source_fd); 
          closedir(dir);
          return 1;
        }
      
      //array temporário onde vamos guardar os bytes lidos antes de os escrever
      char buf[10000]; 
      //guarda quantos bytes foram lidos em cada iteração
      ssize_t n; 
      //copia de source_fd para buff, e n fica com o valor de 
      //quantos bytes conseguiu ler
      while((n = read(source_fd, buf, sizeof(buf))) > 0){ 
        //escreve para o ficheiro de destino os n bytes que foram lidos
        if(write(dest_fd, buf, (size_t)n) != n){ 
          //e no caso de não se conseguir escrever os n bytes 
          // (devido a disco cheio ou assim), tratamos como erro e fechamos tudo
          perror("write"); 
          close(source_fd);
          close(dest_fd);
          closedir(dir);
          return 1;
        }
      }
      //pronto, no final fechamos os ficheiros que abrimos
      close(source_fd); 
      close(dest_fd);

    }
  }
  //fechamos a origem
  closedir(dir); 
  return 0;
}