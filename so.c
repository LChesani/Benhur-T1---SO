#include "so.h"
#include "tela.h"
#include "exec.h"
#include <stdlib.h>

#define MAX 3 //numero maximo de processos


struct processo{
  int *maquina; //codigo do processo
  int tamanho;
  cpu_estado_t *cpue;
  processo_estado prce;
  //int *mem; //variavel que guarda a copia da memoria
  int removido; //pra nao precisar remover o processo da array, ocupa mais memória, mas economiza desempenho
};


struct so_t {
  cpu_modo_t inicial; //backup do modo inicial
  int n_processos; //variavel que guarda quantos procesos tem na bcp
  contr_t *contr;       // o controlador do hardware
  bool paniquei;        // apareceu alguma situação intratável
  cpu_estado_t *cpue;   // cópia do estado da CPU
  processo BCP[MAX];
  int *pr[MAX]; //programas
  int tamanhos[MAX]; //tamanho do programa do processo 
  int bloqueado; //index do processo que foi bloqueado
  int disp_bloq[MAX]; //dispositivo q gerou o bloqueio;
  so_chamada_t assassino[MAX]; //chamada q gerou o bloqueio
  int escolhido;
  int executando;
};



// funções auxiliares
static void init_mem(so_t *self);
static void panico(so_t *self);


so_t *so_cria(contr_t *contr)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;
  self->contr = contr;
  self->paniquei = false;
  self->cpue = cpue_cria();
  self->n_processos = 0;
 

  int init[] = {
    #include "init.maq"
  };
  
  int pr1[] = {
    #include "p1.maq"
  };

  int pr2[] = {
    #include "p2.maq"
  };

  self->tamanhos[0] = sizeof(init)/sizeof(init[0]);
  self->tamanhos[1] = sizeof(pr1)/sizeof(pr1[0]);
  self->tamanhos[2] = sizeof(pr2)/sizeof(pr2[0]);

  self->pr[0] = (int*) malloc(self->tamanhos[0] * sizeof(int));
  for(int i = 0; i < self->tamanhos[0]; i++){
    self->pr[0][i] = init[i];

  }
  self->pr[1] = (int*) malloc(self->tamanhos[1] * sizeof(int));
  for(int i = 0; i < self->tamanhos[1]; i++){
    self->pr[1][i] = pr1[i];
  }
  self->pr[2] = (int*) malloc(self->tamanhos[2] * sizeof(int));
  for(int i = 0; i < self->tamanhos[2]; i++){
    self->pr[2][i] = pr2[i];
  }
  
  init_mem(self);

  // coloca a CPU em modo usuário
  /*
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  cpue_muda_modo(self->cpue, usuario);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  */
  self->inicial = cpue_modo(self->cpue);
  return self;
}

void so_destroi(so_t *self)
{
  cpue_destroi(self->cpue);
  free(self);
}

static void escalonador(so_t *self){
  for(int i = 0; i < self->n_processos; i++){
    if(!self->BCP[i].removido && self->BCP[i].prce == pronto){ //pega o primeiro processo pronto da bcp
      self->escolhido = i;
      return;
    }
  }
  for(int i = 0; i < self->n_processos; i++){
    if(self->BCP[i].prce == exec){
      return;
    }
  }
  self->escolhido = -1;
}

static void despacho(so_t *self){
  escalonador(self);
  if(self->escolhido == -1){ //despacho n conseguiu escolher ngm e ngm ta executando
    cpue_muda_modo(self->cpue, zumbi);
    return;
  }
  self->BCP[self->escolhido].prce = exec; //estado do processo alterado para execucao
  self->executando = self->escolhido;
  for(int i = 0; i < mem_tam(contr_mem(self->contr)); i++){
    mem_t *aux2 = contr_mem(self->contr);
    if(mem_escreve(aux2, i, self->BCP[self->escolhido].maquina[i]) != ERR_OK){ //recupera a memória
      t_printf("so.despacho: erro de memória, endereco %d\n", i);
      panico(self);
    }
  }
  cpue_copia(self->BCP[self->escolhido].cpue, self->cpue); //recupera o estado do processador
}


static void so_trata_bloq(so_t *self){
  self->BCP[self->executando].prce = bloqueado;
  self->bloqueado = self->executando; //index do processo na tabela
  cpue_copia(self->cpue, self->BCP[self->bloqueado].cpue); //salva o contexto
  despacho(self);
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}




static void so_trata_sisop_le(so_t *self)
{
  // faz leitura assíncrona.
  // deveria ser síncrono, verificar es_pronto() e bloquear o processo
  int disp = cpue_A(self->cpue);
  int val;
  
  
  err_t err = es_le(contr_es(self->contr), disp, &val);


  cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
  // interrupção da cpu foi atendida
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  cpue_muda_A(self->cpue, err);




  if(err != ERR_OK){
    self->disp_bloq[self->executando] = disp;
    self->assassino[self->executando] = SO_LE;
    cpue_muda_erro(self->cpue, err, 0);
  }
  
  else{
    cpue_muda_X(self->cpue, val);
  }
  // incrementa o PC
  
  // altera o estado da CPU (deveria alterar o estado do processo)
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}


