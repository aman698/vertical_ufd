/******************************************************************************
 * @file     hub12.h
 * @brief    HUB12 (P10, 1/4 scan) LED matrix driver for M031
 *****************************************************************************/
#ifndef HUB12_H
#define HUB12_H

#include <stdint.h>
#include "NuMicro.h"

/* ---------------------------------------------------------------------------
 * Panel size: three 32 x 16 HUB12 P10 modules chained = 32 x 48 pixels
 *   Tile 1 : y  0..15   GO graphic   (panel connected to the MCU)
 *   Tile 2 : y 16..31   text         (T3.. commands)
 *   Tile 3 : y 32..47   text         (T4.. commands, last panel in chain)
 * ------------------------------------------------------------------------- */
#define DMD_TILE_WIDTH          32U
#define DMD_TILE_HEIGHT         16U
#define DMD_TILES               3U
#define DMD_WIDTH               DMD_TILE_WIDTH
#define DMD_HEIGHT              (DMD_TILE_HEIGHT * DMD_TILES)

/* HUB12 P10 is a 1/4-scan panel: A/B select one of four row groups. */
#define DMD_SCAN_ROWS           4U
#define DMD_GROUPS_PER_TILE     4U
#define DMD_BYTES_PER_GROUP     (DMD_TILE_WIDTH / 8U)
#define DMD_BYTES_PER_TILE      (DMD_GROUPS_PER_TILE * DMD_BYTES_PER_GROUP)

/* Pin map from the supplied schematic. */
#define DMD_OE_PORT             PA
#define DMD_OE_PIN              BIT2
#define DMD_A_PORT              PA
#define DMD_A_PIN               BIT3
#define DMD_B_PORT              PA
#define DMD_B_PIN               BIT4
#define DMD_CLK_PORT            PA
#define DMD_CLK_PIN             BIT6       /* HUB12 CLK: PA6 */
#define DMD_LAT_PORT            PA
#define DMD_LAT_PIN             BIT7       /* HUB12 SCLK/LAT: PA7 */
#define DMD_DATA_PORT           PF
#define DMD_DATA_PIN            BIT2       /* HUB12 R/DATA, pin 12 */

/* Frame buffer: one byte per pixel, 1 = LED on */
extern volatile uint8_t display_buffer[DMD_HEIGHT][DMD_WIDTH];

/* Init and refresh: call DMD_ScanRoutine() continuously (one row group per call) */
void DMD_Init(void);
void DMD_ScanRoutine(void);

/* Frame buffer */
void DMD_Clear(void);
void DMD_WritePixel(uint8_t x, uint8_t y, uint8_t state);

/* 5x7 font */
void DMD_DrawChar(uint8_t x, uint8_t y, char c);
void DMD_DrawString(uint8_t x, uint8_t y, const char *text);
void DMD_DrawScrollString(int16_t start_x, uint8_t y, const char *text);

/* 14x7 bold font */
void DMD_DrawFont14x7Char(uint8_t x, uint8_t y, char c);
void DMD_DrawFont14x7String(uint8_t x, uint8_t y, const char *text);

/* Full-tile graphic: GO on Tile 1 */
void DMD_DrawBigGo(void);

#endif /* HUB12_H */
