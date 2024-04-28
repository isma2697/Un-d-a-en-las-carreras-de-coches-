#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include "falonso.h"


// Definiciones de constantes para el manejo de memoria compartida y semáforos
#define TAM_MEMORIA 600 // Tamaño máximo de la memoria compartida
#define LIBRE 1      // Estado de memoria compartida: libre
#define OCUPADO 0   // Estado de memoria compartida: ocupado
#define OCUPADO_SV 2 // Estado de memoria compartida: ocupado por semáforo vertical
#define OCUPADO_SH 3  // Estado de memoria compartida: ocupado por semáforo horizontal
#define INF 999999  // Valor infinito para ciertas operaciones

sigset_t mask, maskInicio;

union semun{
	int val;
        struct semid_ds *buf;
};

// Definición de la estructura para las variables globales
typedef struct globalVars{
	int sharedMemory; // Identificador de la memoria compartida
	int semaforos;  // Identificador del conjunto de semáforos
	char *pMemory;   // Puntero a la memoria compartida
	int *contador;   // Puntero al contador global
	int hayCoches;   // Flag para indicar si hay coches presentes
	int numCoches;  // Número total de coches
} globalVars;

// Instancia de las variables globales
globalVars gV;

// Declaración de funciones propias
int tengoCocheDelante(int posicion, char *pMemory, int carril); //Consulta si hay un coche adelante en el mismo carril
int mirarSemaforo(int posicion, char *pMemory, int carril); // Consulta el color del semáforo
int mirarAdelantar(int posicion, int carril, char *pMemory, int color); //Consulta la posición de cambio de carril y evalúa si está ocupada
void seccion_critica(int sem_num, int option, struct sembuf *sops); // Realiza operaciones críticas sobre un semáforo
void esperar_y_controlar_signal(int segundos, struct sembuf *sops); // Espera por un tiempo y controla las señales recibidas

// Manejadora de señal para la señal vacía
void handlerVacia(int signal){
	// Esta función podría cambiar la acción por defecto de una señal específica
	
}

//Manejadora de señal para la señal SIGINT (Ctrl+C)
void handler(int signal){
	int status;
	int numeroCoches = gV.numCoches;
	
	//Espera por todos los procesos hijos (coches)
	while(numeroCoches > 0) {
		if(waitpid(-1, &status, 0) == -1) {
			perror("Fatal error in waitpid");
		}
		numeroCoches--;
	}
	//Realiza acciones de limpieza antes de salir del programa
	if(fin_falonso(gV.contador) == -1){fprintf(stderr, "fatal error in fin_falonso\n"); exit(1);} 
	
	 // Elimina los semáforos si existen
	if(gV.semaforos != -1) {
		if(semctl(gV.semaforos, 0, IPC_RMID) == -1) {
			perror("Fatal error in semctl");
		}
	}

	// Desvincula la memoria compartida si está unida
	if(gV.sharedMemory != -1) {
		if(shmdt((char *)gV.pMemory) == -1) {
			perror("Fatal error in shmdt");
		}
		 // Elimina la memoria compartida
		if(shmctl(gV.sharedMemory, IPC_RMID, NULL) == -1) {
			perror("Fatal error in shmctl");
		}
	}
	// Sale del programa
	exit(0);
}

//Función para imprimir el error en los parámetros iniciales de entrada
void printError(char *msgError){
	sprintf(msgError, "ERROR: ./falonso.c {numero de coches(max.20)} {velocidad(0/1)}\n\n");
	write(1, msgError, strlen(msgError));
}

