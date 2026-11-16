#include <Arduino.h>
#include <stdint.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"

// Conexiones del teclado matricial 4x4
#define R1 12
#define R2 13
#define R3 14
#define R4 15

#define C1 16
#define C2 17
#define C3 18
#define C4 19

// Pines asignados a los segmentos a, b, c, d, e, f, g
const uint8_t pinesDisplay[7] = {25, 26, 27, 32, 23, 22, 33}; 

// Pines para el puente H (LEDs indicadores de giro)
#define MOTOR1_IN1  0
#define MOTOR1_IN2  2
#define MOTOR2_IN1  4
#define MOTOR2_IN2  5

// Estados posibles de ejecución del robot
enum { 
  QUIETO, 
  ADELANTE, 
  ATRAS, 
  DERECHA, 
  IZQUIERDA, 
  LEVANTAPLUMA, 
  BAJAPLUMA 
};

volatile int comando = QUIETO;   // Acción actual en ejecución
volatile int valor = 0;          // Distancia (cm) o ángulo (grados)
volatile bool moviendo = false;  // Bandera modificada por la interrupción del Timer

// Puntero al Timer de hardware del ESP32
hw_timer_t *timerMovimiento = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Tabla de verdad para display de 7 segmentos (cátodo común, orden gfedcba)
const uint8_t segments[16] = {
  0b0111111, // 0
  0b0000110, // 1
  0b1011011, // 2
  0b1001111, // 3
  0b1100110, // 4
  0b1101101, // 5
  0b1111101, // 6
  0b0000111, // 7
  0b1111111, // 8
  0b1101111, // 9
  0b1110111, // A
  0b1111100, // b
  0b0111001, // C
  0b1011110, // d
  0b1000000, // * (guion central para parada)
  0b1111011  // #
};

// Máscara de bits para manipular las 4 filas simultáneamente
const uint32_t MASCARA_FILAS = 
  (1UL << R1) | (1UL << R2) | (1UL << R3) | (1UL << R4);

const uint8_t pinesFilas[4] = {R1, R2, R3, R4};

