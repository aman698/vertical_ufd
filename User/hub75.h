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

/* Same pins as port bit masks, for GPIO_SetMode() - keep in sync with the map above */
#define HUB75_PA_PINS           (BIT0 | BIT1 | BIT12 | BIT13 | BIT14 | BIT15)  /* CLK OE G2 B1 R1 G1 */
#define HUB75_PC_PINS           (BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT5)      /* R2 B2 B A D C      */
#define HUB75_PF_PINS           (BIT15)                                         /* LAT                */

/* Init and refresh: call HUB75_ScanRoutine() continuously (one address row per call) */
void HUB75_Init(void);
void HUB75_ScanRoutine(void);

/* Frame buffer */
void HUB75_Clear(void);
void HUB75_WritePixel(uint8_t x, uint8_t y, uint8_t color);

/* Full-panel graphic */
void HUB75_DrawBigStop(uint8_t color);

#endif /* HUB75_H */
