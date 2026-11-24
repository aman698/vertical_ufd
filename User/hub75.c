/******************************************************************************
 * @file     hub75.c
 * @brief    HUB75 RGB LED panel driver for M031
 *           Panel : P10 4S RGB, 32 x 16, 1/4 scan, FM6124 drivers, binary A/B row select
 *           1-bit colour per channel (8 colours), scanned from the timer interrupt.
 *****************************************************************************/
#include <string.h>
#include "hub75.h"

/* Shift data already in panel order: [address row][clock].
 * Each byte holds the 6 data lines for one CLK pulse:
 *   bit0 R1, bit1 G1, bit2 B1   (upper half, y 0..7)
 *   bit3 R2, bit4 G2, bit5 B2   (lower half, y 8..15)
 * Double buffered: drawing goes to hub75_back (main loop), the scan reads
 * hub75_front (timer interrupt), HUB75_Show() copies back -> front. */
static uint8_t hub75_back[HUB75_SCAN_ROWS][HUB75_CLOCKS_PER_ROW];
static volatile uint8_t hub75_front[HUB75_SCAN_ROWS][HUB75_CLOCKS_PER_ROW];
static volatile uint8_t hub75_row;

/* Kept as a variable (not a constant) so the delay loop compiles exactly as it
 * did when HUB75_CLK_DELAY = 10 was tested on the hardware. */
static volatile uint8_t hub75_clk_delay = HUB75_CLK_DELAY;

static const char * const stop_art[16] = {
    "                                ",
    " XXXXXXX XXXXXXX XXXXXXX XXXXXXX",
    " XXXXXXX XXXXXXX XXXXXXX XXXXXXX",
    " XX        XXX   XX   XX XX   XX",
    " XX        XXX   XX   XX XX   XX",
    " XX        XXX   XX   XX XX   XX",
    " XX        XXX   XX   XX XX   XX",
    " XXXXXXX   XXX   XX   XX XXXXXXX",
    " XXXXXXX   XXX   XX   XX XXXXXXX",
    "      XX   XXX   XX   XX XX     ",
    "      XX   XXX   XX   XX XX     ",
    "      XX   XXX   XX   XX XX     ",
    "      XX   XXX   XX   XX XX     ",
    " XXXXXXX   XXX   XXXXXXX XX     ",
    " XXXXXXX   XXX   XXXXXXX XX     ",
    "                                "
};

/* Short busy wait for the shift clock */
static inline void HUB75_Delay(uint8_t n)
{
    while (n--)
    {
        __NOP();
    }
}

#if HUB75_DRIVER_FM6124
/* FM6124 / FM6126A / ICN2038S configuration registers. Values and sequence as used
 * by the ESP32-HUB75-MatrixPanel-DMA library (fm6124init):
 *   REG1 - output current / brightness, written with LAT high for the last 11 clocks
 *   REG2 - bit 9 enables the LED outputs, written with LAT high for the last 12 clocks */
static const uint8_t fm6124_reg1[16] = {0,0,0,0,0, 1,1,1,1,1,1, 0,0,0,0,0};
static const uint8_t fm6124_reg2[16] = {0,0,0,0,0, 0,0,0,0,1,0, 0,0,0,0,0};

static void HUB75_InitDelay(void)
{
    volatile uint32_t n = 24U;              /* ~1-2 us at 48 MHz, init only */
    while (n--) { }
}

static void HUB75_InitClock(void)
{
    HUB75_CLK = 1; HUB75_InitDelay();
    HUB75_CLK = 0; HUB75_InitDelay();
}

static void HUB75_InitAllData(uint8_t v)
{
    HUB75_R1 = v; HUB75_G1 = v; HUB75_B1 = v;
    HUB75_R2 = v; HUB75_G2 = v; HUB75_B2 = v;
}

/* Shift a 16-bit register value into every chip; LAT high during the last 'latches' clocks */
static void HUB75_WriteRegister(const uint8_t *reg, uint8_t latches)
{
    uint8_t l;

    for (l = 0; l < HUB75_CLOCKS_PER_ROW; l++)
    {
        HUB75_InitAllData(reg[l % 16U]);
        if (l >= (uint8_t)(HUB75_CLOCKS_PER_ROW - latches))
            HUB75_LAT = 1;
        HUB75_InitClock();
    }
    HUB75_LAT = 0;
}

static void HUB75_DriverInit(void)
{
    uint8_t l;

    HUB75_OE = 1;                           /* outputs off during init */
    HUB75_WriteRegister(fm6124_reg1, 11U);
    HUB75_WriteRegister(fm6124_reg2, 12U);

    /* Blank the shift registers and latch the blank data */
    HUB75_InitAllData(0);
    for (l = 0; l < HUB75_CLOCKS_PER_ROW; l++)
        HUB75_InitClock();
    HUB75_LAT = 1;
    HUB75_InitClock();
    HUB75_LAT = 0;
    /* OE stays high: HUB75_ScanRoutine() turns the LEDs on after the first real row */
}
#endif

