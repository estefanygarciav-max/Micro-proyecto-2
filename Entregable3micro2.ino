#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"

// Configuración de la pantalla OLED SSD1306 
#define ANCHO_OLED 128
#define ALTO_OLED 64
#define OLED_RESET -1
Adafruit_SSD1306 display(ANCHO_OLED, ALTO_OLED, &Wire, OLED_RESET);

// Pines del teclado matricial 4x4 
#define R1 12
#define R2 13
#define R3 14
#define R4 15

#define C1 16
#define C2 17
#define C3 18
#define C4 19

const uint32_t MASCARA_FILAS = (1UL << R1) | (1UL << R2) | (1UL << R3) | (1UL << R4);
const uint8_t pinesFilas[4] = {R1, R2, R3, R4};

const char matTeclas[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Memoria no volátil (NVS) para conservar el último programa tras apagar el ESP32
Preferences memoria;
String historialInteractivo = "";

// Estados de navegación de la interfaz OLED
enum EstadoMenu {
  MENU_PRINCIPAL,
  MODO_INTERACTIVO,
  MODO_PROGRAMA
};

EstadoMenu estadoActual = MENU_PRINCIPAL;
String comandoActual = "";

// Lectura del teclado matricial por registros 
char readKeypad() {
  char tecla = 0;
  for (int row = 0; row < 4; row++) {
    uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
    uint32_t filaActiva = (1UL << pinesFilas[row]);

    REG_WRITE(GPIO_OUT_REG, (estadoSalida | MASCARA_FILAS) & ~filaActiva);
    delayMicroseconds(10);

    uint32_t entrada = REG_READ(GPIO_IN_REG);
    if ((entrada & (1UL << C1)) == 0) { tecla = matTeclas[row][0]; break; }
    if ((entrada & (1UL << C2)) == 0) { tecla = matTeclas[row][1]; break; }
    if ((entrada & (1UL << C3)) == 0) { tecla = matTeclas[row][2]; break; }
    if ((entrada & (1UL << C4)) == 0) { tecla = matTeclas[row][3]; break; }
  }
  uint32_t estadoSalida = REG_READ(GPIO_OUT_REG);
  REG_WRITE(GPIO_OUT_REG, estadoSalida | MASCARA_FILAS);
  return tecla;
}


// FUNCIONES DE PANTALLA OLED 

void dibujarMenuPrincipal() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(">>>>> ROBOTLOGO");
  display.setCursor(0, 20);
  display.println("1> Modo Interactivo");
  display.setCursor(0, 35);
  display.println("2> Modo Programa");
  display.display();
}

void dibujarModoInteractivo(String textoComando, int puntosProgreso) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.println(">>>>> MODO INTERACTIVO");
  
  display.setCursor(0, 14);
  display.println("Escriba comando");
  display.setCursor(0, 23);
  display.println("y presione #");
  
  display.setCursor(0, 36);
  display.println("Guardar/Salir *");
  
  display.setCursor(0, 46);
  display.print("> ");
  display.println(textoComando);

  // Dibuja la barra animada de puntos mientras el robot ejecuta el movimiento
  if (puntosProgreso > 0) {
    display.setCursor(0, 56);
    for (int i = 0; i < puntosProgreso; i++) {
      display.print(".");
    }
  }
  display.display();
}

void dibujarModoPrograma(int puntosProgreso) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.println(">>>>> MODO PROGRAMA");
  
  display.setCursor(0, 14);
  display.println("1>Cuadrado");
  display.setCursor(0, 24);
  display.println("2>Triangulo");
  display.setCursor(0, 34);
  display.println("3) Ultimo guardado");
  display.setCursor(0, 44);
  display.println("Salir*");

  // Barra animada de puntos durante la ejecución de un programa
  if (puntosProgreso > 0) {
    display.setCursor(0, 56);
    for (int i = 0; i < puntosProgreso; i++) {
      display.print(".");
    }
  }
  display.display();
}


// COMUNICACIÓN Y SINCRONIZACIÓN CON MICRO 1 (ENVÍO + ESPERA DE "OK#")

void enviarYEsperarConfirmacion(String cmd, bool esModoInteractivo) {
  // Envía el comando al Micro 1 terminado en '#'
  Serial.println();
  Serial.print("[MANDO -> ROBOT]: ");
  Serial.print(cmd);
  Serial.println("#");

  String respuesta = "";
  int puntos = 1;
  unsigned long ultimoRefresco = millis();

  // Bucle de espera activa hasta recibir "OK#" desde Micro 1 (o Monitor Serie)
  while (true) {
    // 1. Actualizar animación de la barra de puntos en la OLED cada 250 ms
    if (millis() - ultimoRefresco >= 250) {
      ultimoRefresco = millis();
      puntos++;
      if (puntos > 21) puntos = 1; // 21 puntos llenan el ancho de 128px

      if (esModoInteractivo) {
        dibujarModoInteractivo(cmd, puntos);
      } else {
        dibujarModoPrograma(puntos);
      }
    }

    // 2. Verificar si el usuario presiona '*' para cancelar
    char tecla = readKeypad();
    if (tecla == '*') {
      Serial.println("[MANDO -> ROBOT]: *");
      delay(200);
      break;
    }

    // 3. Leer respuesta desde Micro 1
    while (Serial.available() > 0) {
      char c = Serial.read();
      if (c == '#') {
        respuesta.trim();
        if (respuesta.endsWith("OK")) {
          return; // El robot terminó el movimiento exitosamente
        }
        respuesta = "";
      } else if (c != '\n' && c != '\r') {
        respuesta += c;
      }
    }
    delay(10);
  }
}