// Distribución física de las teclas
const char matTeclas[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Apaga todos los pines de control del puente H mediante registro directo (Banco 0)
inline void detenerMotoresRegistro() {
  uint32_t maskMotores = (1UL << MOTOR1_IN1) | (1UL << MOTOR1_IN2) | 
                         (1UL << MOTOR2_IN1) | (1UL << MOTOR2_IN2);
  REG_WRITE(GPIO_OUT_REG, REG_READ(GPIO_OUT_REG) & ~maskMotores);
}

// Rutina de Servicio de Interrupción (ISR) ejecutada al expirar el Timer
void IRAM_ATTR onTimerMovimiento() {
  portENTER_CRITICAL_ISR(&timerMux);
  detenerMotoresRegistro();
  moviendo = false;
  comando = QUIETO;
  portEXIT_CRITICAL_ISR(&timerMux);
}

// Muestra el carácter en el display escribiendo directo en los registros GPIO
void printdisplay(char caracter) {
  int indice = -1;
  if (caracter >= '0' && caracter <= '9') indice = caracter - '0';
  else if (caracter >= 'A' && caracter <= 'D') indice = caracter - 'A' + 10;
  else if (caracter == '*') indice = 14;
  else if (caracter == '#') indice = 15;

  if (indice == -1) return;

  uint8_t pat = segments[indice];

  // Limpieza y actualización de los pines del banco 0 (GPIO 22, 23, 25, 26, 27)
  uint32_t reg0 = REG_READ(GPIO_OUT_REG) & ~((1UL<<22) | (1UL<<23) | (1UL<<25) | (1UL<<26) | (1UL<<27));
  reg0 |= ((pat & 0x20) << 17) |  // Segmento f -> GPIO 22
          ((pat & 0x10) << 19) |  // Segmento e -> GPIO 23
          ((pat & 0x07) << 25);   // Segmentos a, b, c -> GPIO 25, 26, 27
  REG_WRITE(GPIO_OUT_REG, reg0);

  // Limpieza y actualización de los pines del banco 1 (GPIO 32 y 33)
  uint32_t reg1 = REG_READ(GPIO_OUT1_REG) & ~((1UL << (32-32)) | (1UL << (33-32)));
  reg1 |= ((pat & 0x08) >> 3) |    // Segmento d -> GPIO 32
          ((pat & 0x40) >> 5);     // Segmento g -> GPIO 33
  REG_WRITE(GPIO_OUT1_REG, reg1);
}

// Barrido del teclado matricial por manipulación de registros
char readKeypad() {
  char tecla = 0;
  for (int row = 0; row < 4; row++) {
    uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
    uint32_t filaActiva = (1UL << pinesFilas[row]);

    // Todas las filas en alto excepto la que se está leyendo
    REG_WRITE(GPIO_OUT_REG, (estadoSalida | MASCARA_FILAS) & ~filaActiva);
    delayMicroseconds(10);

    uint32_t entrada = REG_READ(GPIO_IN_REG);
    bool c1 = (entrada & (1UL << C1)) == 0;
    bool c2 = (entrada & (1UL << C2)) == 0;
    bool c3 = (entrada & (1UL << C3)) == 0;
    bool c4 = (entrada & (1UL << C4)) == 0;

    if (c1) { tecla = matTeclas[row][0]; break; }
    if (c2) { tecla = matTeclas[row][1]; break; }
    if (c3) { tecla = matTeclas[row][2]; break; }
    if (c4) { tecla = matTeclas[row][3]; break; }
  }
  uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
  REG_WRITE(GPIO_OUT_REG, estadoSalida | MASCARA_FILAS);
  return tecla;
}

// Envía las señales de dirección a ambos motores (Puente H)
void controlarMotores(int m1a, int m1b, int m2a, int m2b) {
  digitalWrite(MOTOR1_IN1, m1a);
  digitalWrite(MOTOR1_IN2, m1b);
  digitalWrite(MOTOR2_IN1, m2a);
  digitalWrite(MOTOR2_IN2, m2b);
}

// Programa la alarma del Timer de hardware (API ESP32 v3.x) en milisegundos
void iniciarTemporizadorMovimiento(unsigned long tiempo_ms) {
  portENTER_CRITICAL(&timerMux);
  moviendo = true;
  portEXIT_CRITICAL(&timerMux);

  timerStop(timerMovimiento);
  timerWrite(timerMovimiento, 0);
  // Con frecuencia base de 1 MHz (1 tick = 1 us), tiempo_ms * 1000 establece el disparo único
  timerAlarm(timerMovimiento, tiempo_ms * 1000ULL, false, 0);
  timerStart(timerMovimiento);
}

// Detiene de emergencia los motores y cancela el Timer activo
void paradaEmergencia() {
  timerStop(timerMovimiento);

  portENTER_CRITICAL(&timerMux);
  detenerMotoresRegistro();
  moviendo = false;
  comando = QUIETO;
  portEXIT_CRITICAL(&timerMux);

  printdisplay('*');
  Serial.println("* -> Parada de emergencia activada");
}

// Conversión provisional de distancia (cm) y ángulo (grados) a milisegundos
int distancia_tiempo(int val) { return val * 100; }
int angulo_tiempo(int val) { return val * 15; }

// Decodifica la cadena recibida y asigna la acción y su magnitud
void interpreta_comando(char* cadena) {
  int len = strlen(cadena);
  if (len < 2) {
    comando = QUIETO;
    return;
  }

  // Evalúa doble 'A' para retroceso (ej. AA20#)
  if (cadena[0] == 'A' && cadena[1] == 'A') {
    comando = ATRAS;
    valor = atoi(&cadena[2]);
    if (valor <= 0) valor = 10;
    Serial.print("Atrás ");
    Serial.println(valor);
  }
  // Evalúa comandos estándar Letra + Número 
  else if (cadena[0] == 'A') {
    comando = ADELANTE;
    valor = atoi(&cadena[1]);
    if (valor <= 0) valor = 10;
    Serial.print("Adelante ");
    Serial.println(valor);
  }
  else if (cadena[0] == 'B') {
    comando = DERECHA;
    valor = atoi(&cadena[1]);
    if (valor <= 0) valor = 90;
    Serial.print("Derecha ");
    Serial.println(valor);
  }
  else if (cadena[0] == 'C') {
    comando = IZQUIERDA;
    valor = atoi(&cadena[1]);
    if (valor <= 0) valor = 90;
    Serial.print("Izquierda ");
    Serial.println(valor);
  }
  // Evalúa accionamiento directo de pluma (D#)
  else if (cadena[0] == 'D') {
    static bool plumaAbajo = false;
    plumaAbajo = !plumaAbajo;
    comando = plumaAbajo ? BAJAPLUMA : LEVANTAPLUMA;
    valor = 0;
    Serial.println(plumaAbajo ? "Pluma abajo" : "Pluma arriba");
  }
  else {
    comando = QUIETO;
    valor = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("=== ROBOT DIBUJANTE - INTERRUPCIONES POR TIMER ===");

  for (int i = 0; i < 7; i++) pinMode(pinesDisplay[i], OUTPUT);

  pinMode(R1, OUTPUT); pinMode(R2, OUTPUT); pinMode(R3, OUTPUT); pinMode(R4, OUTPUT);
  pinMode(C1, INPUT_PULLUP); pinMode(C2, INPUT_PULLUP); pinMode(C3, INPUT_PULLUP); pinMode(C4, INPUT_PULLUP);

  pinMode(MOTOR1_IN1, OUTPUT); pinMode(MOTOR1_IN2, OUTPUT);
  pinMode(MOTOR2_IN1, OUTPUT); pinMode(MOTOR2_IN2, OUTPUT);

  detenerMotoresRegistro();

  // Inicialización del Timer a 1 MHz (1 tick = 1 microsegundo) en core ESP32 v3.x
  timerMovimiento = timerBegin(1000000);
  timerAttachInterrupt(timerMovimiento, &onTimerMovimiento);
}

void loop() {
  static char teclaAnterior = 0;
  static char cad_comando[20] = {0};
  static int pos = 0;

  // Lectura continua del teclado sin bloquear la ejecución del procesador
  char key = readKeypad();

  // Detección de flanco al presionar una tecla nueva
  if (key != 0 && teclaAnterior == 0) {
    
    // Si se presiona '*', detiene inmediatamente esté o no en movimiento
    if (key == '*') {
      paradaEmergencia();
      pos = 0;
      cad_comando[0] = '\0';
    } 
    // Solo acepta nuevos comandos cuando el robot no está ejecutando un movimiento
    else if (!moviendo) {
      printdisplay(key);
      cad_comando[pos] = key;
      pos++;

      // Al recibir '#' o llenar el búfer, decodifica y dispara el movimiento
      if (key == '#' || pos >= sizeof(cad_comando) - 1) {
        cad_comando[pos] = '\0';
        pos = 0;
        interpreta_comando(cad_comando);

        if (comando == ADELANTE) {
          controlarMotores(HIGH, LOW, HIGH, LOW);
          iniciarTemporizadorMovimiento(distancia_tiempo(valor));
        }
        else if (comando == ATRAS) {
          controlarMotores(LOW, HIGH, LOW, HIGH);
          iniciarTemporizadorMovimiento(distancia_tiempo(valor));
        }
        else if (comando == DERECHA) {
          controlarMotores(HIGH, LOW, LOW, HIGH);
          iniciarTemporizadorMovimiento(angulo_tiempo(valor));
        }
        else if (comando == IZQUIERDA) {
          controlarMotores(LOW, HIGH, HIGH, LOW);
          iniciarTemporizadorMovimiento(angulo_tiempo(valor));
        }
        else if (comando == BAJAPLUMA || comando == LEVANTAPLUMA) {
          detenerMotoresRegistro();
          iniciarTemporizadorMovimiento(400);
        }
      }
    }
  }

  teclaAnterior = key;
  delay(15); // Antirrebote ligero sin impedir la detección inmediata de '*'
}
