/*
 * kernel.c
 *
 *  Created on: 16/4/2016
 *      Author: utnso
 */

#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdbool.h>
#include <commons/string.h>
#include <commons/log.h>
#include <commons/config.h>
#include <commons/collections/dictionary.h>
#include <commons/collections/queue.h>
#include <commons/collections/list.h>
#include "../otros/handshake.h"
#include "../otros/header.h"
#include "../otros/sockets/cliente-servidor.h"
#include "../otros/log.h"
#include "../otros/commonTypes.h"

// Globales de servidor
int socketConsola, socketCPU, mayorDescriptor;
int activadoCPU, activadoConsola; //No hace falta iniciarlizarlas. Lo hacer la funcion permitir reutilizacion ahora.
struct sockaddr_in direccionConsola, direccionCPU;
unsigned int tamanioDireccionConsola, tamanioDireccionCPU;

// Globales de cliente
struct sockaddr_in direccionParaUMC;
int cliente; //se usa para ser cliente de UMC

// Hilos
pthread_t UMC; //Una instancia que finaliza luego de establecer conexion. Hilo para UMC. Asi si UMC tarda, Nucleo puede seguir manejando CPUs y consolas sin bloquearse.
pthread_t crearProcesos; // 1..n instancias. Este hilo crear procesos nuevos para evitar un bloqueo del planificador. Sin este hilo, el principal llama al hilo UMC para pedir paginas y debe bloquearse hasta tener la respuesta!

// Semaforos
pthread_mutex_t lockProccessList;

// ***** INICIO DEBUG ***** //
// setear esto a true desactiva el thread que se conecta con UMC.
// Es util para debugear sin tener una consola extra con UMC abierto.
#define DEBUG_IGNORE_UMC true
// ***** FIN DEBUG ***** //

// Para que rompan las listas y vectores
#define SIN_ASIGNAR -1


// Posibles estados de un proceso
typedef enum {
	NEW, READY, EXEC, BLOCK, EXIT
} t_proceso_estado;

typedef enum {
	FIFO, RR
} t_planificacion;
t_planificacion algoritmo;

typedef struct {
	int consola; // Indice de socketCliente
	int cpu; // Indice de socketCliente, legible solo cuando estado sea EXEC
	t_proceso_estado estado;
	struct t_PCB PCB;
} t_proceso;

t_queue* colaListos;
t_queue* colaSalida;
t_queue* colaCPU; //Mejor tener una cola que tener que crear un struct t_cpu que diga la disponibilidad
t_list* listaProcesos;
// Falta la cola de bloqueados para cada IO

/* INICIO PARA PLANIFICACION */
bool pedirPaginas(int PID, char* codigo){
	int hayMemDisponible;
	char respuesta;
	if(DEBUG_IGNORE_UMC){ // Para DEBUG
		log_warning(activeLogger,"DEBUG_IGNORE_UMC está en true! Se supone que no hay paginas");
		hayMemDisponible = false;
	}
	else{    // Para curso normal del programa
			send_w(cliente, headerToMSG(HeaderScript), 1);
			send_w(cliente, intToChar(strlen(codigo)), 1); //fixme: un char admite de 0 a 255. SI el tamaño supera eso se rompe!
			send_w(cliente, codigo, strlen(codigo));
			read(cliente, &respuesta, 1);
			hayMemDisponible = (int)respuesta;
			if(hayMemDisponible!=0 && hayMemDisponible!=1){
				log_warning(activeLogger,"Umc debería enviar un booleano (0 o 1) y envió %d", hayMemDisponible);
			}
			free(codigo);
			free(respuesta);
			log_debug(bgLogger,"Hay memoria disponible para el proceso %d.",PID);
	}
	return (bool)hayMemDisponible;
}

char* getScript(int consola){
	char scriptSize;
	char* script;
	int size;
	//char* scriptSize = recv_waitall_ws(consola,1);
	read(socketCliente[cliente], &scriptSize, 1);
	size = charToInt(&scriptSize);
	log_debug(bgLogger,"Consola envió un archivo de tamaño: %d",size);
	//free(scriptSize);
	printf("Size:%d\n",size);
	script = malloc(sizeof(char)*size);
	read(socketCliente[cliente],script,size);
	log_info(activeLogger,"Script:\n%s\n",script);
	return script; //recv_waitall_ws(consola,size);
}

