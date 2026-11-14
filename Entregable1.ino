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

// Pines para el puente H (LEDs)
#define M1_IN1  0
#define M1_IN2  2
#define M2_IN1  4
#define M2_IN2  5

// Tabla de verdad para display de 7 segmentos (cátodo común)
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
  0b1000000, // * 
  0b1111011  // #
};

// Máscara de bits para manipular las 4 filas al tiempo
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

// Muestra el carácter en el display escribiendo directo en los registros GPIO
void printdisplay(char caracter) {
  int indice = -1;

  if (caracter >= '0' && caracter <= '9') {
    indice = caracter - '0';
  } else if (caracter >= 'A' && caracter <= 'D') {
    indice = caracter - 'A' + 10;
  } else if (caracter == 'a' && caracter <= 'd') {
    indice = caracter - 'a' + 10;
  } else if (caracter == '*') {
    indice = 14;
  } else if (caracter == '#') {
    indice = 15;
  }

  if (indice == -1) return;

  uint8_t pat = segments[indice];

  // Limpiamos y actualizamos los pines del banco 0 (GPIO 22, 23, 25, 26, 27)
  uint32_t reg0 = REG_READ(GPIO_OUT_REG) & ~((1UL<<22) | (1UL<<23) | (1UL<<25) | (1UL<<26) | (1UL<<27));
  
  reg0 |= ((pat & 0x20) << 17) |  
          ((pat & 0x10) << 19) |  
          ((pat & 0x07) << 25);   

  REG_WRITE(GPIO_OUT_REG, reg0);

  // Actualizamos los pines del banco 1 (GPIO 32 y 33)
  uint32_t reg1 = REG_READ(GPIO_OUT1_REG) & ~((1UL << (32-32)) | (1UL << (33-32)));
  
  reg1 |= ((pat & 0x08) >> 3) |    
          ((pat & 0x40) >> 5);    

  REG_WRITE(GPIO_OUT1_REG, reg1);
}

// Barrido del teclado matricial por manipulación de registros
char readKeypad() {
  char tecla = 0;

  for (int row = 0; row < 4; row++) {
    uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
    uint32_t filaActiva = (1UL << pinesFilas[row]);

    // Ponemos todas las filas en alto excepto la que estamos leyendo
    REG_WRITE(GPIO_OUT_REG, (estadoSalida | MASCARA_FILAS) & ~filaActiva);
    delayMicroseconds(10); // Pequeña espera para estabilizar la señal

    uint32_t entrada = REG_READ(GPIO_IN_REG);

    // Leemos las columnas (lógica inversa por los pull-up)
    bool c1 = (entrada & (1UL << C1)) == 0;
    bool c2 = (entrada & (1UL << C2)) == 0;
    bool c3 = (entrada & (1UL << C3)) == 0;
    bool c4 = (entrada & (1UL << C4)) == 0;

    if (c1) { tecla = matTeclas[row][0]; break; }
    if (c2) { tecla = matTeclas[row][1]; break; }
    if (c3) { tecla = matTeclas[row][2]; break; }
    if (c4) { tecla = matTeclas[row][3]; break; }
  }

  // Dejamos todas las filas en alto al terminar el barrido
  uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
  REG_WRITE(GPIO_OUT_REG, estadoSalida | MASCARA_FILAS);

  return tecla;
}

// Envía las señales de dirección a ambos motores
void controlarMotores(int m1a, int m1b, int m2a, int m2b) {
  digitalWrite(M1_IN1, m1a);
  digitalWrite(M1_IN2, m1b);
  digitalWrite(M2_IN1, m2a);
  digitalWrite(M2_IN2, m2b);
}

void setup() {
  Serial.begin(115200);
  Serial.println("=== ROBOT LOGO DIBUJANTE - ULTRA OPTIMIZADO ===");

  // Pines asignados a los segmentos a, b, c, d, e, f, g
  const uint8_t pinesDisplay[7] = {25, 26, 27, 32, 23, 22, 33}; 
  for (int i = 0; i < 7; i++) {
    pinMode(pinesDisplay[i], OUTPUT);
  }

  // Filas como salidas
  pinMode(R1, OUTPUT);
  pinMode(R2, OUTPUT);
  pinMode(R3, OUTPUT);
  pinMode(R4, OUTPUT);

  // Columnas con resistencia pull-up interna
  pinMode(C1, INPUT_PULLUP);
  pinMode(C2, INPUT_PULLUP);
  pinMode(C3, INPUT_PULLUP);
  pinMode(C4, INPUT_PULLUP);

  // Salidas hacia los drivers de los motores
  pinMode(M1_IN1, OUTPUT);
  pinMode(M1_IN2, OUTPUT);
  pinMode(M2_IN1, OUTPUT);
  pinMode(M2_IN2, OUTPUT);

  // Arrancamos con los motores apagados
  controlarMotores(LOW, LOW, LOW, LOW);
}

void loop() {
  static char teclaAnterior = 0;
  static bool esperandoA = false;
  static unsigned long tiempoA = 0;
  static bool plumaAbajo = false;

  char key = readKeypad();

  // Detectamos flanco de bajada (cuando recién se presiona una tecla)
  if (key != 0 && teclaAnterior == 0) {
    printdisplay(key);

    // Lógica de doble pulsación para la tecla 'A' (avanzar vs retroceder)
    if (key == 'A') {
      if (esperandoA && millis() - tiempoA <= 700) {
        controlarMotores(LOW, HIGH, LOW, HIGH);
        Serial.println("AA -> Atrás");
        esperandoA = false;
      } else {
        esperandoA = true;
        tiempoA = millis();
      }
    } else {
      esperandoA = false;

      if (key == 'B') {
        controlarMotores(HIGH, LOW, LOW, HIGH);
        Serial.println("B -> Giro derecha");
      } 
      else if (key == 'C') {
        controlarMotores(LOW, HIGH, HIGH, LOW);
        Serial.println("C -> Giro izquierda");
      } 
      else if (key == '*') {
        controlarMotores(LOW, LOW, LOW, LOW);
        Serial.println("* -> Parar");
      } 
      else if (key == 'D') {
        plumaAbajo = !plumaAbajo;
        controlarMotores(LOW, LOW, LOW, LOW);
        if (plumaAbajo) {
          Serial.println("D -> Bajar pluma");
        } else {
          Serial.println("D -> Levantar pluma");
        }
      } 
      else if (key == '#') {
        Serial.println("# -> Enter");
      }
      else {
        Serial.print(key);
        Serial.println(" -> Tecla de control (distancia/ángulo)");
      }
    }
  }

  // Si pasó el tiempo de espera y no se volvió a hundir 'A', avanza normal
  if (esperandoA && millis() - tiempoA > 700) {
    controlarMotores(HIGH, LOW, HIGH, LOW);
    Serial.println("A -> Adelante");
    esperandoA = false;
  }

  teclaAnterior = key;
  delay(15); // Antirrebote básico
}
