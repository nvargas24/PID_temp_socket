/****************************** HEATERS **************************************/

#include <OneWire.h>
#include <DallasTemperature.h> 
#include "WiFi.h"

/****************************** MACROS ***************************************/

#define HEATER_PWM 32
#define SENSOR 27
#define DEFAULT_SETPOINT 25
#define SETPOINT_PWM 25

typedef enum{
  CAMBIAR_SETPOINT = 23,
  SUBIR_TEMP = 25,
  BAJAR_TEMP = 26,
  FINALIZAR_COMUNICACION_TCP = 27,
  NEW_DATA = 0
}COMANDO_RECV;


/* Declaracion de varaibles globales */

/****** Wifi TCP ******/
/* Conexion a router 
const char* ssid = "Telecentro-08a4";
const char* pass = "FZNXGZMGN3YT";

const IPAddress serverIP(192,168,0,19); //IP Server 
uint16_t serverPort = 9999;         //PORT Server


/* Conexion por celular */
const char* ssid = "prueba";
const char* pass = "12345678";

const IPAddress serverIP(192,168,43,240); //IP Server 
uint16_t serverPort = 9999;         //PORT Server

/* Declarar un objeto cliente para conectarse al servidor */
WiFiClient client; 

/* Variables para buffer */
char msg[22];  // Mensaje a enviar
char buf[50];  // Guarda mensaje recibido

/* Variables para controlador PID */
float kp = 25;
float ki = 1.25;
float kd = 180.5;

//unsigned long currentTime = 0 ;
//unsigned long previousTime = 0;
//int elapsedTime = 0;

float error = 0;  
float lastError = 0;   
float cumError = 0;
float rateError = 0;

float output; 
int setPoint = DEFAULT_SETPOINT;
int setPoint_duty;

/* GPIO sensor temperatura */
const int oneWireBus = SENSOR;   

/* Flags */
bool crossZero = false;
bool stateTCP = false;

/* Contador para cantidad de mensajes a enviar */ 
int cont =0;

/* PWM */
const int channel = 0;
const int channel2 = 1;
const int frequency = 500;
const int resolution =8;  // puede controlar el brillo del LED usando un valor de 0 a 255.

/*************************** FUNCIONES INTERNAS ***********************************/ 

/* Setup a oneWire instance to communicate with any OneWire devices */
OneWire oneWire(oneWireBus);

/* Pass our oneWire reference to Dallas Temperature sensor */
DallasTemperature sensors(&oneWire);

/* Manejo de PID */
float computePID(float outPID);
float mapf(float val, float inimin, float inimax, float outmin, float outmax);

/* Comunicacion Wifi port TCP */
void initWifi(void);
void connectTCP(void);
void readTCP(void);
void sendTCP(float);

/* Graficos por UART */
void graficoArduino(float dataTem,int dutyCycle);

/* ==================[external functions definition]========================== */

void setup()
{ 
  Serial.begin(115200); /* Start the Serial Monitor */
  initWifi();  /* Start wifi*/
  sensors.begin();  /* Start the DS18B20 sensor */

  ledcSetup(channel, frequency, resolution);
  ledcSetup(channel2, frequency, resolution);
  ledcAttachPin(HEATER_PWM, channel); 
  ledcAttachPin(SETPOINT_PWM, channel2); 
}

void loop(){ 
  float dataTemp;
  float dutyCycle;
  
  if(stateTCP == false){ 
    connectTCP();
  }
  else{
  sensors.requestTemperatures(); 
  dataTemp = sensors.getTempCByIndex(0);

  sendTCP(dataTemp);
  readTCP();
  dataAnalisis();

  delay(1);

  output = computePID(dataTemp);
 
  dutyCycle =map(output, -500, 500, 0, 255);
  setPoint_duty=map(setPoint, 0, 100, 0, 255);
  
  ledcWrite(channel, dutyCycle);
  ledcWrite(channel2, setPoint_duty);
  delay(100);
  
  graficoArduino(dataTemp, dutyCycle);
  }
}

/*==================[internal functions definition]===============================*/

float computePID(float outPID)
{
  error = setPoint - outPID; // determine error
  cumError = error*ki+cumError; // compute integral
  rateError = error - lastError; // compute derivative

  if(cumError >= 500) cumError = 500;
  else if(cumError <= -500) cumError = -500;

  outPID = kp*error + cumError + kd*rateError; //PID output
     
  lastError = error; //remember current error
  
  return outPID; //have function return the PID output
}



float mapf(float val, float inimin, float inimax, float outmin, float outmax)
{
  val=((val-inimin)*((outmax-outmin+1)/(inimax-inimin+1)))+outmin;

  if (val >= outmax) 
    val = outmax;
  else if(val <= outmin) 
    val = outmin;

  return val; 
}


void initWifi(void)
{
  // Wifi TCP
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Desactiva la suspensión de wifi en modo STA para mejorar la velocidad de respuesta
  WiFi.begin(ssid, pass);
  
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Se conecto a red Wifi\n\n");
}

void connectTCP(void)
{
  if (client.connect(serverIP, serverPort)){ // Intenta acceder a la dirección de destino
    Serial.println("Accede a servidor");
    client.print("Hola soy el ESP32\n");// Enviar datos al servidor
    stateTCP = true;
  }
  else{
    Serial.println("Acceso fallido");
    client.stop(); // Cerrar el cliente
    stateTCP =false;
  }
  delay(100);
}

void readTCP(){
  if (client.available()) {
    String line = client.readStringUntil('\n'); // Leer datos a nueva línea
    Serial.print("Server: ");
    Serial.println(line);
    line.toCharArray(buf, 50); // Paso String a char

    delay(10);
  }
}

void sendTCP(float dataTemp)
{
  snprintf(msg, 22, "Temp. Celcius: %.2f\n", dataTemp);
  client.print(msg);
}

COMANDO_RECV dataComparation()
{
  if(!strcmp(buf, "finESP32")){
    return FINALIZAR_COMUNICACION_TCP;
  }
  else if(!strcmp(buf, "+")){
    return SUBIR_TEMP;
  }
  else if(!strcmp(buf, "-")){
    return BAJAR_TEMP;
  } 
  else if(atoi(buf)){
    return CAMBIAR_SETPOINT;
  }

}

void dataAnalisis(void)
{
  COMANDO_RECV tipo;

  tipo = dataComparation(); 

  switch(tipo)
  {
    case FINALIZAR_COMUNICACION_TCP:
      //Serial.println("Termino comunicacion");
      client.stop();
    break;

    case SUBIR_TEMP:
      //Serial.println("Subir temperatura");
      setPoint += 1;
      
      if(setPoint >= 100)
        setPoint = 100;
      else if(setPoint <= 0)
        setPoint = 0;
  
    break;
      
    case BAJAR_TEMP:
      //Serial.println("Bajar temperatura");
      setPoint -= 1;
      
      if(setPoint >= 100)
        setPoint = 100;
      else if(setPoint <= 0)
        setPoint = 0;
      
    break;

    case CAMBIAR_SETPOINT:
      //Serial.println("Nuevo SetPoint");
      setPoint = atoi(buf);

      if(setPoint >= 100){
        setPoint = 100;
      }  
      else if(setPoint <= 0){
        setPoint = 0;
      }
 
    break;
  }
}


void graficoArduino(float dataTemp, int dutyCycle)
{  
  Serial.println("Setpoint,Sensor,outPID");
  Serial.print(setPoint); 
  Serial.print(",");
  Serial.print(dataTemp);
  Serial.print(",");
  Serial.println(dutyCycle);

}

/*==================[end of file]============================================*/