void HUB75_Init(void)
{
    /* Make sure every HUB75 pin is plain GPIO (MFP = 0) */
    SYS->GPA_MFPL &= ~(SYS_GPA_MFPL_PA0MFP_Msk  | SYS_GPA_MFPL_PA1MFP_Msk);
    SYS->GPA_MFPH &= ~(SYS_GPA_MFPH_PA12MFP_Msk | SYS_GPA_MFPH_PA13MFP_Msk |
                       SYS_GPA_MFPH_PA14MFP_Msk | SYS_GPA_MFPH_PA15MFP_Msk);
    SYS->GPC_MFPL &= ~(SYS_GPC_MFPL_PC0MFP_Msk  | SYS_GPC_MFPL_PC1MFP_Msk  |
                       SYS_GPC_MFPL_PC2MFP_Msk  | SYS_GPC_MFPL_PC3MFP_Msk  |
                       SYS_GPC_MFPL_PC4MFP_Msk  | SYS_GPC_MFPL_PC5MFP_Msk);
    SYS->GPF_MFPH &= ~(SYS_GPF_MFPH_PF15MFP_Msk);

    GPIO_SetMode(PA, HUB75_PA_PINS, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PC, HUB75_PC_PINS, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PF, HUB75_PF_PINS, GPIO_MODE_OUTPUT);

    HUB75_OE  = 1;                     /* LEDs off */
    HUB75_LAT = 0;
    HUB75_CLK = 0;
    HUB75_ADDR_A = 0; HUB75_ADDR_B = 0;
    HUB75_ADDR_C = 0; HUB75_ADDR_D = 0;
    HUB75_R1 = 0; HUB75_G1 = 0; HUB75_B1 = 0;
    HUB75_R2 = 0; HUB75_G2 = 0; HUB75_B2 = 0;

#if HUB75_DRIVER_FM6124
    HUB75_DriverInit();                     /* enable FM6124 outputs */
#endif

    HUB75_Clear();
    HUB75_Show();
    hub75_row = 0;
}

void HUB75_Clear(void)
{
    memset(hub75_back, 0, sizeof(hub75_back));
}

/* Whole panel in one colour (used for the upper-UFD full-on test) */
void HUB75_Fill(uint8_t color)
{
    color &= 0x07U;
    memset(hub75_back, (int)(color | (color << 3)), sizeof(hub75_back));
}

/* Publish the back buffer to the panel (256 bytes, a few microseconds) */
void HUB75_Show(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    memcpy((void *)hub75_front, hub75_back, sizeof(hub75_back));
    __set_PRIMASK(primask);
}

/* Pixel -> shift position (DMD_STM32 pattern 102, pixbase 8, UPS):
 *   address row = (y % 8) % 4
 *   clock       = (x / 8) * 16 + x % 8       rows 0..3 of a half
 *                 (x / 8) * 16 + x % 8 + 8   rows 4..7 of a half
 * i.e. the panel takes 8 pixels of the upper row, then the same 8 of the lower row. */
void HUB75_WritePixel(uint8_t x, uint8_t y, uint8_t color)
{
    uint8_t half_y, row, clk, shift;

    if ((x >= HUB75_WIDTH) || (y >= HUB75_HEIGHT))
        return;

    half_y = (uint8_t)(y % HUB75_HALF_HEIGHT);
    row    = (uint8_t)(half_y % HUB75_SCAN_ROWS);
    clk    = (uint8_t)((x / HUB75_PIXBASE) * HUB75_ROWS_PER_SCAN * HUB75_PIXBASE + (x % HUB75_PIXBASE));
    if (half_y >= HUB75_SCAN_ROWS)
        clk = (uint8_t)(clk + HUB75_PIXBASE);
    shift  = (y < HUB75_HALF_HEIGHT) ? 0U : 3U;

    hub75_back[row][clk] = (uint8_t)((hub75_back[row][clk] & ~(0x07U << shift)) |
                                     ((color & 0x07U) << shift));
}

/* Output one address row (called from the timer interrupt):
 * LEDs off, select row, shift 64 clocks, latch, LEDs on */
void HUB75_ScanRoutine(void)
{
    const volatile uint8_t *p = hub75_front[hub75_row];
    uint8_t i, v;
    uint8_t d = hub75_clk_delay;

    HUB75_OE = 1;                                  /* LEDs off while shifting */
    HUB75_ADDR_A = hub75_row & 1U;                 /* binary row select */
    HUB75_ADDR_B = (hub75_row >> 1) & 1U;

    for (i = 0; i < HUB75_CLOCKS_PER_ROW; i++)
    {
        v = p[i];
        HUB75_R1 = v & 1U;
        HUB75_G1 = (v >> 1) & 1U;
        HUB75_B1 = (v >> 2) & 1U;
        HUB75_R2 = (v >> 3) & 1U;
        HUB75_G2 = (v >> 4) & 1U;
        HUB75_B2 = (v >> 5) & 1U;
        HUB75_Delay(d);                            /* data settles before the edge */
        HUB75_CLK = 1;                             /* data taken on rising edge */
        HUB75_Delay(d);                            /* clock high time */
        HUB75_CLK = 0;
    }

    HUB75_LAT = 1;                                 /* shift register -> LED drivers */
    HUB75_Delay(d);
    HUB75_LAT = 0;
    HUB75_OE = 0;                                  /* LEDs on */

    hub75_row = (uint8_t)((hub75_row + 1U) % HUB75_SCAN_ROWS);
}

void HUB75_DrawBigStop(uint8_t color)
{
    uint8_t x, y;
    for (y = 0; y < HUB75_HEIGHT; y++) {
        for (x = 0; x < HUB75_WIDTH; x++) {
            if (stop_art[y][x] == 'X') HUB75_WritePixel(x, y, color);
        }
    }
}
