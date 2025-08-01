/*
* This file is part of the hoverboard-firmware-hack project.
*
* Copyright (C) 2017-2018 Rene Hopf <renehopf@mac.com>
* Copyright (C) 2017-2018 Nico Stute <crinq@crinq.de>
* Copyright (C) 2017-2018 Niklas Fauth <niklas.fauth@kit.fail>
* Copyright (C) 2019-2020 Emanuel FERU <aerdronix@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h> // for abs()
#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "util.h"
#include "BLDC_controller.h"      /* BLDC's header file */
#include "rtwtypes.h"

void SystemClock_Config(void);

//------------------------------------------------------------------------
// Global variables set externally
//------------------------------------------------------------------------
extern TIM_HandleTypeDef htim_left;
extern TIM_HandleTypeDef htim_right;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern volatile adc_buf_t adc_buffer;

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;

volatile uint8_t uart_buf[200];

// Matlab defines - from auto-code generation
//---------------
extern P    rtP_Left;                   /* Block parameters (auto storage) */
extern P    rtP_Right;                  /* Block parameters (auto storage) */
extern ExtY rtY_Left;                   /* External outputs */
extern ExtY rtY_Right;                  /* External outputs */
extern ExtU rtU_Left;                   /* External inputs */
extern ExtU rtU_Right;                  /* External inputs */
//---------------

extern uint8_t     inIdx;               // input index used for dual-inputs
extern uint8_t     inIdx_prev;
extern InputStruct input1[];            // input structure
extern InputStruct input2[];            // input structure

extern int16_t speedAvg;                // Average measured speed
extern int16_t speedAvgAbs;             // Average measured speed in absolute
extern volatile uint32_t timeoutCntGen; // Timeout counter for the General timeout (PPM, PWM, Nunchuk)
extern volatile uint8_t  timeoutFlgGen; // Timeout Flag for the General timeout (PPM, PWM, Nunchuk)
extern uint8_t timeoutFlgADC;           // Timeout Flag for for ADC Protection: 0 = OK, 1 = Problem detected (line disconnected or wrong ADC data)
extern uint8_t timeoutFlgSerial;        // Timeout Flag for Rx Serial command: 0 = OK, 1 = Problem detected (line disconnected or wrong Rx data)

extern volatile int pwml;               // global variable for pwm left. -1000 to 1000
extern volatile int pwmr;               // global variable for pwm right. -1000 to 1000

extern uint8_t enable;                  // global variable for motor enable

extern int16_t batVoltage;              // global variable for battery voltage

#if defined(CONTROL_PPM_LEFT)
extern volatile uint16_t ppm_captured_value[PPM_NUM_CHANNELS+1];
#endif
#if defined(CONTROL_PWM_LEFT)
extern volatile uint16_t pwm_captured_ch1_value;
extern volatile uint16_t pwm_captured_ch2_value;
#endif


//------------------------------------------------------------------------
// Global variables set here in main.c
//------------------------------------------------------------------------
uint8_t backwardDrive;
extern volatile uint32_t buzzerTimer;
volatile uint32_t main_loop_counter;
int16_t batVoltageCalib;         // global variable for calibrated battery voltage
int16_t board_temp_deg_c;        // global variable for calibrated temperature in degrees Celsius
int16_t left_dc_curr;            // global variable for Left DC Link current 
int16_t right_dc_curr;           // global variable for Right DC Link current
int16_t dc_curr;                 // global variable for Total DC Link current 
int16_t cmdL;                    // global variable for Left Command 
int16_t cmdR;                    // global variable for Right Command 

//------------------------------------------------------------------------
// Local variables
//------------------------------------------------------------------------
typedef struct{
  uint16_t  start;
  int16_t   cmd1;
  int16_t   cmd2;
  int16_t   speedR_meas;
  int16_t   speedL_meas;
  int16_t   batVoltage;
  int16_t   boardTemp;
  uint16_t  cmdLed;
  uint16_t  checksum;
} SerialFeedback;
static SerialFeedback Feedback;
static uint8_t sideboard_leds_L;

static int16_t     speed;                // local variable for speed. -1000 to 1000
static int16_t     steer;                // local variable for steering. -1000 to 1000
static int16_t     steerRateFixdt;       // local fixed-point variable for steering rate limiter
static int16_t     speedRateFixdt;       // local fixed-point variable for speed rate limiter
static int32_t     steerFixdt;           // local fixed-point variable for steering low-pass filter
static int32_t     speedFixdt;           // local fixed-point variable for speed low-pass filter

static uint32_t    buzzerTimer_prev = 0;
static MultipleTap MultipleTapBrake;    // define multiple tap functionality for the Brake pedal

static uint16_t rate = RATE; // Adjustable rate to support multiple drive modes on startup


