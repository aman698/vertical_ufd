/******************************************************************************
 * @file     main.c
 * @brief    Vertical UFD (toll plaza) - UART1 command control of HUB12 display
 *           MCU : M031LD2AE (64 KB flash / 8 KB SRAM), Eclipse + GCC
 *****************************************************************************/
#include <stdint.h>
#include <string.h>
#include "NuMicro.h"
#include "hub12.h"
#include "hub75.h"

#define LED_PIN PF5

/* Default startup text strings */
char default_scroll_msg[64] = "WELCOME TO TOLL PLAZA";

/* HUB75 panel: red STOP (on at power-on; |STOP| turns it on, |GO| turns it off) */
static uint8_t stop_active = 1;

/* HUB12 Tile 1: GO graphic (off at power-on; |GO| turns it on, |STOP| turns it off) */
static uint8_t go_active = 0;

static int16_t scroll_x = DMD_WIDTH;
static uint32_t scroll_speed_threshold = 10UL;

/* Text panels - names follow the command prefixes:
 *   t3_* (T3.. commands) -> Tile 2, y 16..31
 *   t4_* (T4.. commands) -> Tile 3, y 32..47 */
static uint8_t t3_mode = 1; // 1 = Full Scroll (T3SF)
static uint8_t t4_mode = 3; // 3 = Static Upper/Lower (T4SU / T4SL)

static char t3_sf_text[128] = "WELCOME TO TOLL PLAZA";
static char t4_sf_text[128] = "";

static char t3_sd_upper[64] = "";
static char t3_sd_lower[128] = "";
static char t4_sd_upper[64] = "";
static char t4_sd_lower[128] = "";

static char t3_su_text[64] = "";
static char t3_sl_text[64] = "";
static char t4_su_text[64] = "FARE";   // Default Tile 4 Upper Static Text
static char t4_sl_text[64] = "RS250"; // Default Tile 4 Lower Static Text

void SYS_Init(void)
{
    SYS_UnlockReg();

    /* HIRC 48 MHz */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);

    CLK_SetHCLK(CLK_CLKSEL0_HCLKSEL_HIRC,
                CLK_CLKDIV0_HCLK(1));

    /* UART1 clock */
    CLK_EnableModuleClock(UART1_MODULE);
    CLK_SetModuleClock(UART1_MODULE,
                       CLK_CLKSEL1_UART1SEL_HIRC,
                       CLK_CLKDIV0_UART1(1));

    /* PB.2 = RX, PB.3 = TX */
    SYS->GPB_MFPL &= ~(SYS_GPB_MFPL_PB2MFP_Msk |
                       SYS_GPB_MFPL_PB3MFP_Msk);

    SYS->GPB_MFPL |= (SYS_GPB_MFPL_PB2MFP_UART1_RXD |
                      SYS_GPB_MFPL_PB3MFP_UART1_TXD);

    SYS_LockReg();
}

void LED_Init(void)
{
    GPIO_SetMode(PF, BIT5, GPIO_MODE_OUTPUT);
    PF5 = 1; /* Initially OFF */
}

void UART1_Init(void)
{
    UART_Open(UART1, 9600);
}

void UART1_SendString(const char *str)
{
    while (*str)
    {
        while (UART_GET_TX_FULL(UART1));
        UART_WRITE(UART1, *str++);
    }
}

static void Update_Display_Buffer(void)
{
    DMD_Clear();

    /* 1. TILE 1: GO DISPLAY */
    if (go_active)
    {
        DMD_DrawBigGo();
    }

    /* 2. TILE 2 RENDERING - T3 commands (Independent) */
    if (t3_mode == 1) // Full Scroll using Font14x7 Bold
    {
        int16_t cur_x = scroll_x;
        const char *p = t3_sf_text;
        while (*p) {
            if (cur_x > -7 && cur_x < (int16_t)DMD_WIDTH) {
                DMD_DrawFont14x7Char((uint8_t)cur_x, 16, *p);
            }
            cur_x += 9;
            p++;
        }
    }
    else if (t3_mode == 2) // Split (Upper Static Y=16 + Lower Scroll Y=24)
    {
        DMD_DrawString(0, 16, t3_sd_upper);
        DMD_DrawScrollString(scroll_x, 24, t3_sd_lower);
    }
    else if (t3_mode == 3) // Static Upper / Lower
    {
        if (strlen(t3_su_text) > 0) DMD_DrawString(0, 16, t3_su_text);
        if (strlen(t3_sl_text) > 0) DMD_DrawString(0, 24, t3_sl_text);
    }

    /* 3. TILE 3 RENDERING - T4 commands (Independent) */
    if (t4_mode == 1) // Full Scroll using Font14x7 Bold
    {
        int16_t cur_x = scroll_x;
        const char *p = t4_sf_text;
        while (*p) {
            if (cur_x > -7 && cur_x < (int16_t)DMD_WIDTH) {
                DMD_DrawFont14x7Char((uint8_t)cur_x, 32, *p);
            }
            cur_x += 9;
            p++;
        }
    }
    else if (t4_mode == 2) // Split (Upper Static Y=32 + Lower Scroll Y=40)
    {
        DMD_DrawString(0, 32, t4_sd_upper);
        DMD_DrawScrollString(scroll_x, 40, t4_sd_lower);
    }
    else if (t4_mode == 3) // Static Upper / Lower
    {
        if (strlen(t4_su_text) > 0) DMD_DrawString(0, 32, t4_su_text);
        if (strlen(t4_sl_text) > 0) DMD_DrawString(0, 40, t4_sl_text);
    }
}

