/********************************************************************
 *
 * Proyecto: Control PID por computadora 
 * Alumno: VARGAS, Nahuel Enrique
 * Legajo: 15-23117 
 * Catedra: Tecnicas Digitales 3
 *
 ************************* Avance de proyecto ***********************
 *
 * Comunicacion TCP servidor concurrente
 * Programa funcionando como echo
 * Identificacion de clientes con IP estatica
 * Implementacion de Semaforos y Shared Memory 
 * Se almacenan datos de temperatura y comandos
 * Interaccion entre servidor, clientes y esp32
      * comandos, lecturas de temperatura, respuesta por servidir
 *
 ******************************* Falta ****************************** 
 *
 * Se almacenan solicitudes pendientes, hasta que se libere semaforo
 * Hacer 2 semaforos, uno para cada Shared Memory
 *
 ******************************* Problema ****************************** 
 *
 * Carga de varios valores a Sahred Memory, se carga un valor y se cuelga
   ese cliente, para desbloquear, solo finalizando programa
 * Puede ser debido a los semaforos, comentarlos y probar como ejemplo
   de Shared Memory
 *
 **************************** IMPORTANTE ****************************
 * 
 * SocketTest manda los string terminado con: "\r\n"
 *
 ********************************************************************/

/************************************** HEATERS **************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ipc.h>   
#include <sys/shm.h>
#include <sys/sem.h>
  
/*************************************** MACROS **************************************************/

#define MAXLINE 4096 /* Max text line length */
#define SERV_PORT 9999 /* PORT */
#define LISTENQ 8 /* Maximum number of client connections */

#define KEY_SHMEM (key_t)447885 /* llave de la memoria compartida */
#define KEY_SHMEM2 (key_t)443685 /* llave de la memoria compartida */
#define KEY_SHMEM3 (key_t)448765 /* llave de la memoria compartida */

#define KEY_LLENO (key_t)788544 /* llave semaforo del almacen lleno */
#define KEY_VACIO (key_t)885447 /* llave semaforo del almacen vacio */
#define KEY_MUTEX_SHMEM (key_t)478854 /* llave semaforo de entrada/salida seccion critica */
#define KEY_MUTEX_SOLIC (key_t)473492 /* llave semaforo de entrada/salida seccion critica */
#define N 20 /* numero maximo elementos del almacen */
#define LLENO 5 /* numero casillas llenas del almacen */

#define SEGSIZE (sizeof(char)) /* longitud de la memoria compartida */
#define SHMEM_DATA 0 /* ID Shared Memory Data */
#define SHMEM_SOLICITUDES 1 /* ID Shared Memory Solicitudes */
#define SHMEM_SOLICITUD_A_ESP32 2 /* ID Shared Memory Solicitudes a ESP32 */

typedef enum{
    NEW_SETPOINT = 11,
    CAMBIAR_SETPOINT = 12,
    LEER_TEMPERATURA = 13,
    FINALIZAR_LECTURA_TEMPERATURA = 14,
    NO_RECIBIO_PALABRA_CLAVE = 15,
    FINALIZAR_ESP32 = 16,
    SUBIR_TEMPERATURA = 17,
    BAJAR_TEMPERATURA = 18
} TIPO_SOLICITUD;

/******************************** ESTRUCTURAS y UNIONIES ****************************************/

/* Union necesaria para la utilizacion de semaforos */
union semun {
    int val;
    struct semid_ds *buf;
    ushort *array;
    struct seminfo *__buf;
};

/******************************** VARIABLES GLOBALES ********************************************/

/* Variables para Shared Memory */
struct shmid_ds *shmbuf;
int shData, shSolicitudes, shSolicitud2ESP32; /* identificador de Shared Memory */
char *dataTemp, *dataPedido, *data2ESP32; /* punteros a la Shared Memory */ 

/* Variables para Semaforo */
int semSolicitudes, semShMem, semVacio, semLleno; /* identificador semaforos */

/* Flags */
int cantHijos = 0;
bool stateTCP = true;


/*************************** DECLARACIONES DE FUNCIONES INTERNAS ********************************/

/* Inicializa socket TCP */
void initSocketTCP(int * listenfd, struct sockaddr_in * my_addr);

/* Handler que atiende a muerte de hijos (desconexion de cliente) */
void handlerChild(int val);

/* Atiende solicitudes realizadas por clientes */
int solicitudesClientes(char*bufPedido, int connfd, char *ipCliente, int bytesRecv);

/* Tomar semaforo */
void takeSem(int sem, int n);