int main(void) {

  HAL_Init();
  __HAL_RCC_AFIO_CLK_ENABLE();
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  /* System interrupt init*/
  /* MemoryManagement_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  /* BusFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  /* UsageFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  /* SVCall_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SVCall_IRQn, 0, 0);
  /* DebugMonitor_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 0, 0);
  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);

  SystemClock_Config();

  __HAL_RCC_DMA1_CLK_DISABLE();
  MX_GPIO_Init();
  MX_TIM_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  BLDC_Init();        // BLDC Controller Init

  HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, GPIO_PIN_SET);   // Activate Latch
  Input_Lim_Init();   // Input Limitations Init
  Input_Init();       // Input Init

  HAL_ADC_Start(&hadc1);
  HAL_ADC_Start(&hadc2);

  poweronMelody();
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
  
  int32_t board_temp_adcFixdt = adc_buffer.temp << 16;  // Fixed-point filter output initialized with current ADC converted to fixed-point
  int16_t board_temp_adcFilt  = adc_buffer.temp;

  while(1) {
    if (buzzerTimer - buzzerTimer_prev > 16*DELAY_IN_MAIN_LOOP) {   // 1 ms = 16 ticks buzzerTimer

    readCommand();                        // Read Command: input1[inIdx].cmd, input2[inIdx].cmd
    calcAvgSpeed();                       // Calculate average measured speed: speedAvg, speedAvgAbs

    // ####### MOTOR ENABLING: Only if the initial input is very small (for SAFETY) #######
    if (enable == 0 && !rtY_Left.z_errCode && !rtY_Right.z_errCode && 
        ABS(input1[inIdx].cmd) < 50 && ABS(input2[inIdx].cmd) < 50){
      beepShort(6);                     // make 2 beeps indicating the motor enable
      beepShort(4); HAL_Delay(100);
      steerFixdt = speedFixdt = 0;      // reset filters
      enable = 1;                       // enable motors
      printf("-- Motors enabled --\r\n");
    }

    #ifdef STANDSTILL_HOLD_ENABLE
      standstillHold();                                           // Apply Standstill Hold functionality. Only available and makes sense for VOLTAGE or TORQUE Mode
    #endif

    #ifdef ELECTRIC_BRAKE_ENABLE
      electricBrake(speedBlend, MultipleTapBrake.b_multipleTap);  // Apply Electric Brake. Only available and makes sense for TORQUE Mode
    #endif

    // ####### LOW-PASS FILTER #######
    rateLimiter16(input1[inIdx].cmd, rate, &steerRateFixdt);
    rateLimiter16(input2[inIdx].cmd, rate, &speedRateFixdt);
    filtLowPass32(steerRateFixdt >> 4, FILTER, &steerFixdt);
    filtLowPass32(speedRateFixdt >> 4, FILTER, &speedFixdt);
    steer = (int16_t)(steerFixdt >> 16);  // convert fixed-point to integer
    speed = (int16_t)(speedFixdt >> 16);  // convert fixed-point to integer

    mixerFcn(speed << 4, steer << 4, &cmdR, &cmdL);   // This function implements the equations above

    // ####### SET OUTPUTS (if the target change is less than +/- 100) #######
    #ifdef INVERT_R_DIRECTION
      pwmr = cmdR;
    #else
      pwmr = -cmdR;
    #endif
    #ifdef INVERT_L_DIRECTION
      pwml = -cmdL;
    #else
      pwml = cmdL;
    #endif

    // ####### CALC BOARD TEMPERATURE #######
    filtLowPass32(adc_buffer.temp, TEMP_FILT_COEF, &board_temp_adcFixdt);
    board_temp_adcFilt  = (int16_t)(board_temp_adcFixdt >> 16);  // convert fixed-point to integer
    board_temp_deg_c    = (TEMP_CAL_HIGH_DEG_C - TEMP_CAL_LOW_DEG_C) * (board_temp_adcFilt - TEMP_CAL_LOW_ADC) / (TEMP_CAL_HIGH_ADC - TEMP_CAL_LOW_ADC) + TEMP_CAL_LOW_DEG_C;

    // ####### CALC CALIBRATED BATTERY VOLTAGE #######
    batVoltageCalib = batVoltage * BAT_CALIB_REAL_VOLTAGE / BAT_CALIB_ADC;

    // ####### CALC DC LINK CURRENT #######
    left_dc_curr  = -(rtU_Left.i_DCLink * 100) / A2BIT_CONV;   // Left DC Link Current * 100 
    right_dc_curr = -(rtU_Right.i_DCLink * 100) / A2BIT_CONV;  // Right DC Link Current * 100
    dc_curr       = left_dc_curr + right_dc_curr;            // Total DC Link Current * 100

    // ####### DEBUG SERIAL OUT #######
    if (main_loop_counter % 25 == 0) {    // Send data periodically every 125 ms      
      #if defined(DEBUG_SERIAL_PROTOCOL)
        process_debug();
      #else
        printf("in1:%i in2:%i cmdL:%i cmdR:%i BatADC:%i BatV:%i TempADC:%i Temp:%i \r\n",
          input1[inIdx].raw,        // 1: INPUT1
          input2[inIdx].raw,        // 2: INPUT2
          cmdL,                     // 3: output command: [-1000, 1000]
          cmdR,                     // 4: output command: [-1000, 1000]
          adc_buffer.batt1,         // 5: for battery voltage calibration
          batVoltageCalib,          // 6: for verifying battery voltage calibration
          board_temp_adcFilt,       // 7: for board temperature calibration
          board_temp_deg_c);        // 8: for verifying board temperature calibration
      #endif
    }

    // ####### FEEDBACK SERIAL OUT #######
    if (main_loop_counter % 2 == 0) {    // Send data periodically every 10 ms
      Feedback.start	        = (uint16_t)SERIAL_START_FRAME;
      Feedback.cmd1           = (int16_t)input1[inIdx].cmd;
      Feedback.cmd2           = (int16_t)input2[inIdx].cmd;
      Feedback.speedR_meas	  = (int16_t)rtY_Right.n_mot;
      Feedback.speedL_meas	  = (int16_t)rtY_Left.n_mot;
      Feedback.batVoltage	    = (int16_t)batVoltageCalib;
      Feedback.boardTemp	    = (int16_t)board_temp_deg_c;

      if(__HAL_DMA_GET_COUNTER(huart2.hdmatx) == 0) {
        Feedback.cmdLed     = (uint16_t)sideboard_leds_L;
        Feedback.checksum   = (uint16_t)(Feedback.start ^ Feedback.cmd1 ^ Feedback.cmd2 ^ Feedback.speedR_meas ^ Feedback.speedL_meas 
                                        ^ Feedback.batVoltage ^ Feedback.boardTemp ^ Feedback.cmdLed);

        HAL_UART_Transmit_DMA(&huart2, (uint8_t *)&Feedback, sizeof(Feedback));
      }
    }

    // ####### POWEROFF BY POWER-BUTTON #######
    // poweroffPressCheck();

    // ####### BEEP AND EMERGENCY POWEROFF #######
    if (TEMP_POWEROFF_ENABLE && board_temp_deg_c >= TEMP_POWEROFF && speedAvgAbs < 20){  // poweroff before mainboard burns OR low bat 3
      printf("Powering off, temperature is too high\r\n");
      poweroff();
    } else if ( BAT_DEAD_ENABLE && batVoltage < BAT_DEAD && speedAvgAbs < 20){
      printf("Powering off, battery voltage is too low\r\n");
      poweroff();
    } else if (rtY_Left.z_errCode || rtY_Right.z_errCode) {                                           // 1 beep (low pitch): Motor error, disable motors
      enable = 0;
      beepCount(1, 24, 1);
    } else if (timeoutFlgADC) {                                                                       // 2 beeps (low pitch): ADC timeout
      beepCount(2, 24, 1);
    } else if (timeoutFlgSerial) {                                                                    // 3 beeps (low pitch): Serial timeout
      beepCount(3, 24, 1);
    } else if (timeoutFlgGen) {                                                                       // 4 beeps (low pitch): General timeout (PPM, PWM, Nunchuk)
      beepCount(4, 24, 1);
    } else if (TEMP_WARNING_ENABLE && board_temp_deg_c >= TEMP_WARNING) {                             // 5 beeps (low pitch): Mainboard temperature warning
      beepCount(5, 24, 1);
    } else if (BAT_LVL1_ENABLE && batVoltage < BAT_LVL1) {                                            // 1 beep fast (medium pitch): Low bat 1
      beepCount(0, 10, 6);
    } else if (BAT_LVL2_ENABLE && batVoltage < BAT_LVL2) {                                            // 1 beep slow (medium pitch): Low bat 2
      beepCount(0, 10, 30);
    } else if (BEEPS_BACKWARD && (((cmdR < -50 || cmdL < -50) && speedAvg < 0) || MultipleTapBrake.b_multipleTap)) { // 1 beep fast (high pitch): Backward spinning motors
      beepCount(0, 5, 1);
      backwardDrive = 1;
    } else {  // do not beep
      beepCount(0, 0, 0);
      backwardDrive = 0;
    }

    // HAL_GPIO_TogglePin(LED_PORT, LED_PIN);                 // This is to measure the main() loop duration with an oscilloscope connected to LED_PIN
    // Update states
    inIdx_prev = inIdx;
    buzzerTimer_prev = buzzerTimer;
    main_loop_counter++;
    }
  }
}


// ===========================================================
/** System Clock Configuration
*/
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL16;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_ClkInitStruct.ClockType           = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource        = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider       = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider      = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider      = RCC_HCLK_DIV1;

  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);

  PeriphClkInit.PeriphClockSelection    = RCC_PERIPHCLK_ADC;
  // PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV8;  // 8 MHz
  PeriphClkInit.AdcClockSelection       = RCC_ADCPCLK2_DIV4;  // 16 MHz
  HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

  /**Configure the Systick interrupt time
    */
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);

  /**Configure the Systick
    */
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}
