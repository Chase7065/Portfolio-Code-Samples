/**
 * @file    MultiCore_Control.cpp
 * @brief   Dual-core (FreeRTOS) firmware controller for the GermPass UVC
 *          disinfection unit.
 *
 * @details Coordinates door actuation, UV cleaning cycles, occupancy sensing,
 *          status indication, battery / low-power monitoring, and Bluetooth
 *          communication across the ESP32's two cores:
 *
 *            Core 0 - sensing / communication:
 *              Sensor_Check, IND_Control, BLE_Control, Wifi_Control
 *
 *            Core 1 - mechanism / state machine:
 *              Open_Unit, Close_Unit, Clean_Unit, SBI_Control, LowPower_Check
 *
 *          Software timers schedule the door-open delay, routine cleaning
 *          interval, Bluetooth scan window, and per-cycle UV timing.
 *
 * @author  Chase Bryson
 * @target  ESP32 (Arduino core + FreeRTOS)
 */

#include "Arduino.h"
#include "BluetoothSerial.h"
#include "BTAdvertisedDevice.h"

#include "Battery.h"
#include "ButtonInterface.h"
#include "Door.h"
#include "Indicator.h"
#include "Motor.h"
#include "Sensor.h"
#include "UV.h"
#include "IOExpander.h"
#include <Wire.h>

// =========================================================================
//  Configuration
// =========================================================================

// Serial / debug
static const uint32_t SERIAL_BAUD = 115200;
#define DEBUG_ENABLED 1   // set to 0 to silence all serial debug output

// I/O
static const uint8_t SENSOR_PIN = 26;   // active-low occupancy sensor

// Bluetooth
static const char* BLE_DEVICE_NAME = "Germpass";

// Timing (milliseconds)
static const uint32_t DISCOVER_DELAY_MS    = 10000;    // BLE scan window
static const uint32_t TIME_PER_CYCLE_MS    = 5000;     // UV time per cycle
static const uint32_t OPEN_DOOR_DELAY_MS   = 10000;    // hold door open
static const uint32_t CLEANING_INTERVAL_MS = 1800000;  // routine clean (30 min)
static const uint32_t TASK_INIT_DELAY_MS   = 500;      // stagger task startup

// Reported cycle count
static const int CYCLES_PER_RUN = 5;

// FreeRTOS core assignment
static const BaseType_t CORE_COMM = 0;  // sensing / communication
static const BaseType_t CORE_MECH = 1;  // mechanism / state machine

// FreeRTOS task stack sizes (words) - tuned via stack high-water-mark profiling
static const uint32_t STACK_SENSOR    = 1220;
static const uint32_t STACK_IND       = 1440;
static const uint32_t STACK_BLE       = 2060;
static const uint32_t STACK_WIFI      = 860;
static const uint32_t STACK_DOOR_INIT = 1350;
static const uint32_t STACK_OPEN      = 1680;
static const uint32_t STACK_CLOSE     = 1420;
static const uint32_t STACK_CLEAN     = 1550;
static const uint32_t STACK_SBI       = 1580;
static const uint32_t STACK_LOWPOWER  = 1840;

// =========================================================================
//  Debug logging
// =========================================================================

#if DEBUG_ENABLED
  #define DBG_PRINT(x)    Serial.print(x)
  #define DBG_PRINTLN(x)  Serial.println(x)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
#endif

/**
 * @brief Logs the free heap and the calling task's stack high-water mark.
 * @param context Short tag identifying the call site (e.g. "BLE_CONTROL").
 */