// Función para configurar el manejo de la señal SIGINT (Ctrl+C)
void configurarSIGINT(struct sigaction *accion, struct sigaction *accionNula, struct sigaction *accionVieja, sigset_t *maskInicio) {
	// Llena el conjunto de señales con todas las señales disponibles
    if(sigfillset(&mask) == -1) {
        perror("Fatal error in sigfillset");
        return;
    }
     // Elimina SIGINT y SIGALRM del conjunto de señales
    if(sigdelset(&mask, SIGINT) == -1 || sigdelset(&mask, SIGALRM) == -1) {
        perror("Fatal error in sigdelset");
        return;
    }
     // Configura la acción para la señal SIGINT
    accion->sa_handler = handler;
    accion->sa_mask = mask;
    accion->sa_flags = 0;

    // Bloquea todas las señales excepto SIGINT y SIGALRM
    if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
        perror("Fatal error in sigprocmask");
        return;
    }
     // Inicializa el conjunto de señales de inicio con SIGINT
    if(sigemptyset(maskInicio) == -1 || sigaddset(maskInicio, SIGINT) == -1) {
        perror("Fatal error in sigfillset");
        return;
    }
     // Configura la acción para la señal vacía
    accionNula->sa_handler = handlerVacia;
    accionNula->sa_mask = mask;
    accionNula->sa_flags = 0;
    
    // Bloquea SIGINT en el conjunto de señales de inicio
    if(sigprocmask(SIG_BLOCK, maskInicio, NULL) == -1) {
        perror("Fatal error in sigprocmask");
        return;
    }
    // Asigna las acciones para las señales SIGINT y SIGALRM
    if(sigaction(SIGINT, accion, accionVieja) == -1 || sigaction(SIGALRM, accionNula, NULL) == -1) { 
        perror("Fatal error in sigaction");
        return;
    }
}