/* Redraw the HUB75 panel - only needed when the STOP state changes */
static void Update_Stop_Panel(void)
{
    HUB75_Clear();

    if (stop_active)
    {
        HUB75_DrawBigStop(HUB75_RED);
    }
}

/* Copy src into a fixed-size buffer, truncating if too long.
 * A frame can carry up to ~250 payload chars but the text buffers are 64/128,
 * so a plain strcpy here would overwrite neighbouring variables. */
static void Copy_Text(char *dst, size_t dst_size, const char *src)
{
    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}
#define COPY_TEXT(dst, src)  Copy_Text((dst), sizeof(dst), (src))

/* Parser for format: |PREFIX-TEXT| */
static uint8_t Parse_Custom_Command(char *rx_str)
{
    char *start_pipe = strchr(rx_str, '|');
    if (!start_pipe) return 0;
    
    char *hyphen = strchr(start_pipe + 1, '-');
    if (!hyphen) return 0;
    *hyphen = '\0';
    char *prefix = start_pipe + 1; 
    
    char *end_pipe = strrchr(hyphen + 1, '|');
    if (!end_pipe) return 0;
    *end_pipe = '\0';
    char *payload = hyphen + 1; 

    if (strcmp(prefix, "T3SF") == 0)
    {
        t3_mode = 1;
        COPY_TEXT(t3_sf_text, payload);
        scroll_x = DMD_WIDTH;
        return 1;
    }
    else if (strcmp(prefix, "T4SF") == 0)
    {
        t4_mode = 1;
        COPY_TEXT(t4_sf_text, payload);
        scroll_x = DMD_WIDTH;
        return 1;
    }
    else if (strcmp(prefix, "T3SD") == 0)
    {
        char *tilde = strchr(payload, '~');
        if (tilde) {
            *tilde = '\0';
            COPY_TEXT(t3_sd_upper, payload);
            COPY_TEXT(t3_sd_lower, tilde + 1);
        } else {
            COPY_TEXT(t3_sd_upper, payload);
        }
        t3_mode = 2;
        scroll_x = DMD_WIDTH;
        return 1;
    }
    else if (strcmp(prefix, "T4SD") == 0)
    {
        char *tilde = strchr(payload, '~');
        if (tilde) {
            *tilde = '\0';
            COPY_TEXT(t4_sd_upper, payload);
            COPY_TEXT(t4_sd_lower, tilde + 1);
        } else {
            COPY_TEXT(t4_sd_upper, payload);
        }
        t4_mode = 2;
        scroll_x = DMD_WIDTH;
        return 1;
    }
    else if (strcmp(prefix, "T3SU") == 0)
    {
        COPY_TEXT(t3_su_text, payload);
        t3_mode = 3;
        return 1;
    }
    else if (strcmp(prefix, "T4SU") == 0)
    {
        COPY_TEXT(t4_su_text, payload);
        t4_mode = 3;
        return 1;
    }
    else if (strcmp(prefix, "T3SL") == 0)
    {
        COPY_TEXT(t3_sl_text, payload);
        t3_mode = 3;
        return 1;
    }
    else if (strcmp(prefix, "T4SL") == 0)
    {
        COPY_TEXT(t4_sl_text, payload);
        t4_mode = 3;
        return 1;
    }

    return 0;
}

