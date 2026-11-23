/******************************************************************************
 * @file     hub75.h
 * @brief    HUB75 RGB LED panel driver for M031
 *           Panel : RGB32x16_S4_VitaliyDKZ (32 x 16, 1/4 scan, binary A/B mux)
 *           Pixel order ported from DMD_STM32: pattern 102, pixbase 1, LPS
 *****************************************************************************/
#ifndef HUB75_H
#define HUB75_H

#include <stdint.h>
#include "NuMicro.h"

/* ---------------------------------------------------------------------------
 * Panel geometry
 *   y 0..7  -> upper half, driven by R1/G1/B1
 *   y 8..15 -> lower half, driven by R2/G2/B2
 *   1/4 scan: address A/B = 0..3 lights rows r and r+4 of each half together
 * ------------------------------------------------------------------------- */
#define HUB75_WIDTH             32U
#define HUB75_HEIGHT            16U
#define HUB75_SCAN_ROWS         4U                                      /* address 0..3      */
#define HUB75_HALF_HEIGHT       (HUB75_HEIGHT / 2U)                     /* 8 rows per half   */
#define HUB75_ROWS_PER_SCAN     (HUB75_HALF_HEIGHT / HUB75_SCAN_ROWS)   /* 2 rows per half   */
#define HUB75_CLOCKS_PER_ROW    (HUB75_WIDTH * HUB75_ROWS_PER_SCAN)     /* 64 clocks per row */

/* Colours: 1 bit per channel (bit0 = R, bit1 = G, bit2 = B) */
#define HUB75_BLACK             0U
#define HUB75_RED               1U
#define HUB75_GREEN             2U
#define HUB75_YELLOW            3U
#define HUB75_BLUE              4U
#define HUB75_MAGENTA           5U
#define HUB75_CYAN              6U
#define HUB75_WHITE             7U

/* Pin map from the supplied schematic (GPIO pin data registers) */
#define HUB75_R1                PA14
#define HUB75_G1                PA15
#define HUB75_B1                PA13
#define HUB75_R2                PC0
#define HUB75_G2                PA12
#define HUB75_B2                PC1
#define HUB75_ADDR_A            PC3
#define HUB75_ADDR_B            PC2
#define HUB75_ADDR_C            PC5        /* not used by 1/4 scan, held low */
#define HUB75_ADDR_D            PC4        /* not used by 1/4 scan, held low */
#define HUB75_LAT               PF15
#define HUB75_OE                PA1        /* active low: 0 = LEDs on */
#define HUB75_CLK               PA0

/* LED driver chip: FM6124 (same init as FM6126A / ICN2038S). These chips power up
 * with their outputs disabled, so a register init is sent first - without it the
 * panel stays completely dark. Set to 0 for plain shift-register drivers. */
#define HUB75_DRIVER_FM6124     1

/* Row select type (A/B/C/D):
 *   HUB75_MUX_BINARY : A/B carry the row number 0..3 into a 74HC138-type decoder
 *   HUB75_MUX_DIRECT : A/B/C/D each switch one row directly, active LOW
 * RGB32x16_S4_VitaliyDKZ is BINARY. If the panel has no '138' decoder and the image
 * shows several rows lit at once, change this to HUB75_MUX_DIRECT. */
#define HUB75_MUX_BINARY        0
#define HUB75_MUX_DIRECT        1
#define HUB75_MUX_TYPE          HUB75_MUX_BINARY

/* Same pins as port bit masks, for GPIO_SetMode() - keep in sync with the map above */
#define HUB75_PA_PINS           (BIT0 | BIT1 | BIT12 | BIT13 | BIT14 | BIT15)  /* CLK OE G2 B1 R1 G1 */
#define HUB75_PC_PINS           (BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT5)      /* R2 B2 B A D C      */
#define HUB75_PF_PINS           (BIT15)                                         /* LAT                */

/* Init and refresh:
 *   HUB75_ScanRoutine() outputs one address row. Call it at a fixed rate from a
 *   timer interrupt so every row gets the same on-time (no flicker). */
void HUB75_Init(void);
void HUB75_ScanRoutine(void);

/* Frame buffer (double buffered):
 *   all drawing goes to a back buffer; HUB75_Show() copies it to the panel in one step */
void HUB75_Clear(void);
void HUB75_WritePixel(uint8_t x, uint8_t y, uint8_t color);
void HUB75_Fill(uint8_t color);
void HUB75_Show(void);

/* Full-panel graphic */
void HUB75_DrawBigStop(uint8_t color);

/* Wiring test - pause the scan and drive the HUB75 pins by hand:
 *   HUB75_TestPause(1)   stop scanning (pins keep what the test sets), 0 = resume
 *   HUB75_TestPins(n)    n = 0: all LOW, 1..13: only pin n HIGH, 14: all HIGH
 *   HUB75_TestPinName(n) "R1  = PA14 -> HUB75 pin 1" ... for n = 1..13 */
#define HUB75_TEST_PINS         13U
void HUB75_TestPause(uint8_t pause);
void HUB75_TestPins(uint8_t n);
const char *HUB75_TestPinName(uint8_t n);

#endif /* HUB75_H */