void rechazarProceso(int PID){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_remove(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	if (proceso->estado != NEW)
		log_warning(activeLogger, "Se esta rechazando el proceso %d ya aceptado!",PID);
	send(socketCliente[proceso->consola], intToChar(HeaderConsolaFinalizarRechazado), 1, 0); // Le decimos adios a la consola
	quitarCliente(proceso->consola); // Esto no es necesario, ya que si la consola funciona bien se desconectaria, pero quien sabe...
	// todo: avisarUmcQueLibereRecursos(proceso->PCB) // e vo' umc liberá los datos
	free(proceso); // Destruir Proceso y PCB
}

int crearProceso(int consola) {
	t_proceso* proceso = malloc(sizeof(t_proceso));
	pthread_mutex_lock(&lockProccessList);
	proceso->PCB.PID = list_add(listaProcesos, proceso);
	pthread_mutex_unlock(&lockProccessList);
	proceso->PCB.PC = SIN_ASIGNAR;
	proceso->PCB.SP = SIN_ASIGNAR;
	proceso->estado = NEW;
	proceso->consola = consola;
	proceso->cpu = SIN_ASIGNAR;
	char* codigo = getScript(consola); // TODO: cuando haya hilos semaforear esto
	// Si la UMC me rechaza la solicitud de paginas, rechazo el proceso
	/*if(!pedirPaginas(proceso->PCB.PID, codigo)) {
		rechazarProceso(proceso->PCB.PID);
		log_info(activeLogger, "UMC no da paginas para el proceso %d!", proceso->PCB.PID);
		log_info(activeLogger, "Se rechazo el proceso %d.",proceso->PCB.PID);
	}*/
	free(codigo);
	return proceso->PCB.PID;
}

void cargarProceso(int consola){
	// Crea un hilo que crea el proceso y se banca esperar a que umc le de paginas. Mientras tanto, el planificador sigue andando.
	pthread_create(&crearProcesos, NULL, (void*)crearProceso, consola);
	//sleep(1000000);
}

void ejecutarProceso(int PID, int cpu){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_get(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	if (proceso->estado != READY)
		log_warning(activeLogger, "Ejecucion del proceso %d sin estar listo!",PID);
	proceso->estado = EXEC;
	proceso->cpu = cpu;
	// todo: mandarProcesoCpu(cpu, proceso->PCB);
};

void finalizarProceso(int PID){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_get(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	queue_push(colaCPU,(int*)proceso->cpu); // Disponemos de nuevo de la CPU
	proceso->cpu = SIN_ASIGNAR;
	proceso->estado= EXIT;
	queue_push(colaSalida,PID);
}

void destruirProceso(int PID){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_remove(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	if (proceso->estado != EXIT)
		log_warning(activeLogger, "Se esta destruyendo el proceso %d que no libero sus recursos!",PID);
	send(socketCliente[proceso->consola], intToChar(HeaderConsolaFinalizarNormalmente), 1, 0); // Le decimos adios a la consola
	quitarCliente(proceso->consola); // Esto no es necesario, ya que si la consola funciona bien se desconectaria, pero quien sabe...
	// todo: avisarUmcQueLibereRecursos(proceso->PCB) // e vo' umc liberá los datos
	free(proceso); // Destruir Proceso y PCB
}

void planificarProcesos(){
	//TODO RR, FIFO por ahora
	switch (algoritmo){
		// Procesos especificos
	case FIFO: break;
	case RR: break;
	}

	// Procesos Comunes a ambos
	if (!queue_is_empty(colaListos) && !queue_is_empty(colaCPU))
		ejecutarProceso(queue_pop(colaListos),queue_pop(colaCPU));

	if (!queue_is_empty(colaSalida))
		destruirProceso(queue_pop(colaSalida));
}

void bloquearProceso(int PID, int IO){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_get(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	if (proceso->estado != EXEC)
		log_warning(activeLogger, "El proceso %d se bloqueo pese a que no estaba ejecutando!",PID);
	proceso->estado = BLOCK;
	queue_push(colaCPU,proceso->cpu); // Disponemos de la CPU
	proceso->cpu = SIN_ASIGNAR;
	// todo: Añadir a la cola de ese IO
}

void desbloquearProceso(int PID){
	pthread_mutex_lock(&lockProccessList);
	t_proceso* proceso = list_get(listaProcesos,PID);
	pthread_mutex_unlock(&lockProccessList);
	if (proceso->estado != BLOCK)
		log_warning(activeLogger, "Desbloqueando el proceso %d sin estar bloqueado!",PID);
	proceso->estado = READY;
	queue_push(colaListos,PID);
}
/* FIN PARA PLANIFICACION */



// FIXME: error al compilar: expected ‘struct t_config *’ but argument is of type ‘struct t_config *’
// Si nadie lo sabe arreglar, podemos preguntarle a los ayudantes xD es muuuuy raro esto.

 typedef struct customConfig {
 int puertoConsola;
 int puertoCPU;

 int quantum; //TODO que sea modificable en tiempo de ejecucion si el archivo cambia
 int queantum_sleep; //TODO que sea modificable en tiempo de ejecucion si el archivo cambia
 char** sem_ids;
 int* semInit;
 char** io_ids;
 int* ioSleep;
 char** sharedVars;
 // Agrego cosas que no esta en la consigna pero necesitamos
 int puertoUMC;
 char* ipUMC;
 } customConfig_t;

customConfig_t config;
t_config* configNucleo;

void cargarCFG() {
	t_config* configNucleo;
	configNucleo = config_create("nucleo.cfg");
	config.puertoConsola = config_get_int_value(configNucleo, "PUERTO_PROG");
	config.puertoCPU = config_get_int_value(configNucleo, "PUERTO_CPU");
	config.quantum = config_get_int_value(configNucleo, "QUANTUM");
	config.queantum_sleep = config_get_int_value(configNucleo, "QUANTUM_SLEEP");
	config.sem_ids = config_get_array_value(configNucleo, "SEM_IDS");
	//retorna chars, no int, pero como internamente son lo mismo, entender un puntero como a char* o a int* es indistinto
	config.semInit = config_get_array_value(configNucleo, "SEM_INIT");
	config.io_ids = config_get_array_value(configNucleo, "IO_IDS");
	//retorna chars, no int, pero como internamente son lo mismo, entender un puntero como a char* o a int* es indistinto
	config.ioSleep = config_get_array_value(configNucleo, "IO_SLEEP");
	config.sharedVars = config_get_array_value(configNucleo, "SHARED_VARS");
	config.ipUMC = config_get_string_value(configNucleo, "IP_UMC");
	config.puertoUMC = config_get_int_value(configNucleo, "PUERTO_UMC");
}



int getConsolaAsociada(int cliente){
	int PID = charToInt(recv_waitall_ws(cliente, sizeof(int)));
	t_proceso* proceso = list_get(listaProcesos,PID);
	return proceso->consola;
}

void imprimirVariable(int cliente) {
	int consola = getConsolaAsociada(cliente);
	char* msgValue = recv_waitall_ws(cliente, sizeof(ansisop_var_t));
	char* name = recv_waitall_ws(cliente, sizeof(char));
	send_w(consola, headerToMSG(HeaderImprimirVariableConsola), 1);
	send_w(consola, msgValue, sizeof(ansisop_var_t));
	send_w(consola, name, sizeof(char));
	free(msgValue);
	free(name);
}

void imprimirTexto(int cliente) {
	int consola = getConsolaAsociada(cliente);
	char* msgSize = recv_waitall_ws(cliente, sizeof(int));
	int size = charToInt(msgSize);
	char* texto = recv_waitall_ws(cliente, size);
	send_w(consola, headerToMSG(HeaderImprimirTextoConsola), 1);
	send_w(consola, intToChar(size), 1);
	send_w(consola, texto, size);
	free(msgSize);
	free(texto);
}

void procesarHeader(int cliente, char *header) {
	// Segun el protocolo procesamos el header del mensaje recibido
	char* payload;
	int payload_size;
	log_debug(bgLogger, "Llego un mensaje con header %d", charToInt(header));

	switch (charToInt(header)) {

	case HeaderError:
		log_error(activeLogger, "Header de Error");
		quitarCliente(cliente);
		break;

	case HeaderHandshake:
		log_debug(bgLogger, "Llego un handshake");
		payload_size = 1;
		payload = malloc(payload_size);
		read(socketCliente[cliente], payload, payload_size);
		log_debug(bgLogger, "Llego un mensaje con payload %d",
				charToInt(payload));
		if ((charToInt(payload) == SOYCONSOLA)
				|| (charToInt(payload) == SOYCPU)) {
			log_debug(bgLogger,
					"Es un cliente apropiado! Respondiendo handshake");
			send(socketCliente[cliente], intToChar(SOYNUCLEO), 1, 0);
		} else {
			log_error(activeLogger,
					"No es un cliente apropiado! rechazada la conexion");
			log_warning(activeLogger, "Se quitará al cliente %d.", cliente);
			quitarCliente(cliente);
		}
		free(payload);
		break;

	case HeaderImprimirVariableNucleo:
		imprimirVariable(cliente);
		break;

	case HeaderImprimirTextoNucleo:
		imprimirTexto(cliente);
		break;

	case HeaderScript:
		//cargarProceso(cliente); // la posta con hilos! cuando se sincronize bien borrar la siguiente linea
		crearProceso(cliente);
		break;

	default:
		log_error(activeLogger, "Llego cualquier cosa.");
		log_error(activeLogger,
				"Llego el header numero %d y no hay una acción definida para él.",
				charToInt(header));
		log_warning(activeLogger, "Se quitará al cliente %d.", cliente);
		quitarCliente(cliente);
		break;
	}
}

struct timeval newEspera() {
	struct timeval espera;
	espera.tv_sec = 2; 				//Segundos
	espera.tv_usec = 500000; 		//Microsegundos
	return espera;
}

/* INICIO PARA UMC */
int getHandshake() {
	char* handshake = recv_nowait_ws(cliente, 1);
	return charToInt(handshake);
}

void handshakear() {
	char *hand = string_from_format("%c%c", HeaderHandshake, SOYNUCLEO);
	send_w(cliente, hand, 2);

	log_debug(bgLogger, "UMC handshakeo.");
	if (getHandshake() != SOYUMC) {
		perror("Se esperaba conectarse a la UMC.");
	} else
		log_debug(bgLogger, "Núcleo recibió handshake de UMC.");
}

void conectarALaUMC() {
	direccionParaUMC = crearDireccionParaCliente(config.puertoUMC);
	cliente = socket_w();
	connect_w(cliente, &direccionParaUMC);
}

void realizarConexionConUMC() {
	conectarALaUMC();
	log_info(activeLogger, "Conexion al nucleo correcta :).");
	handshakear();
	log_info(activeLogger, "Handshake con UMC finalizado exitosamente.");
}

void manejarUMC() {
	log_debug(bgLogger, "Hilo para solicitudes de UMC inicializado.");
	realizarConexionConUMC();
}

void iniciarHiloUMC(){
	if(!DEBUG_IGNORE_UMC){
			// Me conecto a la umc y hago el handshake
			pthread_create(&UMC, NULL, (void*)manejarUMC, NULL);
		}
		else{
			warnDebug();
		}
}

void warnDebug() {
	log_warning(activeLogger, "--- CORRIENDO EN MODO DEBUG!!! ---");
	log_info(activeLogger, "NO SE ESTABLECE CONEXION CON UMC EN ESTE MODO!");
	log_info(activeLogger, "Para correr nucleo en modo normal, settear en false el define DEBUG_IGNORE_UMC.");
	log_warning(activeLogger, "--- CORRIENDO EN MODO DEBUG!!! ---");
}
/* FIN PARA UMC */


void finalizar() {
	destruirLogs();
	list_destroy(listaProcesos);
	queue_destroy(colaCPU);
	queue_destroy(colaListos);
	queue_destroy(colaSalida);
}

int main(void) {
	system("clear");
	int i;
	struct timeval espera = newEspera(); // Periodo maximo de espera del select
	char header[1];
	listaProcesos = list_create();
	colaCPU = queue_create();
	colaListos = queue_create();
	colaSalida = queue_create();
	pthread_mutex_init(&lockProccessList, NULL);

	config.puertoConsola=8080;
	config.puertoCPU=8088;
	cargarCFG();
	crearLogs("Nucleo", "Nucleo");

	configurarServidorExtendido(&socketConsola, &direccionConsola,
			config.puertoConsola, &tamanioDireccionConsola, &activadoConsola);
	configurarServidorExtendido(&socketCPU, &direccionCPU, config.puertoCPU,
			&tamanioDireccionCPU, &activadoCPU);

	inicializarClientes();
	log_info(activeLogger, "Esperando conexiones ...");

	iniciarHiloUMC();

	while (1) {
		FD_ZERO(&socketsParaLectura);
		FD_SET(socketConsola, &socketsParaLectura);
		FD_SET(socketCPU, &socketsParaLectura);

		mayorDescriptor = incorporarClientes();
		if ((socketConsola>mayorDescriptor) || (socketCPU>mayorDescriptor)){
			if (socketConsola > socketCPU)
				mayorDescriptor = socketConsola;
			else
				mayorDescriptor = socketCPU;
		}

		select(mayorDescriptor + 1, &socketsParaLectura, NULL, NULL, &espera);

		if (tieneLectura(socketConsola))
			procesarNuevasConexionesExtendido(&socketConsola);

		if (tieneLectura(socketCPU))
			procesarNuevasConexionesExtendido(&socketCPU);

		for (i = 0; i < getMaxClients(); i++) {
			if (tieneLectura(socketCliente[i])) {
				if (read(socketCliente[i], header, 1) == 0) {
					log_error(activeLogger,
							"Se rompio la conexion. Read leyó 0 bytes");
					quitarCliente(i);
				} else
					procesarHeader(i, header);
			}
		}

		planificarProcesos();
	}

	finalizar();
	return EXIT_SUCCESS;
}