/* Liberar semaforo */
void releaseSem(int sem, int n);

/* Asigna el valor x, al semaforo n de sem */
int asig_val_sem(int sem, int n, int x );

/* Crea semaforo */
int createSem(key_t k, int n);

/* Inicializo Semaforo */
void initSem(void);

/* Accedo o creo Shared Memory */
void initShMem(int idShMem);

/*********************************** PROGRAMA PRINCIPAL ****************************************/

int main ()
{
  int listenfd = 0, connfd = 0, bytesRecv = 0;
  socklen_t clilen;

  char buf[MAXLINE];

  struct sockaddr_in my_addr; /* IP y el numero de puerto local */
  struct sockaddr_in their_addr; /* IP y numero de puerto del cliente */ 

  pid_t pidHijo; /* Identifico PID del proceso hijo que atiende a cliente */
 
  int state_solicitud; 

  /* Senial para saber cuando muere un proceso hijo*/
  signal(SIGCHLD, handlerChild);

  /* Inicializo Semaforo */
  //initSem();   // fijarse que bloquea programa

  /* Inicializo Shared Memory */
  initShMem(SHMEM_SOLICITUDES);
  initShMem(SHMEM_DATA);
  initShMem(SHMEM_SOLICITUD_A_ESP32);
  
  /* Create a socket for the soclet */
  if ((listenfd = socket (AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("Problem in creating the socket");
    exit(2);
  }

  /* Inicializa Socket TCP */
  initSocketTCP(&listenfd ,&my_addr);

  printf("Server funcionando ...esperando conexiones\n");
  printf("PORT: %d\n\n", (int) ntohs(my_addr.sin_port));
 
  /* Loop mientras este conexion TCP*/
  while(stateTCP){
 
    clilen = sizeof(their_addr);

    /* accept a connection */
    if ((connfd =  accept(listenfd, (struct sockaddr *)&their_addr, &clilen))== -1){
      perror("error accept");
      exit(1);
    }

    printf("Se acepto una conexion: %d\n", connfd);
    printf("Conexion desde: %s\n", inet_ntoa(their_addr.sin_addr ));
    send(connfd, "Bienvenido a server TD3-PID\r\n", strlen("Bienvenido a server TD3-PID\r\n"), 0);

    /* Creo un hijo para atender a un cliente nuevo */
    if (!fork()) { 
      
      pidHijo = getpid();
      printf("\nSe crea child para identificar al cliente\n");
      printf("Child: %d  Padre: %d\n\n", pidHijo, getppid());

      /* Close listening socket */
      close (listenfd);

      /* Se muestra y compara string recibido por cliente, tambien se identidfica por IP*/
      while ( (bytesRecv = recv(connfd, buf, MAXLINE,0)) > 0)  {
        /* Mensaje de recepcion */
        printf("\nSe recibieron: %d bytes\n", bytesRecv);
        printf("Cliente: %s\n", inet_ntoa(their_addr.sin_addr ));
        printf("Mensaje: %s\n", buf);
      
//////////////////////////////////////////////////////////////////////////////////
        
        /* Se cargan datos de ESP32 a Shared Memory */
        if(!strcmp("192.168.0.17", inet_ntoa(their_addr.sin_addr ))){       //cargo ip estatica de ESP32
        //if(!strcmp("192.168.43.85", inet_ntoa(their_addr.sin_addr ))){       //cargo ip estatica de ESP32
          printf("Se recibe dato de ESP32\n");
          /* Acceso a Shared Memory Temperatura*/
          initShMem(SHMEM_DATA);  // Se almacena en shData
          strcpy(dataTemp, buf);          
          printf("Se guardo en Shared Memory data: %s\n\n", dataTemp);
          /* Control de acceso por semaforo */
///////////////////////////////////////////////////////////////////////////////////////////////////////
          
          /* Accion a realizar por pedido a ESP32 */
          printf("Algun pedido a ESP32?\n");
          initShMem(SHMEM_SOLICITUD_A_ESP32);
          if(data2ESP32[0] != 0 ){
              printf("Si, leyendo... \n");
              printf("dataESP32: %s\n", data2ESP32);
              send(connfd, data2ESP32, strlen(data2ESP32), 0);
              printf("Se envio comando a ESP32\n");
              data2ESP32[0] = 0; // para todo el tiempo verificar que hay un dato 
          }
          else
            printf("No hay nuevo pedido a ESP32\n");

/////////////////////////////////////////////////////////////////////////////////////////////////////////
        }
        /* Se atienden solicitudes de clientes */
        else{

          printf("Se recibe un pedido de cliente\n");
          /* Acceso a Shared Memory Solicitudes*/
          initShMem(SHMEM_SOLICITUDES); // Se almacena en shSolicitudes
          strcpy(dataPedido, buf);          
          printf("Se guardo en Shared Memory solicitudes: %s\n\n", dataPedido);

        }

        /* Acceso a Shared Memory para solicitudes de pedidos*/
        printf("Se lee de Shared Memory solicitudes...\n");

        if(dataPedido[0] != 0){         
          /* Se atienden pedidos (string) de clientes y devuelve que cual fue la solicitud*/
          state_solicitud = solicitudesClientes(dataPedido, connfd, inet_ntoa(their_addr.sin_addr), bytesRecv);
          
          dataPedido[0] = 0; // para todo el tiempo verificar que hay un dato 
        }
        else{
          printf("No se cargo nuevo comando a la Shared Memory Solicitudes\n\n");
        }  
        
        bzero(buf, strlen(buf));

      }
  ////////////////////////////////////////////////////////////////////////////////////
    
      /* Acciones al no recibir mensajes de clientes */
      printf("No se reciben datos\n");
      sleep(1);
      printf("Se desconecto: %s\n", inet_ntoa(their_addr.sin_addr));
      printf("Termino hijo: %d  PID:%d\n", cantHijos+1, pidHijo);
      
      kill(pidHijo, SIGKILL);   /* Mato hijo que antiende cliente, esto finaliza su conexion */
      close(connfd);   
    } 
  
    cantHijos++; /* Contador de hijos creados para antender clientes */
  
  }// fin de while
  
  /* Espera para asegurar accion de Handler */
  while(cantHijos) 
    sleep(1);

  printf("Finalizo programa padre\n\n");
  close(connfd);
  exit(0);

  return 0;
}

void handlerChild(int val)
{
  wait(0); /* Espero la señal SIGCHLD de cualquier hijo */

  cantHijos--; /* Decremento cantidad de hijos */
  printf("Handler: Quedan %d hijos restantes\n\n", cantHijos);

  if(cantHijos == 0){ 
    stateTCP = 0;   
    exit(0);
  }
}

void initSocketTCP(int * sockfd, struct sockaddr_in * my_addr)
{
  if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
    perror("socket TCP");
    exit(1);
  }

  /* Estructura my_addr para luego poder llamar a la funcion bind() */
  my_addr->sin_family = AF_INET;
  my_addr->sin_port = htons(SERV_PORT); /* Debe convertirse a network byte order porque es enviado por la red */
  my_addr->sin_addr.s_addr = INADDR_ANY; /* Automaticamente usa la IP local */
  bzero(&(my_addr->sin_zero), 8);  /* Rellena con ceros el resto de la estructura */

  if ( bind(*sockfd, (struct sockaddr *) my_addr, sizeof(struct sockaddr)) == -1){
    perror("Fallo en bind TCP");
    exit(1);
  }

  if (listen(*sockfd, LISTENQ) == -1){
    perror("listen");
    exit(1);
  }
}

/* Comparacion de comandos mandados si es una palabra con una accion especifica o no */
TIPO_SOLICITUD clasificacionPedidos(char *pedido){

  if( (!(strcmp(pedido, "temperatura\r\n"))) || (!(strcmp(pedido, "temperatura\n"))) ){
    return LEER_TEMPERATURA;
  }
  else if( (!(strcmp(pedido, "cambiar setpoint\r\n"))) || (!(strcmp(pedido, "cambiar setpoint\n"))) ){
    return CAMBIAR_SETPOINT;
  }
  else if( (!(strcmp(pedido, "fin esp32\r\n"))) || (!(strcmp(pedido, "fin esp32\n"))) ){
    return FINALIZAR_ESP32;
  }
  else if( (!(strcmp(pedido, "+\r\n"))) || (!(strcmp(pedido, "+\n"))) ){
    return SUBIR_TEMPERATURA;
  }
  else if( (!(strcmp(pedido, "-\r\n"))) || (!(strcmp(pedido, "-\n"))) ){
    return BAJAR_TEMPERATURA;
  }
  else if(atoi(pedido))
    return NEW_SETPOINT;
  else{
    return NO_RECIBIO_PALABRA_CLAVE;
  }

}


int solicitudesClientes(char *bufPedido, int connfd, char *ipCliente, int bytesRecv)
{

  TIPO_SOLICITUD state_solicitud;
  char msj2cli[MAXLINE]="";
  int setPoint;
  //send(connfd, "Solicitud aceptada...\n", strlen("Solicitud aceptada...\n"), 0);
 

  state_solicitud = clasificacionPedidos(bufPedido);

  switch(state_solicitud){
    /* Solicitud de temperatura actual */
    case LEER_TEMPERATURA:  
      
      printf("\nSe atiende pedido...\nLectura de temperatura...\n");
      printf("Cliente: %s\n", ipCliente);
    
      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para leer temperatura*/
      printf("Se lee de Shared Memory data...\n");
    
      if(dataTemp[0] != 0){
          printf("\ndataTemp: %s\n", dataTemp);
          send(connfd, dataTemp, strlen(dataTemp), 0);
          dataTemp[0] = 0; // para todo el tiempo verificar que hay un dato
          printf("Servidor: Se envio temperatura actual a cliente\n");
      }
      else{
        printf("No se cargo nuevo temperatura a la Shared Memory\n");
        send(connfd, "No se cargo nueva temperatura a la Shared Memory\r\n", strlen("No se cargo nuevo temperatura a la Shared Memory\r\n"), 0);    
      } 

      break;

    /* Solicitud de cambio SetPoint */
    case CAMBIAR_SETPOINT:   
      printf("\nSe atiende pedido...\nCambio de SetPoint...");

      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para solicitudes a ESP32*/
      initShMem(SHMEM_SOLICITUD_A_ESP32); // Se almacena en shSolicitudes
      strcpy(data2ESP32, "cambiarSetPoint");          
      printf("Se guardo en Shared Memory solicittudes a ESP32: %s\n\n", data2ESP32);

      /* Control de acceso por semaforo */

      send(connfd, "Se cambia SetPoint\r\n", strlen("Se cambia SetPoint\r\n"), 0);

      break;
        /* Solicitud de cambio SetPoint */
    case NEW_SETPOINT:   
      printf("\nSe atiende pedido...\nNuevo Setpoint...\n");

      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para solicitudes a ESP32*/
      initShMem(SHMEM_SOLICITUD_A_ESP32); // Se almacena en shSolicitudes
      strcpy(data2ESP32, bufPedido);          
      printf("Se guardo en Shared Memory solicitudes a ESP32: %s\n\n", data2ESP32);

      setPoint = atoi(bufPedido);
      /* Control de acceso por semaforo */
      if(setPoint >= 100){
        setPoint = 100;
      }
        
      else if(setPoint <= 0){
        setPoint = 0;
      }
 
      sprintf(msj2cli,"Se cambia SetPoint a %d°C\r\n", setPoint);
      send(connfd, msj2cli, strlen(msj2cli), 0);
 
      break;


    /* Solicitud de finalizar conexion con ESP32 */
    case FINALIZAR_ESP32:      
      printf("\nSe atiende pedido...\nFinalizar conexion con ESP32");

      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para solicitudes a ESP32*/
      initShMem(SHMEM_SOLICITUD_A_ESP32); // Se almacena en shSolicitudes
      strcpy(data2ESP32, "finESP32");          
      printf("Se guardo en Shared Memory solicittudes a ESP32: %s\n\n", data2ESP32);

      /* Control de acceso por semaforo */
      
      send(connfd, "Se desconecta ESP32\r\n", strlen("Se desconecta a ESP32 de la red\r\n"), 0);    

      break;

    case SUBIR_TEMPERATURA:      
      printf("\nSe atiende pedido...\nSubir temperatura de calentador\n");

      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para solicitudes a ESP32*/
      initShMem(SHMEM_SOLICITUD_A_ESP32); // Se almacena en shSolicitudes
      strcpy(data2ESP32, "+");          
      printf("Se guardo en Shared Memory solicittudes a ESP32: %s\n\n", data2ESP32);

      /* Control de acceso por semaforo */
      
      send(connfd, "Se aumenta temperatura de calentador\r\n", strlen("Se aumenta temperatura de calentador\r\n"), 0);    

      break;

    case BAJAR_TEMPERATURA:      
      printf("\nSe atiende pedido...\nBajar temperatura de calentador\n");

      /* Accion que se desea realizar con esta palabra */
      /* Acceso a Shared Memory para solicitudes a ESP32*/
      initShMem(SHMEM_SOLICITUD_A_ESP32); // Se almacena en shSolicitudes
      strcpy(data2ESP32, "-");          
      printf("Se guardo en Shared Memory solicittudes a ESP32: %s\n\n", data2ESP32);

      /* Control de acceso por semaforo */
      
      send(connfd, "Se baja temperatura de calentador\r\n", strlen("Se baja temperatura de calentador\r\n"), 0);    

      break;

    /* Pedido no valido */
    case NO_RECIBIO_PALABRA_CLAVE:
      printf("\nNo se recibio palabra clave\n\n");

      /* Accion que se desea realizar con esta palabra */
      send(connfd, "Comando no valido\n", strlen("Comando no valido\\n"), 0);
      
      break;   

  }

  /* Se limpia buffer para evitar problemas */
  bzero(bufPedido, strlen(bufPedido)); 

  return state_solicitud;
}

/* Accedo o creo Shared Memory */
void initShMem(int idShMem)
{
  switch(idShMem ){
    case SHMEM_DATA:
      /* Se accede o crea Shared Memory */
      if((shData = shmget(KEY_SHMEM, SEGSIZE, IPC_CREAT|0666)) == -1){
        perror("Error en shData");
        exit(0);
      }
  
      /* Asigno puntero a Shared Memory */
      dataTemp = shmat(shData, (void *)0, 0); 
      if (dataTemp == (char *)(-1)){ 
        perror("shmat de dataTemp: "); 
        exit(1); 
      }
      break;

    case SHMEM_SOLICITUDES:
      /* Se accede o crea Shared Memory */
      if((shSolicitudes = shmget(KEY_SHMEM2, SEGSIZE, IPC_CREAT|0666)) == -1){
        perror("Error en shSolicitudes");
         exit(1); 
      }

      /* Asigno puntero a Shared Memory */
      dataPedido = shmat(shSolicitudes, (void *)0, 0); 
      if (dataPedido == (char *)(-1)){ 
        perror("shmat de dataPedido: "); 
        exit(1); 
      }

      break;
    
    case SHMEM_SOLICITUD_A_ESP32:
      /* Se accede o crea Shared Memory */
      if((shSolicitud2ESP32 = shmget(KEY_SHMEM3, SEGSIZE, IPC_CREAT|0666)) == -1){
        perror("Error en shSolicitud2ESP32");
        exit(1); 
      }

      /* Asigno puntero a Shared Memory */
      data2ESP32 = shmat(shSolicitud2ESP32, (void *)0, 0); 
      if (data2ESP32 == (char *)(-1)){ 
        perror("shmat de data2ESP32: "); 
        exit(1); 
      }

      break;

    default:
      printf("\nPedido no valido\n");
  }

}


/* Funciones para control semaforos*/
/* Inicializo semaforos */
void initSem(void)
{
  /* Se crea Semaforos */
  semSolicitudes = createSem(KEY_MUTEX_SOLIC, 1);
  semShMem = createSem(KEY_MUTEX_SHMEM, 1);
  semVacio = createSem(KEY_VACIO, 1);
  semLleno = createSem(KEY_LLENO, 1);

  /* Inicializo semaforos */
  asig_val_sem(semSolicitudes, 0, 1);
  asig_val_sem(semShMem, 0, 1);
  asig_val_sem(semVacio, 0, N-LLENO);
  asig_val_sem(semLleno, 0, LLENO);
}


/* Tomar semaforo*/
void takeSem(int sem, int n){
  struct sembuf sop;

  //sop.sem_num = n;
  sop.sem_op = -1;
  //sop.sem_flg = 0;
  semop(sem, &sop, 1);

}

/* Liberar semaforo */
void releaseSem(int sem, int n){
  struct sembuf sop;

  //sop.sem_num = n;
  sop.sem_op = 1;
  //sop.sem_flg = 0;
  semop(sem, &sop, 1);
}

/* asigna el valor x, al semaforo n de sem */
int asig_val_sem(int sem, int n, int x )
{
    union{
        int val;
        struct semid_ds *buf;
        ushort *array;
    } semctl_arg;

    semctl_arg.val = x;
    
    if (semctl(sem, n, SETVAL, semctl_arg) < 0)
        return(-1);
    else
        return(0);
}

int createSem(key_t k, int n)
{
    int semid, i=0;
    /* Si ya existe un conjunto de de semaforos lo destruye */
    if ((semid=semget(k, n, 0)) != -1)
        semctl(semid, 0, IPC_RMID); /* destruccion del semaforo */
     
    if ((semid=semget(k, n, IPC_CREAT | 0666)) != -1) {       
        for (i=0; i<n; i++){       
            takeSem(semid, i);
        }
    }
    else
        return(-1);
    
    return semid;
}