int main(void)
{
    uint32_t heartbeat_counter = 0;
    uint32_t scroll_counter = 0;
    static char rx_fifo[256];   /* static: keeps 256 bytes off the 512-byte stack */
    uint8_t index = 0;

    SYS_Init();
    LED_Init();
    UART1_Init();
    DMD_Init();
    HUB75_Init();

    DMD_Clear();
    Update_Display_Buffer();
    Update_Stop_Panel();

    UART1_SendString("\r\nTOLL PLAZA UFD SYSTEM READY\r\n");

    while (1)
    {
        /* 1. MATRIX SCAN ROUTINE (one row of each panel type per loop) */
        DMD_ScanRoutine();      /* HUB12: GO + text tiles */
        HUB75_ScanRoutine();    /* HUB75: STOP panel      */

        /* 2. HEARTBEAT LED BLINK (~400ms tick) */
        if (++heartbeat_counter > 2500UL)
        {
            PF5 ^= 1;
            heartbeat_counter = 0;
        }

        /* 3. NON-BLOCKING UART COMMAND POLLING */
        while (!UART_GET_RX_EMPTY(UART1))
        {
            char c = (char)UART_READ(UART1);

            if (c == '\r' || c == '\n')
            {
                if (index > 0)
                {
                    rx_fifo[index] = '\0';

                    if (rx_fifo[0] == '|' && rx_fifo[index - 1] == '|')
                    {
                        UART1_SendString("\r\nRX: ");
                        UART1_SendString(rx_fifo);
                        UART1_SendString("\r\n");

                        if (strcmp(rx_fifo, "|STOP|") == 0 || strcmp(rx_fifo, "|S|") == 0)
                        {
                            stop_active = 1;
                            go_active = 0;   // Automatically clear GO from HUB12 Tile 1
                            Update_Stop_Panel();
                            Update_Display_Buffer();
                            UART1_SendString("ACK: STOP\r\n");
                        }
                        else if (strcmp(rx_fifo, "|GO|") == 0 || strcmp(rx_fifo, "|G|") == 0)
                        {
                            go_active = 1;
                            stop_active = 0; // Automatically clear STOP from HUB75 panel
                            Update_Stop_Panel();
                            Update_Display_Buffer();
                            UART1_SendString("ACK: GO\r\n");
                        }
                        else if (strcmp(rx_fifo, "|MIX|") == 0 || strcmp(rx_fifo, "|M|") == 0)
                        {
                            // Reset defaults
                            stop_active = 1;
                            go_active = 0;
                            Update_Stop_Panel();
                            t3_mode = 1;
                            t4_mode = 3;
                            strcpy(t3_sf_text, default_scroll_msg);
                            strcpy(t4_su_text, "FARE");
                            strcpy(t4_sl_text, "RS250");
                            scroll_x = DMD_WIDTH;
                            Update_Display_Buffer();
                            UART1_SendString("ACK: MIX\r\n");
                        }
                        else if (strcmp(rx_fifo, "|CLR|") == 0 || strcmp(rx_fifo, "|C|") == 0)
                        {
                            stop_active = 0;
                            go_active = 0;
                            Update_Stop_Panel();
                            t3_mode = 1;
                            t4_mode = 1;
                            memset(t3_sf_text, 0, sizeof(t3_sf_text));
                            memset(t4_sf_text, 0, sizeof(t4_sf_text));
                            memset(t3_sd_upper, 0, sizeof(t3_sd_upper));
                            memset(t3_sd_lower, 0, sizeof(t3_sd_lower));
                            memset(t4_sd_upper, 0, sizeof(t4_sd_upper));
                            memset(t4_sd_lower, 0, sizeof(t4_sd_lower));
                            memset(t3_su_text, 0, sizeof(t3_su_text));
                            memset(t3_sl_text, 0, sizeof(t3_sl_text));
                            memset(t4_su_text, 0, sizeof(t4_su_text));
                            memset(t4_sl_text, 0, sizeof(t4_sl_text));
                            DMD_Clear();
                            UART1_SendString("ACK: CLR\r\n");
                        }
                        else if (Parse_Custom_Command(rx_fifo))
                        {
                            Update_Display_Buffer();
                            UART1_SendString("ACK: CUSTOM CMD OK\r\n");
                        }
                        else
                        {
                            UART1_SendString("ERR: UNKNOWN\r\n");
                        }
                    }
                    else
                    {
                        UART1_SendString("\r\nERR: INVALID FRAME FORMAT\r\n");
                    }

                    memset(rx_fifo, 0, sizeof(rx_fifo));
                    index = 0;
                }
            }
            else if (index < sizeof(rx_fifo) - 1)
            {
                if (c >= 32 && c <= 126)
                {
                    rx_fifo[index++] = c;
                }
            }
            else
            {
                memset(rx_fifo, 0, sizeof(rx_fifo));
                index = 0;
            }
        }

        /* 4. SCROLLING TICKER CHECK */
        if (++scroll_counter > scroll_speed_threshold)
        {
            scroll_counter = 0;
            scroll_x--;

            int16_t min_x = -(int16_t)(64 * 9U);
            if (scroll_x < min_x)
            {
                scroll_x = DMD_WIDTH;
            }

            Update_Display_Buffer();
        }
    }
}