// Ejecuta una secuencia completa de comandos separados por '#' (ej. "A15#B90#...")
void ejecutarSecuenciaPrograma(String secuencia) {
  if (secuencia.length() == 0) {
    display.clearDisplay();
    display.setCursor(0, 25);
    display.println("Sin programa guardado");
    display.display();
    delay(1500);
    dibujarModoPrograma(0);
    return;
  }

  int inicio = 0;
  int fin = secuencia.indexOf('#', inicio);

  while (fin != -1) {
    String cmdIndividual = secuencia.substring(inicio, fin);
    cmdIndividual.trim();
    if (cmdIndividual.length() > 0) {
      enviarYEsperarConfirmacion(cmdIndividual, false);
    }
    inicio = fin + 1;
    fin = secuencia.indexOf('#', inicio);
  }

  // Al terminar todo el programa, limpia la barra de puntos
  dibujarModoPrograma(0);
}

// SETUP Y LOOP PRINCIPAL

void setup() {
  Serial.begin(115200);

  // Configurar pines del teclado
  pinMode(R1, OUTPUT); pinMode(R2, OUTPUT); pinMode(R3, OUTPUT); pinMode(R4, OUTPUT);
  pinMode(C1, INPUT_PULLUP); pinMode(C2, INPUT_PULLUP); pinMode(C3, INPUT_PULLUP); pinMode(C4, INPUT_PULLUP);

  // Inicializar pantalla OLED SSD1306
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Error al iniciar OLED SSD1306");
    for (;;);
  }

  // Recuperar último programa guardado en memoria Flash (si existe)
  memoria.begin("robotlogo", false);
  historialInteractivo = memoria.getString("ultimo_prog", "");

  dibujarMenuPrincipal();
  Serial.println("=== MICRO 2: MANDO INALAMBRICO LISTO ===");
  Serial.println("Tip prueba unitaria: escribe OK# en el Monitor Serie cuando el robot deba confirmar.");
}

void loop() {
  static char teclaAnterior = 0;
  char key = readKeypad();

  if (key != 0 && teclaAnterior == 0) {
    
    // ------------------------------------------------------------------------
    // 1. MENÚ PRINCIPAL (Figura 2)
    // ------------------------------------------------------------------------
    if (estadoActual == MENU_PRINCIPAL) {
      if (key == '1') {
        estadoActual = MODO_INTERACTIVO;
        comandoActual = "";
        historialInteractivo = ""; // Reinicia el histórico al entrar a nueva sesión
        dibujarModoInteractivo(comandoActual, 0);
      } 
      else if (key == '2') {
        estadoActual = MODO_PROGRAMA;
        dibujarModoPrograma(0);
      }
      // Cualquier otra tecla, incluido '*', es ignorada según la guía
    }

    // 2. MODO INTERACTIVO 
  
    else if (estadoActual == MODO_INTERACTIVO) {
      if (key == '*') {
        // Guarda la sesión en memoria no volátil (NVS) y vuelve al menú principal
        if (historialInteractivo.length() > 0) {
          memoria.putString("ultimo_prog", historialInteractivo);
          Serial.print("Programa guardado en Flash: ");
          Serial.println(historialInteractivo);
        }
        estadoActual = MENU_PRINCIPAL;
        dibujarMenuPrincipal();
      } 
      else if (key == '#') {
        if (comandoActual.length() > 0) {
          // Añade al histórico de la sesión actual
          historialInteractivo += comandoActual + "#";
          // Envía al robot y muestra barra de puntos hasta recibir OK#
          enviarYEsperarConfirmacion(comandoActual, true);
          // Limpia entrada y vuelve a vista Figura 3-1
          comandoActual = "";
          dibujarModoInteractivo(comandoActual, 0);
        }
      } 
      else {
        // Añade el carácter presionado (A, B, C, D, 0-9) y lo muestra en pantalla
        if (comandoActual.length() < 12) {
          comandoActual += key;
          dibujarModoInteractivo(comandoActual, 0);
        }
      }
    }
    // 3. MODO PROGRAMA
    
    else if (estadoActual == MODO_PROGRAMA) {
      if (key == '*') {
        estadoActual = MENU_PRINCIPAL;
        dibujarMenuPrincipal();
      } 
      else if (key == '1') {
        // Programa 1: Cuadrado (4 lados de 15 cm y giros de 90 grados)
        ejecutarSecuenciaPrograma("A15#B90#A15#B90#A15#B90#A15#B90#");
      } 
      else if (key == '2') {
        // Programa 2: Triángulo equilátero (3 lados de 15 cm y giros exteriores de 120 grados)
        ejecutarSecuenciaPrograma("A15#B120#A15#B120#A15#B120#");
      } 
      else if (key == '3') {
        // Programa 3: Último guardado en memoria persistente
        String guardado = memoria.getString("ultimo_prog", "");
        ejecutarSecuenciaPrograma(guardado);
      }
    }
  }

  teclaAnterior = key;
  delay(20); // Antirrebote del teclado
}