static void logRuntimeStats(const char* context)
{
#if DEBUG_ENABLED
  Serial.printf("[%s] Free heap: %u bytes | Stack HWM: %u words\n",
                context,
                (unsigned)xPortGetFreeHeapSize(),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
  (void)context;
#endif
}

// =========================================================================
//  Bluetooth command protocol
// =========================================================================

/// Command codes exchanged with a peer unit / companion app.
enum BTCodes
{
  NOPERSON          = 0,  // no occupant detected
  PERSON            = 1,  // occupant detected
  CYCLES_RUN        = 2,  // request: number of completed cycles
  LENGTH_OF_RUNTIME = 3,  // request: total runtime
  DEBRA_MODE_CMD    = 4   // request: enter DEBRA_MODE
};

// =========================================================================
//  Unit state machine
// =========================================================================

enum unitStates
{
  OPENED = 0,
  OPENING,
  CLOSED,
  CLOSING,
  CLEAN,
  CLEANING,
  LOWPOWER,
  SETTINGS,
  DEBRA_MODE
};
static enum unitStates currentState;

// =========================================================================
//  Peripherals
// =========================================================================

IO_Expander     IO;                 // I2C I/O expander
BluetoothSerial Bluetooth;
Battery         battery;
Door            door;
Indicator       IND;                // status indicator (pin 22)
Sensor          sensor(SENSOR_PIN); // occupancy sensor
UV              uv;
ButtonInterface SBI;                // single-button interface

static bool clean = false;
static bool scan  = true;

// =========================================================================
//  FreeRTOS handles & prototypes
// =========================================================================

// --- Core 0: sensing / communication ---
static TaskHandle_t Sensor_Task = NULL;
static TaskHandle_t IND_Task    = NULL;
static TaskHandle_t BLE_Task    = NULL;
static TaskHandle_t Wifi_Task   = NULL;

void Sensor_Check(void* parameter);
void IND_Control(void* parameter);
void BLE_Control(void* parameter);
void Wifi_Control(void* parameter);

// --- Core 1: mechanism / state machine ---
static TaskHandle_t Setup_Task    = NULL;
static TaskHandle_t Open_Task     = NULL;
static TaskHandle_t Close_Task    = NULL;
static TaskHandle_t Clean_Task    = NULL;
static TaskHandle_t SBI_Task      = NULL;
static TaskHandle_t LowPower_Task = NULL;

void Door_Setup(void* parameter);
void Open_Unit(void* parameter);
void Close_Unit(void* parameter);
void Clean_Unit(void* parameter);
void SBI_Control(void* parameter);
void LowPower_Check(void* parameter);

// --- Software timers ---
static TimerHandle_t Open_Timer            = NULL;
static TimerHandle_t RoutineCleaning_Timer = NULL;
static TimerHandle_t Scan_Timer            = NULL;
static TimerHandle_t Cycle_Timer           = NULL;

void OpenTimerCallback(TimerHandle_t xTimer);
void RoutineCleaningCallback(TimerHandle_t xTimer);
void BTStartScan(TimerHandle_t xTimer);
void cleanCycleOver(TimerHandle_t xTimer);

// =========================================================================
//  Setup
// =========================================================================

void setup()
{
  Serial.begin(SERIAL_BAUD);
  vTaskDelay(pdMS_TO_TICKS(1000));

  DBG_PRINTLN("====== Multicore startup ======");
  logRuntimeStats("BOOT");

  // ---- Initialize peripherals ----
  // TODO: enable INPUT_PULLUP once sensor wiring is confirmed.
  // pinMode(SENSOR_PIN, INPUT_PULLUP);

  IO.begin();
  // uv.begin(IO);   // enabled once UV driver wiring is verified
  // SBI.begin();    // initialized inside SBI_Control
  door.begin(IO);
  battery.begin();
  IND.begin();

  // ---- Core 0: sensing / communication ----
  xTaskCreatePinnedToCore(Sensor_Check, "Sensor_Check", STACK_SENSOR, NULL, 1, &Sensor_Task, CORE_COMM);
  vTaskSuspend(Sensor_Task);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(IND_Control, "IND_Control", STACK_IND, NULL, 2, &IND_Task, CORE_COMM);
  // runs immediately
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(BLE_Control, "BLE_Control", STACK_BLE, NULL, 1, &BLE_Task, CORE_COMM);
  vTaskSuspend(BLE_Task);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  // Wi-Fi / IoT task reserved for future use:
  // xTaskCreatePinnedToCore(Wifi_Control, "Wifi_Control", STACK_WIFI, NULL, 0, &Wifi_Task, CORE_COMM);
  // vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  // ---- Core 1: mechanism / state machine ----
  // Door_Setup is only required when the accelerometer is in use:
  // xTaskCreatePinnedToCore(Door_Setup, "Door_Setup", STACK_DOOR_INIT, NULL, 3, &Setup_Task, CORE_MECH);
  // vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(Open_Unit, "Open_Unit", STACK_OPEN, NULL, 0, &Open_Task, CORE_MECH);
  // runs immediately
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(Close_Unit, "Close_Unit", STACK_CLOSE, NULL, 0, &Close_Task, CORE_MECH);
  vTaskSuspend(Close_Task);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(Clean_Unit, "Clean_Cycle", STACK_CLEAN, NULL, 0, &Clean_Task, CORE_MECH);
  vTaskSuspend(Clean_Task);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(SBI_Control, "SBI_Control", STACK_SBI, NULL, 1, &SBI_Task, CORE_MECH);
  vTaskSuspend(SBI_Task);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  xTaskCreatePinnedToCore(LowPower_Check, "LowPower_Check", STACK_LOWPOWER, NULL, 1, &LowPower_Task, CORE_MECH);
  vTaskSuspend(LowPower_Task);

  // ---- Software timers ----
  Open_Timer = xTimerCreate(
      "Open_Timer",
      pdMS_TO_TICKS(OPEN_DOOR_DELAY_MS),
      pdFALSE,                  // one-shot
      (void*)0,
      OpenTimerCallback);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  RoutineCleaning_Timer = xTimerCreate(
      "Cleaning_Timer",
      pdMS_TO_TICKS(CLEANING_INTERVAL_MS),
      pdFALSE,
      (void*)1,
      RoutineCleaningCallback);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  Scan_Timer = xTimerCreate(
      "Scan_Timer",
      pdMS_TO_TICKS(DISCOVER_DELAY_MS),
      pdFALSE,
      (void*)2,
      BTStartScan);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  Cycle_Timer = xTimerCreate(
      "Cycle_Timer",
      pdMS_TO_TICKS(TIME_PER_CYCLE_MS),
      pdFALSE,
      (void*)3,
      cleanCycleOver);
  vTaskDelay(pdMS_TO_TICKS(TASK_INIT_DELAY_MS));

  DBG_PRINTLN("====== Task setup complete ======");
  logRuntimeStats("SETUP");

  // The setup task has finished provisioning; delete it to free its stack.
  vTaskDelete(NULL);
}

void loop()
{
  // All work runs in pinned tasks; the Arduino loop task is unused.
  vTaskDelete(NULL);
}

// =========================================================================
//  Core 0 tasks
// =========================================================================

/**
 * @brief Monitors the occupancy sensor. On detection, aborts any active
 *        cleaning cycle and opens the unit, then suspends itself.
 */
void Sensor_Check(void* parameter)
{
  DBG_PRINTLN("Sensor_Check: startup");
  logRuntimeStats("SENSOR_CHECK");

  while (true)
  {
    if (digitalRead(SENSOR_PIN) == LOW)  // active-low: occupant detected
    {
      if (SBI.BTActivated())
      {
        Bluetooth.write(PERSON);
      }

      vTaskSuspend(Clean_Task);
      uv.clear();
      vTaskSuspend(Close_Task);
      vTaskResume(Open_Task);
      vTaskSuspend(Sensor_Task);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/**
 * @brief Drives the status indicator to match the current unit state.
 */
void IND_Control(void* parameter)
{
  DBG_PRINTLN("IND_Control: started");
  IND.begin();

  while (true)
  {
    switch (currentState)
    {
      case CLOSED:
      case CLOSING:
      case OPENING:
      case SETTINGS:
        IND.idle();
        break;

      case CLEANING:
        IND.cleaning();
        break;

      case LOWPOWER:
        DBG_PRINTLN("IND: low power");
        IND.lowPower();
        break;

      case DEBRA_MODE:
        DBG_PRINTLN("IND: DEBRA mode");
        IND.debraMode();
        break;

      default:
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/**
 * @brief Manages Bluetooth discovery, pairing, and inbound command handling.
 */
void BLE_Control(void* parameter)
{
  bool          forFirstTime = true;
  unsigned long scanStartTime;
  unsigned long runTimeLength;

  while (true)
  {
    DBG_PRINTLN("BLE_Control: started");

    if (forFirstTime)
    {
      scanStartTime = millis();
      DBG_PRINTLN("Bluetooth started for the first time - ready to pair.");
      forFirstTime = false;
    }

    // Scan for a peer unit within the discovery window.
    while (scanStartTime + DISCOVER_DELAY_MS > millis())
    {
      Bluetooth.begin(BLE_DEVICE_NAME, true);  // master/sender role
      Bluetooth.connect(BLE_DEVICE_NAME);
      DBG_PRINTLN("Scanning...");
      Bluetooth.getScanResults();
    }

    if (!Bluetooth.connected())
    {
      DBG_PRINTLN("No discovery made.");
      Bluetooth.begin(BLE_DEVICE_NAME);
      DBG_PRINTLN("Ready to pair...");
    }
    else
    {
      DBG_PRINTLN("Unit connected!");
    }

    DBG_PRINT("hasClient: ");
    DBG_PRINTLN(Bluetooth.hasClient());
    DBG_PRINT("isConnected: ");
    DBG_PRINTLN(Bluetooth.connected());
    DBG_PRINT("isReady: ");
    DBG_PRINTLN(Bluetooth.isReady());

    // Handle a single inbound command.
    if (Bluetooth.available() > 0)
    {
      int incomingCode = Bluetooth.read();
      DBG_PRINT("Incoming command: ");
      DBG_PRINTLN(incomingCode);

      switch (incomingCode)
      {
        case NOPERSON:
          DBG_PRINTLN("No person seen by peer.");
          break;

        case PERSON:
          DBG_PRINTLN("Person seen by peer.");
          currentState = LOWPOWER;
          break;

        case CYCLES_RUN:
          Bluetooth.print("NumOfCycles: ");
          Bluetooth.println(CYCLES_PER_RUN);
          break;

        case LENGTH_OF_RUNTIME:
          runTimeLength = millis() / 1000;
          Bluetooth.print("RunTimeInSeconds: ");
          Bluetooth.println(runTimeLength);
          break;

        case DEBRA_MODE_CMD:
          currentState = DEBRA_MODE;
          Bluetooth.print("DEBRA mode activated.");
          vTaskDelay(pdMS_TO_TICKS(10000));
          currentState = OPENING;
          break;

        default:
          DBG_PRINT("Unknown command: ");
          DBG_PRINTLN(incomingCode);
          break;
      }
    }
    else if (!SBI.BTActivated() && !Bluetooth.hasClient())
    {
      Bluetooth.disconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    logRuntimeStats("BLE_CONTROL");
  }
}

/**
 * @brief Reserved for future Wi-Fi / IoT connectivity (not yet scheduled).
 */
void Wifi_Control(void* parameter)
{
  DBG_PRINTLN("Wifi_Control: started");

  while (true)
  {
    DBG_PRINTLN("Wifi_Control: running");
    vTaskDelay(pdMS_TO_TICKS(2000));
    logRuntimeStats("WIFI_CONTROL");
  }
}

// =========================================================================
//  Core 1 tasks
// =========================================================================

/**
 * @brief One-shot door homing routine. Only required when the accelerometer
 *        is fitted; enable the corresponding task in setup() to use it.
 */
/*
void Door_Setup(void* parameter)
{
  DBG_PRINTLN("Door_Setup: running");
  door.setupRoutine();
  DBG_PRINTLN("Door_Setup: complete");
  vTaskDelete(NULL);
}
*/

/**
 * @brief Opens the door, then arms the open-hold timer and suspends itself.
 */
void Open_Unit(void* parameter)
{
  DBG_PRINTLN("Open_Unit: started");

  while (true)
  {
    currentState = OPENING;
    DBG_PRINTLN("Open_Unit: opening");

    door.backward();
    while (door.isOpened())
    {
      door.openDoor();
    }
    door.reset();

    DBG_PRINTLN("Open_Unit: finished");
    xTimerStart(Open_Timer, portMAX_DELAY);
    vTaskSuspend(Open_Task);

    logRuntimeStats("OPEN_UNIT");
  }
}

/**
 * @brief Closes the door, transitions to CLEANING, and resumes Clean_Task.
 */
void Close_Unit(void* parameter)
{
  DBG_PRINTLN("Close_Unit: started");

  while (true)
  {
    currentState = CLOSING;
    DBG_PRINTLN("Close_Unit: closing");

    door.forward();
    while (door.isClosed())
    {
      door.closeDoor();
    }

    DBG_PRINTLN("Close_Unit: finished");
    currentState = CLEANING;
    vTaskResume(Clean_Task);
    vTaskSuspend(Close_Task);

    logRuntimeStats("CLOSE_UNIT");
  }
}

/**
 * @brief Runs a UV cleaning cycle, arms the routine-cleaning timer, then
 *        reopens the unit and suspends itself.
 */
void Clean_Unit(void* parameter)
{
  DBG_PRINTLN("Clean_Unit: started");

  while (true)
  {
    currentState = CLEANING;
    clean = true;

    // TODO: replace fixed delay with timed UV run once UV driver is enabled.
    // unsigned long startTime = millis();
    // while (millis() - startTime < TIME_PER_CYCLE_MS) { uv.runCycle(); }
    // uv.clear();
    vTaskDelay(pdMS_TO_TICKS(TIME_PER_CYCLE_MS));

    DBG_PRINTLN("Clean_Unit: finished");
    xTimerStart(RoutineCleaning_Timer, portMAX_DELAY);
    currentState = OPENING;
    vTaskDelay(pdMS_TO_TICKS(10000));
    vTaskResume(Open_Task);
    vTaskSuspend(Clean_Task);

    logRuntimeStats("CLEAN_UNIT");
  }
}

/**
 * @brief Single-button interface handler: enters the settings menu on
 *        activation and suspends the mechanism tasks while active.
 */
void SBI_Control(void* parameter)
{
  DBG_PRINTLN("SBI_Control: started");

  while (true)
  {
    if (currentState == SETTINGS)
    {
      DBG_PRINTLN("SBI_Control: settings menu");

      while (!door.isOpened())
      {
        door.openDoor();
      }

      while (!SBI.userIsDone())
      {
        SBI.settingsMenu();
        vTaskDelay(pdMS_TO_TICKS(1));
      }

      DBG_PRINTLN("SBI_Control: settings complete");
      currentState = CLOSING;
      vTaskResume(Close_Task);
    }
    else if (SBI.interfaceActivated())
    {
      currentState = SETTINGS;
      DBG_PRINTLN("SBI_Control: activated");
      vTaskSuspend(Sensor_Task);
      vTaskSuspend(Open_Task);
      vTaskSuspend(Close_Task);
      vTaskSuspend(Clean_Task);
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/**
 * @brief Monitors battery level; on low power, suspends operating tasks and
 *        opens the door.
 */
void LowPower_Check(void* parameter)
{
  DBG_PRINTLN("LowPower_Check: started");

  while (true)
  {
    DBG_PRINTLN(battery.powerValueToString());

    if (battery.powerIsLow())
    {
      vTaskSuspend(SBI_Task);
      vTaskSuspend(Open_Task);
      vTaskSuspend(Close_Task);
      vTaskSuspend(Clean_Task);

      DBG_PRINTLN("LowPower_Check: power is low");
      currentState = LOWPOWER;

      while (!door.isOpened())
      {
        door.openDoor();
      }

      DBG_PRINTLN("LowPower_Check: door open, power low");
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// =========================================================================
//  Timer callbacks
// =========================================================================

/// Fires after the door has been held open; triggers the closing sequence.
void OpenTimerCallback(TimerHandle_t xTimer)
{
  vTaskResume(Close_Task);
}

/// Fires on the routine cleaning interval; starts a cleaning cycle.
void RoutineCleaningCallback(TimerHandle_t xTimer)
{
  vTaskResume(Clean_Task);
}

/// Toggles the Bluetooth scan flag at the end of the scan window.
void BTStartScan(TimerHandle_t xTimer)
{
  scan = !scan;
  DBG_PRINTLN("Scan timer elapsed.");
}

/// Marks the end of a UV cleaning cycle.
void cleanCycleOver(TimerHandle_t xTimer)
{
  clean = false;
  DBG_PRINTLN("Cleaning cycle complete.");
}
