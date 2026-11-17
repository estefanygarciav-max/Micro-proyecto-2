#include <Arduino.h>
#include <stdint.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"

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

volatile int comando = QUIETO;             // Acción actual en ejecución
volatile int valor = 0;                    // Distancia (cm) o ángulo (grados)
volatile bool moviendo = false;            // Bandera modificada por la ISR del Timer
volatile bool movimientoTerminado = false; // Bandera para avisar "OK#" al finalizar

String cad_comando = "";                   // Variable global para almacenar el comando leído

// Puntero al Timer de hardware del ESP32
hw_timer_t *timerMovimiento = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

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
  movimientoTerminado = true; // Marca que el movimiento finalizó para enviar OK#
  portEXIT_CRITICAL_ISR(&timerMux);
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
  movimientoTerminado = false;
  portEXIT_CRITICAL(&timerMux);

  timerStop(timerMovimiento);
  timerWrite(timerMovimiento, 0);
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
  movimientoTerminado = true;
  portEXIT_CRITICAL(&timerMux);

  Serial.println("* -> Parada de emergencia activada");
}

// Conversión de distancia (cm) y ángulo (grados) a milisegundos
int distancia_tiempo(int val) { return val * 100; }
int angulo_tiempo(int val) { return val * 15; }

// Decodifica la cadena recibida y asigna la acción y su magnitud
void interpreta_comando(String cadena) {
  cadena.trim(); // Elimina espacios o saltos de línea residuales
  int len = cadena.length();
  
  if (len == 0) {
    comando = QUIETO;
    return;
  }

  // Parada de emergencia si llega '*'
  if (cadena[0] == '*') {
    paradaEmergencia();
    return;
  }

  // Evalúa doble 'A' para retroceso (ej. AA20)
  if (len >= 2 && cadena[0] == 'A' && cadena[1] == 'A') {
    comando = ATRAS;
    valor = cadena.substring(2).toInt();
    if (valor <= 0) valor = 10;
    Serial.print("Atras ");
    Serial.println(valor);
  }
  // Evalúa avance frontal (ej. A15)
  else if (cadena[0] == 'A') {
    comando = ADELANTE;
    valor = cadena.substring(1).toInt();
    if (valor <= 0) valor = 10;
    Serial.print("Adelante ");
    Serial.println(valor);
  }
  // Evalúa giro derecha (ej. B90)
  else if (cadena[0] == 'B') {
    comando = DERECHA;
    valor = cadena.substring(1).toInt();
    if (valor <= 0) valor = 90;
    Serial.print("Derecha ");
    Serial.println(valor);
  }
  // Evalúa giro izquierda (ej. C90)
  else if (cadena[0] == 'C') {
    comando = IZQUIERDA;
    valor = cadena.substring(1).toInt();
    if (valor <= 0) valor = 90;
    Serial.print("Izquierda ");
    Serial.println(valor);
  }
  // Evalúa accionamiento de pluma (D)
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

// Función modificada
bool leer_comando_teclado() {
  if (Serial.available() > 0) {
    cad_comando = Serial.readStringUntil('#');
    cad_comando.trim();
    if (cad_comando.length() > 0) {
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.println("=== MICRO 1: ROBOT RECEPTOR LISTO ===");
  Serial.println("Escribe comandos terminados en # (ej: A15#, AA20#, B90#, C90#, D#)");

  pinMode(MOTOR1_IN1, OUTPUT); pinMode(MOTOR1_IN2, OUTPUT);
  pinMode(MOTOR2_IN1, OUTPUT); pinMode(MOTOR2_IN2, OUTPUT);

  detenerMotoresRegistro();

  // Inicialización del Timer a 1 MHz (1 tick = 1 microsegundo)
  timerMovimiento = timerBegin(1000000);
  timerAttachInterrupt(timerMovimiento, &onTimerMovimiento);
}

void loop() {
  // 1. Verificar si el Timer acaba de finalizar un movimiento para responder OK#
  if (movimientoTerminado) {
    portENTER_CRITICAL(&timerMux);
    movimientoTerminado = false;
    portEXIT_CRITICAL(&timerMux);

    Serial.println("OK#");
  }

  // 2. Leer comandos entrantes mediante la función modificada
  if (leer_comando_teclado()) {
    Serial.print("Recibido: ");
    Serial.println(cad_comando);

    // Si recibe '*' detiene de inmediato aunque esté en movimiento
    if (cad_comando.indexOf('*') != -1) {
      paradaEmergencia();
      return;
    }

    // Solo procesa un nuevo movimiento si no está ejecutando otro
    if (!moviendo) {
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
      else {
        // Si el comando no fue válido, responde OK# para no bloquear el protocolo
        Serial.println("OK#");
      }
    }
  }
}