int main(int argc, char *argV[]){

	// Variables 
	gV.sharedMemory = -1;
	gV.semaforos = -1;
	gV.pMemory = NULL;
	gV.contador = NULL;
	
	union semun su; //Para que funcione semctl en encima
	
	int i, j, carrilAux, posAux, carril, posicion, color, velocimetro;
	char msgError[100];
	int modo, procesoAvanza;
	gV.hayCoches=0;
	int *miPos = NULL;
	int *miCarril = NULL;
	int *posConsulta = NULL;
	int *carrilConsulta = NULL;
	
	
	struct sigaction accion, accionNula, accionVieja;
    configurarSIGINT(&accion, &accionNula, &accionVieja, &maskInicio);
	
	// Precondiciones
	if(argc != 3){
		printError(msgError); // Imprime un mensaje de error si los parámetros de entrada son incorrectos
		return 1;
	}
	else{
		gV.numCoches = atoi(argV[1]); // Convierte el número de coches a entero
		
		if(gV.numCoches<1 || gV.numCoches>20){
			printError(msgError); // Imprime un mensaje de error si el número de coches está fuera del rango permitido
			return 1;
		}
		
		modo = atoi(argV[2]); // Convierte el modo a entero
		
		if(strcmp(argV[2], "0")!=0 && strcmp(argV[2], "1")!=0){
			printError(msgError); // Imprime un mensaje de error si el modo no es válido
			return 1;
		}
	}

	
	// Memoria Compartida
	if((gV.sharedMemory = shmget(IPC_PRIVATE, TAM_MEMORIA, 0664 | IPC_CREAT)) == -1) {
        	perror("Fatal error in shmget"); // Imprime un mensaje de error si falla la creación de la memoria compartida
    	}
	
	if((gV.pMemory = shmat(gV.sharedMemory, (void *)0, 0)) == NULL){ //Asociacion
		perror("Fatal error in shmat"); // Imprime un mensaje de error si falla la asociación de la memoria compartida
	}
	
	gV.contador = (int *)(gV.pMemory+304); // Establece el puntero al contador en la memoria compartida
	*(gV.contador) = 0; // Inicializa el contador en 0

	 
	//Semaforos 
	int numSemaforos = 5 + 1 + gV.numCoches; // Calcula el número total de semáforos necesarios
	struct sembuf sops[1]; // Arreglo de operaciones sobre semáforos
	gV.semaforos = semget(IPC_PRIVATE, numSemaforos, IPC_CREAT | 0600); // Crea el conjunto de semáforos

		//su.array=NULL;
	su.buf=NULL; // Inicializa el buffer de la estructura de la unión
	
	su.val=0; if(semctl(gV.semaforos, 1, SETVAL, su) == -1) perror("Fatal error in semctl");
	su.val=1; if(semctl(gV.semaforos, 2, SETVAL, su) == -1) perror("Fatal error in semctl");
	su.val=1; if(semctl(gV.semaforos, 3, SETVAL, su) == -1) perror("Fatal error in semctl");
	su.val=1; if(semctl(gV.semaforos, 4, SETVAL, su) == -1) perror("Fatal error in semctl"); 
	su.val=1; if(semctl(gV.semaforos, 5, SETVAL, su) == -1) perror("Fatal error in semctl"); 
	
	// Inicializa los semáforos de los coches
	for(i=1; i<=gV.numCoches; i++){
		su.val=0; if(semctl(gV.semaforos, 5+gV.numCoches, SETVAL, su) == -1) perror("Fatal error in semctl");
	}
	

	//Pintamos el circuito y configuramos las señales
	if(inicio_falonso(modo, gV.semaforos, gV.pMemory) == -1){ fprintf(stderr, "fatal error in inicio_falonso\n"); return 1;}
	if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
	
	//Creación de procesos(coches)
	for(i=1; i<=gV.numCoches; i++){
		switch(fork()){
			case -1: perror("Fatal error in fork...\n"); return 1;
		
			case 0: //codigo del hijo
				if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
				
				// Restaurar la acción original de SIGINT
				if(sigaction(SIGINT, &accionVieja, NULL) == -1){ 
					perror("Fatal error in sigaction");
					return 1;
				}
				
				srand(getpid()); // Inicializar la semilla para números aleatorios
				
				carril = (rand()) % 2; //Carril aleatorio entre 0 y 1
				posicion = i; //Posición inicial distinta para cada proceso
				velocimetro = (rand()) % 99 + 1; //Velocidad inicial aleatoria entre 1 y 99
				// Generar un color inicial aleatorio
				do{
					color = (rand()) % 8 + 16; //Color inicial aleatorio(+16 para verlo más vivo)	
				}while(color==AZUL); // Asegurarse de que el color no sea AZUL
				
				//Guardo mi situacion en la memoria compartida
				miPos = (int *)(gV.pMemory+320+8*(i-1));
				*miPos = INF;
				miCarril = (int *)(gV.pMemory+320+8*(i-1)+4);
				*miCarril = INF;
				
				//Pintar el coche en el circuito
				if(inicio_coche(&carril, &posicion, color) == -1){fprintf(stderr, "fatal error in inicio_coche\n"); return 1;}
				
				//Los coches esperan a que todos hayan llegado a la salida
				if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
				seccion_critica(1, 1, sops); // El coche espera en la sección crítica 1 hasta que todos los coches estén listos para avanzar
	     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_1"); return 1;}
	     			
	     			seccion_critica(2, 0, sops); // El coche libera la sección crítica 2 para permitir que los demás coches avancen
	     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_2"); return 1;}
	     			if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
	     			
				
				while(1){
					if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
					
					// El coche adquiere la sección crítica 3 para poder avanzar
					seccion_critica(3, -1, sops);
	     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_3"); return 1;}
					
					// Si el semáforo vertical está rojo, el coche se detiene
					if(mirarSemaforo(posicion, gV.pMemory, carril) == OCUPADO_SV){
		     				seccion_critica(3, 1, sops);
		     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_4"); return 1;}
		     				
		     				//Suelto la sección crítica y me quedo esperando a VERDE
		     				if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
		     				seccion_critica(4, 0, sops); 
		     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_5"); return 1;}
		     				if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
					}
					
					//Si el semáforo horizontal está rojo, el coche se detiene
					else if(mirarSemaforo(posicion, gV.pMemory, carril) == OCUPADO_SH){
						seccion_critica(3, 1, sops);
		     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_6"); return 1;}
		     				
		     				//El coche libera la sección crítica 5 y espera a que el semáforo esté verde
		     				if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
		     				seccion_critica(5, 0, sops);
		     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_7"); return 1;}
		     				if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
					}
					
					// Si el semáforo está verde (o no está en frente) y no hay nadie delante, el coche avanza
					else if(tengoCocheDelante(posicion, gV.pMemory, carril)==LIBRE){
						if(avance_coche(&carril, &posicion, color) == -1){fprintf(stderr, "fatal error in avance_coche\n"); return 1;}
						*miPos = INF;
						*miCarril = INF; // Eliminar la situación del coche de la memoria compartida

						
						//Actualización atómica de la variable compartida: contador
						if( (carril==CARRIL_DERECHO && posicion==133) || (carril==CARRIL_IZQUIERDO && posicion==131)){
							(*(gV.contador))++;
						}
						
						//Avisamos al coche que estaba esperando detrás de nosotros que puede avanzar
						
						for(j=320; j<gV.numCoches*8+320; j=j+8){
							posConsulta = (int*)(gV.pMemory+j);
							carrilConsulta = (int*)(gV.pMemory+j+4);
							
							if(posicion==0){
								if(*posConsulta == 135 && *carrilConsulta == carril){
									seccion_critica((j-320)/8 + 6, 1, sops);
				     					if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_8"); return 1;}
								}
							}
							
							else if(posicion==1){
								if(*posConsulta == 136 && *carrilConsulta == carril){
									seccion_critica((j-320)/8 + 6, 1, sops);
				     					if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_9"); return 1;}
								}
							}
							
							else if(*posConsulta == posicion-2 && *carrilConsulta == carril){
								seccion_critica((j-320)/8 + 6, 1, sops);
			     					if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_10"); return 1;}
							}
						}
			     			
			     			
			     			//Avisamos al coche que estaba esperando justo fuera del cruce de que puede avanzar
						// Determinamos la posición y carril del coche que necesita ser avisado para avanzar
					     	if(posicion==23 && carril==CARRIL_DERECHO){ posAux=101-1; carrilAux=CARRIL_IZQUIERDO;}
					     	if(posicion==21 && carril==CARRIL_DERECHO){ posAux=108-1; carrilAux=CARRIL_DERECHO;}
					     	if(posicion==106 && carril==CARRIL_DERECHO){ posAux=23-1; carrilAux=CARRIL_IZQUIERDO;}
					     	if(posicion==108 && carril==CARRIL_DERECHO){ posAux=21-1; carrilAux=CARRIL_DERECHO;}
					     	if(posicion==23 && carril==CARRIL_IZQUIERDO){ posAux=106-1; carrilAux=CARRIL_DERECHO;}
					     	if(posicion==25 && carril==CARRIL_IZQUIERDO){ posAux=99-1; carrilAux=CARRIL_IZQUIERDO;}
					     	if(posicion==99 && carril==CARRIL_IZQUIERDO){ posAux=25-1; carrilAux=CARRIL_IZQUIERDO;}
					     	if(posicion==101 && carril==CARRIL_IZQUIERDO){ posAux=23-1; carrilAux=CARRIL_DERECHO;}
					     	
						// Buscamos entre todos los coches en el circuito para encontrar el que estaba esperando en la posición y carril adecuados		
					     	for(j=320; j<gV.numCoches*8+320; j=j+8){
							posConsulta = (int*)(gV.pMemory+j);
							carrilConsulta = (int*)(gV.pMemory+j+4);

							// Si encontramos el coche que estaba esperando, lo liberamos para que pueda avanzar		
							if(*posConsulta == posAux && *carrilConsulta == carrilAux){
								seccion_critica((j-320)/8 + 6, 1, sops);
					     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_11"); return 1;}
							}
						}
					     	
						// Liberamos la sección crítica 3 para que el coche pueda avanzar
						seccion_critica(3, 1, sops);
			     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_12"); return 1;}
						// Calculamos la velocidad del coche y avanzamos
						if(velocidad(velocimetro, carril, posicion) == -1){fprintf(stderr, "fatal error in velocidad\n"); return 1;}
					}

					else if((procesoAvanza = tengoCocheDelante(posicion, gV.pMemory, carril)) > LIBRE){
						switch(procesoAvanza){
							// Casos específicos de procesos que están delante y requieren atención
							case 105:
							case 107:
							case 98:
							case 100:
							case 221:
							case 20:
							case 24:
							case 220:
								// Escribimos nuestra posición actual en la memoria compartida
								*miPos = posicion;
				     				*miCarril = carril;
								//Liberamos la sección crítica 3 para que otros coches puedan avanzar
								seccion_critica(3, 1, sops);
					     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_13"); return 1;}
					     			
					     			//Esperamos hasta que el coche que está delante nos avise de que podemos avanzar
					     			if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
					     			seccion_critica(5+i, -1, sops);
					     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_14"); return 1;}
					     			if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
						}
					}
					
					//Si estoy obstaculizado por delante...
					else if(tengoCocheDelante(posicion, gV.pMemory, carril) == OCUPADO){
					
						//Si puedo adelantar, me cambio de carril...
						if(mirarAdelantar(posicion, carril, gV.pMemory, color)){
							// Guardo la posición y carril original en la memoria compartida
							posAux = posicion-1;
							carrilAux = carril;
							if(cambio_carril(&carril, &posicion, color) == -1){fprintf(stderr, "fatal error in cambio_carril\n"); return 1;} // Realizo el cambio de carril
							*miPos = INF;
							*miCarril = INF; //Borro mi situación de la memoria compartida
							
							// Aviso al coche que estaba esperando detrás de mí que puede avanzar
							
							for(j=320; j<gV.numCoches*8+320; j=j+8){
								posConsulta = (int*)(gV.pMemory+j);
								carrilConsulta = (int*)(gV.pMemory+j+4);
								if(*posConsulta == posAux && *carrilConsulta == carrilAux){
									seccion_critica((j-320)/8 + 6, 1, sops);
					     				if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_15"); return 1;}
								}
							}
					     		
							// Libero la sección crítica 3 para que otros coches puedan avanzar
							seccion_critica(3, 1, sops);
		     					if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_16"); return 1;}
								// Calculo la velocidad del coche y avanzo
							if(velocidad(velocimetro, carril, posicion) == -1){fprintf(stderr, "fatal error in velocidad\n"); return 1;}
						}
					
						// Si no puedo adelantar (obstaculizado totalmente), me paro...
						else{
							// Guardo mi posición y carril en la memoria compartida
							*miPos = posicion;
				     			*miCarril = carril;
							// Libero la sección crítica 3 para que otros coches puedan avanzar
							seccion_critica(3, 1, sops);
				     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_17"); return 1;}
				     			
				     			//Esperar hasta que el coche que tengo delante me avise de que puedo avanzar
				     			if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
				     			seccion_critica(5+i, -1, sops);
				     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_18"); return 1;}
				     			if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
	     					}
					}
					if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
				}
			return 0;
			
			default: //Codigo del padre
				//Espero a que todos los hijos hayan llegado para dar el banderazo (un wait por hijo)
				if(sigprocmask(SIG_UNBLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
				seccion_critica(1, -1, sops);
	     			if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_19"); return 1;}
	     			if(sigprocmask(SIG_BLOCK, &maskInicio, NULL)==-1){ perror("fatal error in sigprocmask"); return 1;}
		}
	}
	
	gV.hayCoches=1; // Marca que hay coches en la pista

	
	//Captura del CTRL+C
	if(sigaction(SIGINT, &accion, NULL) == -1){ 
		perror("Fatal error in sigaction");
		return 1;
	}
	
	//Cambio de acción a SIGALRM
	if(sigaction(SIGALRM, &accionNula, NULL) == -1){ 
		perror("Fatal error in sigaction");
		return 1;
	}
	

	seccion_critica(2, -1, sops);
	if( (semop(gV.semaforos, sops, 1)) == -1){ perror("Fatal error in semop_20"); return 1;}
	
	
	while(1){ //Bucle para ir cambiando el semaforo
		// Cambiar el semáforo horizontal a verde y el semáforo vertical a rojo
		if(luz_semAforo(HORIZONTAL, VERDE) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;} // Configurar semáforo horizontal en verde
		if(luz_semAforo(VERTICAL, ROJO) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;} //Configurar semáforo vertical en rojo
		   // Liberar la sección crítica para permitir que los coches avancen
		seccion_critica(5, -1, sops);// Sección crítica para el semáforo horizontal
		esperar_y_controlar_signal(3, sops);// Esperar 3 segundos

		    // Cambiar el semáforo horizontal a amarillo y tomar la sección crítica nuevamente
		if(luz_semAforo(HORIZONTAL, AMARILLO) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;}; // Configurar semáforo horizontal en amarillo
		seccion_critica(5, 1, sops);// Tomar la sección crítica para el semáforo horizontal
		esperar_y_controlar_signal(1, sops);// Esperar 1 segundo
	
		 // Cambiar el semáforo horizontal a rojo y el semáforo vertical a verde
		if(luz_semAforo(HORIZONTAL, ROJO) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;} // Configurar semáforo horizontal en rojo
		if(luz_semAforo(VERTICAL, VERDE) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;} // Configurar semáforo vertical en verde
		// Liberar la sección crítica para permitir que los coches avancen en la dirección vertical
		seccion_critica(4, -1, sops);// Sección crítica para el semáforo vertical
		esperar_y_controlar_signal(3, sops); // Sección crítica para el semáforo vertical
		
		// Cambiar el semáforo vertical a amarillo y tomar la sección crítica nuevamente
		if(luz_semAforo(VERTICAL, AMARILLO) == -1){fprintf(stderr, "fatal error in luz_semaforo\n"); return 1;}; // Configurar semáforo vertical en amarillo
		seccion_critica(4, 1, sops); // Tomar la sección crítica para el semáforo vertical
		esperar_y_controlar_signal(1, sops); // Esperar 1 segundo
		
	}
		
	return 0;// El bucle es infinito, así que nunca se llega aquí, pero se agrega para completar la función principal
}

int tengoCocheDelante(int posicion, char *pMemory, int carril){
	int posSig;
	
	if(posicion==136) posicion=-1; // El circuito es circular, volvemos al inicio si llegamos al final
	// Las posiciones del carril izquierdo están a partir de la posición 137 en la memoria compartida
	if(carril==CARRIL_IZQUIERDO) posicion+=137;
	
	// Casos especiales del cruce (consultar cuadro anexo para detalles)
	if ((posicion == (106 - 1) && pMemory[23 + 137] != ' ') || 
    (posicion == (108 - 1) && pMemory[21] != ' ')) return posicion - 1;

	if ((posicion == (99 + 137 - 1) && pMemory[25 + 137] != ' ') || 
		(posicion == (101 + 137 - 1) && pMemory[23] != ' ')) return posicion - 1;

	if ((posicion == (23 + 137 - 1) && pMemory[106] != ' ') || 
		(posicion == (21 - 1) && pMemory[108] != ' ')) return posicion - 1;

	if ((posicion == (25 + 137 - 1) && pMemory[99 + 137] != ' ') || 
		(posicion == (23 - 1) && pMemory[101 + 137] != ' ')) return posicion - 1;
	
	// Casos habituales
	posSig = posicion+1;
	
	if(pMemory[posSig] == ' ') return LIBRE; // No hay coche delante
	else return OCUPADO; // Hay un coche delante
}

int mirarSemaforo(int posicion, char *pMemory, int carril){
	// Las posiciones 20, 22, 105 y 98 son las posiciones inmediatamente anteriores a los puntos de comprobación de semáforo
	
	// Comprobar el estado del semáforo vertical
	// Si estamos en la posición de control del semáforo vertical para el carril derecho o izquierdo
	// Evaluamos el estado del semáforo vertical
    if ((carril == CARRIL_DERECHO && posicion == 20) || (carril == CARRIL_IZQUIERDO && posicion == 22)) {
        if (pMemory[275] == VERDE) return LIBRE;
        else return OCUPADO_SV; // Semaforo vertical en rojo o amarillo
    }
    
  	  // Comprobar el estado del semáforo horizontal
	// Si estamos en la posición de control del semáforo horizontal para el carril derecho o izquierdo
       	 // Evaluamos el estado del semáforo horizontal
    if ((carril == CARRIL_DERECHO && posicion == 105) || (carril == CARRIL_IZQUIERDO && posicion == 98)) {
        if (pMemory[274] == VERDE) return LIBRE; // Si el semáforo horizontal está en verde, podemos avanzar
        else return OCUPADO_SH;  // Si el semáforo horizontal está en rojo o amarillo, debemos detenernos
    }

	// Cuando consultamos el semáforo y estamos lejos de él, siempre recibimos LIBRE
   	 // Esto significa que si no estamos cerca de ningún punto de control de semáforo, podemos avanzar sin restricciones
	return LIBRE; 
}

int mirarAdelantar(int posicion, int carril, char *pMemory, int color) {
    // Comprobación de semáforo
    if (pMemory[274] == ROJO && ((carril == CARRIL_DERECHO && posicion - 5 == 99) || 
                                  (carril == CARRIL_IZQUIERDO && posicion + 5 == 106)))
        return 0;
    if (pMemory[275] == ROJO && ((carril == CARRIL_DERECHO && posicion + 1 == 23) || 
                                  (carril == CARRIL_IZQUIERDO && posicion - 1 == 21)))
        return 0;

    // Equivalencias de adelantamientos
    if (carril == CARRIL_DERECHO) {
        if ((posicion >= 0 && posicion <= 13) || (posicion >= 29 && posicion <= 60)) {
            if (pMemory[posicion + 137] == ' ') return 1;
        } else if (posicion >= 14 && posicion <= 28) {
            if (posicion == 22 && (pMemory[23 + 137] != ' ' || pMemory[106] != ' ')) return 0;
            if (posicion == 24 && (pMemory[25 + 137] != ' ' || pMemory[99 + 137] != ' ')) return 0;
            if (pMemory[posicion + 137 + 1] == ' ') return 1;
        } else if (posicion >= 61 && posicion <= 68) {
            if (pMemory[posicion + 137 - (65 - posicion)] == ' ') return 1;
        } else if (posicion >= 69 && posicion <= 129) {
            if (posicion == 106 && (pMemory[101 + 137] != ' ' || pMemory[23] != ' ')) return 0;
            if (posicion == 104 && (pMemory[99 + 137] != ' ' || pMemory[25 + 137] != ' ')) return 0;
            if (pMemory[posicion + 137 - 5] == ' ') return 1;
        } else if (posicion >= 130 && posicion <= 136) {
            if (pMemory[posicion + 137 - (130 - posicion)] == ' ') return 1;
        }
        return 0;
    } else if (carril == CARRIL_IZQUIERDO) {
        if ((posicion >= 0 && posicion <= 15) || (posicion >= 29 && posicion <= 58)) {
            if (pMemory[posicion] == ' ') return 1;
        } else if (posicion >= 16 && posicion <= 28) {
            if (posicion == 22 && (pMemory[21] != ' ' || pMemory[108] != ' ')) return 0;
            if (posicion == 24 && (pMemory[23] != ' ' || pMemory[101 + 137] != ' ')) return 0;
            if (pMemory[posicion - 1] == ' ') return 1;
        } else if (posicion >= 59 && posicion <= 64) {
            if (pMemory[posicion + (posicion - 59)] == ' ') return 1;
        } else if (posicion >= 65 && posicion <= 125) {
            if (posicion == 101 && (pMemory[106] != ' ' || pMemory[23 + 137] != ' ')) return 0;
            if (posicion == 103 && (pMemory[108] != ' ' || pMemory[21] != ' ')) return 0;
            if (pMemory[posicion + 5] == ' ') return 1;
        } else if (posicion >= 126 && posicion <= 133) {
            if (pMemory[posicion + (posicion - 126) + 4] == ' ') return 1;
        } else if (posicion >= 134 && posicion <= 136) {
            if (pMemory[136] == ' ') return 1;
        }
        return 0;
    }
}

//Operaciones recurrentes sobre semáforos
void seccion_critica(int sem_num, int option, struct sembuf *sops){
	sops[0].sem_num = sem_num;
	sops[0].sem_op = option;
	sops[0].sem_flg = 0;
}

void esperar_y_controlar_signal(int segundos, struct sembuf *sops) {
    // Realizar la operación semop
    if (semop(gV.semaforos, sops, 1) == -1) {
        perror("Fatal error in semop");
        exit(1);
    }

    // Configurar la alarma
    alarm(segundos);

    // Desbloquear la señal
    if (sigprocmask(SIG_UNBLOCK, &maskInicio, NULL) == -1) {
        perror("fatal error in sigprocmask");
        exit(1);
    }

    // Esperar la señal
    pause();

    // Bloquear la señal nuevamente
    if (sigprocmask(SIG_BLOCK, &maskInicio, NULL) == -1) {
        perror("fatal error in sigprocmask");
        exit(1);
    }
}