static void so_trata_sisop_escr(so_t *self)
{
  int disp = cpue_A(self->cpue);
  int val = cpue_X(self->cpue);

  err_t err = es_escreve(contr_es(self->contr), disp, val);
  cpue_muda_A(self->cpue, err);
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
    
  if(err != ERR_OK){
    self->disp_bloq[self->executando] = disp;
    self->assassino[self->executando] = SO_ESCR;
    cpue_muda_erro(self->cpue, err, 0);
  }
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// chamada de sistema para término do processo
static void so_trata_sisop_fim(so_t *self)
{
  self->BCP[self->executando].prce = -1; //indicando que ele sera removido, nao possui mais estado
  self->BCP[self->executando].removido = 1; //escalonador vai ignorar

  cpue_muda_erro(self->cpue, ERR_OK, 0);
  despacho(self);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// chamada de sistema para criação de processo



static void so_trata_sisop_cria(so_t *self)
{
  //basicamente todo o processo abaixo tá setando o processo na BCP na posição n_processos
  self->BCP[self->n_processos].tamanho = self->tamanhos[cpue_A(self->cpue)]; //tamanho do programa
  self->BCP[self->n_processos].maquina = (int*) malloc(mem_tam(contr_mem(self->contr)) * sizeof(int));
  for(int i = 0; i < self->BCP[self->n_processos].tamanho; i++){
    self->BCP[self->n_processos].maquina[i] = self->pr[cpue_A(self->cpue)][i];
    if(i == self->BCP[self->n_processos].tamanho-1){
      for(int j = i+1; j < mem_tam(contr_mem(self->contr)); j++){
        self->BCP[self->n_processos].maquina[j] = 0; //vai completar o resto da memoria com zeros
      }
    }
  }
  self->BCP[self->n_processos].removido = 0;
  self->BCP[self->n_processos].cpue = cpue_cria(); //malloc
  self->BCP[self->n_processos].prce = pronto;
  
  self->n_processos++;
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2); //incrementa o pc
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// trata uma interrupção de chamada de sistema
static void so_trata_sisop(so_t *self)
{
  // o tipo de chamada está no "complemento" do cpue
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  so_chamada_t chamada = cpue_complemento(self->cpue);
  switch (chamada) {
    case SO_LE:
      so_trata_sisop_le(self);
      break;
    case SO_ESCR:
      so_trata_sisop_escr(self);
      break;
    case SO_FIM:
      so_trata_sisop_fim(self);
      break;
    case SO_CRIA:
      so_trata_sisop_cria(self);
      break;
    default:
      t_printf("so: chamada de sistema não reconhecida %d\n", chamada);
      panico(self);
  }
}

// trata uma interrupção de tempo do relógio
static void so_trata_tic(so_t *self)
{
  for(int i = 0; i < self->n_processos; i++){
    if(self->BCP[i].prce == bloqueado){
      int disp = self->disp_bloq[i];
      acesso_t assassino;
      if(self->assassino[i] == SO_LE){
        assassino = leitura;
      }
      else if(self->assassino[i] == SO_ESCR){
        assassino = escrita;
      }
      if(!self->BCP[i].removido && es_pronto(contr_es(self->contr), disp, assassino)){ //verifica se algum processo precisa ser desbloqueado
        self->BCP[i].prce = pronto;
        if(cpue_modo(self->cpue) == zumbi){
          cpue_muda_modo(self->cpue, self->inicial);
          exec_altera_estado(contr_exec(self->contr), self->cpue);
        }
      }
    }
  }
  for(int i = 0; i < self->n_processos; i++){
    if(!self->BCP[i].removido){ //ainda tem processos pra executar
      return;
    }
  }
  //se chegar aqui é pq todos os processos estão marcados como removidos
  t_printf("Todos os processos terminaram de executar");
  contr_destroi(self->contr);
}

// houve uma interrupção do tipo err — trate-a
void so_int(so_t *self, err_t err){
  switch (err) {
    case ERR_SISOP:
      so_trata_sisop(self);
      break;
    case ERR_TIC:
      so_trata_tic(self);
      break;
    case ERR_OCUP: //caso de bloqueio por escrita ou leitura
      so_trata_bloq(self);
      break;
    default:
      t_printf("SO: interrupção não tratada [%s]", err_nome(err));
      self->paniquei = true;
  }
}

// retorna false se o sistema deve ser desligado
bool so_ok(so_t *self)
{
  return !self->paniquei;
}


// carrega um programa na memória
static void init_mem(so_t *self)
{
  // programa inicial para executar na nossa CPU




  int tam_progr = self->tamanhos[0]; //pr[0] é o init e tamanhos[0] é o tamanho de init
  // inicializa a memória com o programa 
  mem_t *mem = contr_mem(self->contr);

  self->BCP[self->n_processos].tamanho = tam_progr; //tamanho do programa
  self->BCP[self->n_processos].maquina = (int*) malloc(mem_tam(contr_mem(self->contr)) * sizeof(int));
  for(int i = 0; i < self->BCP[self->n_processos].tamanho; i++){
    self->BCP[self->n_processos].maquina[i] = self->pr[cpue_A(self->cpue)][i];
    if(i == self->BCP[self->n_processos].tamanho-1){
      for(int j = i+1; j < mem_tam(contr_mem(self->contr)); j++){
        self->BCP[self->n_processos].maquina[j] = 0;
      }
    }
  }
  self->BCP[self->n_processos].cpue = self->cpue;
  self->BCP[self->n_processos].prce = exec; //init sai executando
  self->executando = self->n_processos;
  

  self->n_processos++;

  for (int i = 0; i < tam_progr; i++) {
    if (mem_escreve(mem, i, self->pr[0][i]) != ERR_OK) {
      t_printf("so.init_mem: erro de memória, endereco %d\n", i);
      panico(self);
    }
  }
}
  
static void panico(so_t *self) 
{
  t_printf("Problema irrecuperável no SO");
  self->paniquei = true;
}
