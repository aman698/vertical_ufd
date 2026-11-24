/******************************************************************************
 * @file     main.c
 * @brief    Vertical UFD (toll plaza)
 *             Upper UFD : HUB75 RGB panel = STOP, HUB12 panel 1 = GO
 *             Lower UFD : HUB12 panel 2  = big scrolling text
 *                         HUB12 panel 3  = fare / static text (default FARE / RS250)
 *           Control   : VRS UFD protocol on UART1 (PB2 RXD, PB3 TXD, 9600 8N1)
 *           MCU       : M031LD2AE (64 KB flash / 8 KB SRAM), Eclipse + GCC
 *****************************************************************************/
#include <stdint.h>
#include <string.h>
#include "NuMicro.h"
#include "hub12.h"
#include "hub75.h"

/* ============================================================================
 * SETTINGS
 * ========================================================================== */
#define UART_BAUD           9600U
#define UART_ECHO_RX        0U      /* 1: echo every command back ("RX: ...") - debug only */
#define RX_RING_SIZE        512U    /* UART receive buffer filled by the RX interrupt (power of 2) */
#define RX_LINE_MAX         255U    /* longest command accepted */

#define SCAN_RATE_HZ        2000U                               /* 4 row groups -> 500 Hz full refresh */
#define MS_TO_TICKS(ms)     ((uint32_t)(ms) * SCAN_RATE_HZ / 1000U)
#define HEARTBEAT_MS        400U    /* LED toggle period */
#define BLINK_MS            500U    /* STOP / GO "moving" = blink period */
#define DEFAULT_SPEED       7U      /* scroll speed 1..9 (9 = fastest); 7 = 30 ms per pixel */

/* HUB12 panel positions (y of the top row) */
#define GO_Y                0U      /* panel 1: GO            */
#define P2_Y                16U     /* panel 2: scrolling text */
#define P3_Y                32U     /* panel 3: fare / static  */

#define BOLD_ADVANCE        9       /* 14x7 font: 7 px glyph + 2 px gap */
#define SMALL_ADVANCE       6       /* 5x7 scroll font: 5 px glyph + 1 px gap */

/* Text panel modes (panel 2 = t3_*, panel 3 = t4_*; names follow the old T3../T4.. commands) */
#define MODE_SCROLL         1U      /* bold 14x7 text scrolling                    */
#define MODE_SPLIT          2U      /* 5x7: static upper line + scrolling lower    */
#define MODE_LINES          3U      /* 5x7: static upper and lower lines           */
#define MODE_BIG            4U      /* bold 14x7 static text (VRS "full display")  */

#define MENU_NONE           0U
#define MENU_MAIN           1U
#define MENU_DEFAULT_MSG    2U

/* ============================================================================
 * STATE
 * ========================================================================== */
static char default_scroll_msg[64] = "WELCOME TO TOLL PLAZA";

/* Upper UFD */
static uint8_t stop_active  = 1;    /* HUB75: red STOP                        */
static uint8_t go_active    = 0;    /* HUB12 panel 1: GO                      */
static uint8_t signal_blink = 0;    /* |C|8|x|1| "moving" -> STOP / GO blink  */
static uint8_t blink_on     = 1;
static uint8_t upper_test   = 0;    /* |C|5| upper UFD all LEDs on            */
static uint8_t lower_test   = 0;    /* |C|7| lower UFD all LEDs on            */

static int16_t  scroll_x       = DMD_WIDTH;
static uint32_t scroll_step_ms = 30U;

/* Panel 2 text */
static uint8_t t3_mode = MODE_SCROLL;
static char t3_sf_text[128];
static char t3_sd_upper[64];
static char t3_sd_lower[128];
static char t3_su_text[64];
static char t3_sl_text[64];

/* Panel 3 text (static lines keep their own X / Y from the VRS command) */
static uint8_t t4_mode = MODE_LINES;
static char t4_sf_text[128];
static char t4_sd_upper[64];
static char t4_sd_lower[128];
static char t4_su_text[64];
static char t4_sl_text[64];
static char t4_big_text[64];
static uint8_t t4_su_x, t4_su_y, t4_sl_x, t4_sl_y = 8U, t4_big_x, t4_big_y;

static uint8_t menu_state = MENU_NONE;

static volatile uint32_t g_ticks;                   /* +1 every TIMER0 interrupt */

/* UART receive ring buffer: written by UART13_IRQHandler, read by the main loop */
static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint8_t  rx_overflow;

/* ============================================================================
 * SYSTEM
 * ========================================================================== */
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

    /* TIMER0 clock: display refresh tick */
    CLK_EnableModuleClock(TMR0_MODULE);
    CLK_SetModuleClock(TMR0_MODULE, CLK_CLKSEL1_TMR0SEL_HIRC, 0);

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

/* TIMER0 periodic interrupt at SCAN_RATE_HZ (48 MHz HIRC, no prescaler) */
static void Display_Timer_Start(void)
{
    TIMER0->CTL = TIMER_PERIODIC_MODE;          /* prescaler 0 */
    TIMER0->CMP = __HIRC / SCAN_RATE_HZ;        /* 24000 counts = 500 us */
    TIMER_EnableInt(TIMER0);
    NVIC_SetPriority(TMR0_IRQn, 2);             /* below UART: a received byte is never delayed */
    NVIC_EnableIRQ(TMR0_IRQn);
    TIMER_Start(TIMER0);
}

/* Panel refresh: one row group of each panel per interrupt */
void TMR0_IRQHandler(void)
{
    if (TIMER_GetIntFlag(TIMER0))
    {
        TIMER_ClearIntFlag(TIMER0);

        DMD_ScanRoutine();      /* HUB12: GO + text panels */
        HUB75_ScanRoutine();    /* HUB75: STOP panel       */
        g_ticks++;
    }
}

/* ============================================================================
 * UART1: interrupt-driven receive, blocking transmit
 * ========================================================================== */
static void UART1_Init(void)
{
    UART_Open(UART1, UART_BAUD);                /* 8N1, RX interrupt level = 1 byte */
    UART1->FIFO |= UART_FIFO_RXRST_Msk;         /* drop anything received during start-up */
    UART_ENABLE_INT(UART1, UART_INTEN_RDAIEN_Msk);
    NVIC_SetPriority(UART1_IRQn, 1);
    NVIC_EnableIRQ(UART1_IRQn);
}

/* Move every received byte from the 16-byte hardware FIFO into the ring buffer.
 * Because this runs on every byte, nothing is lost while the main loop is busy
 * drawing or sending a reply. */
void UART13_IRQHandler(void)
{
    while ((UART1->FIFOSTS & UART_FIFOSTS_RXEMPTY_Msk) == 0U)
    {
        uint8_t  c    = (uint8_t)UART1->DAT;
        uint16_t next = (uint16_t)((rx_head + 1U) & (RX_RING_SIZE - 1U));

        if (next != rx_tail)
        {
            rx_ring[rx_head] = c;
            rx_head = next;
        }
        else
        {
            rx_overflow = 1U;
        }
    }

    /* Clear receive errors (overrun, parity, framing, break - write 1 to clear)
     * so a noisy line can never leave the UART in an error state. */
    UART1->FIFOSTS = UART_FIFOSTS_RXOVIF_Msk | UART_FIFOSTS_PEF_Msk |
                     UART_FIFOSTS_FEF_Msk   | UART_FIFOSTS_BIF_Msk;
}

static uint8_t UART1_GetByte(uint8_t *c)
{
    uint16_t tail = rx_tail;

    if (tail == rx_head)
        return 0U;

    *c = rx_ring[tail];
    rx_tail = (uint16_t)((tail + 1U) & (RX_RING_SIZE - 1U));
    return 1U;
}

void UART1_SendString(const char *str)
{
    while (*str)
    {
        while (UART_GET_TX_FULL(UART1));
        UART_WRITE(UART1, *str++);
    }
}

/* ============================================================================
 * TEXT HELPERS
 * ========================================================================== */

/* Copy src into a fixed-size buffer, truncating if too long */
static void Copy_Text(char *dst, size_t dst_size, const char *src)
{
    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}
#define COPY_TEXT(dst, src)  Copy_Text((dst), sizeof(dst), (src))

/* Strip spaces at both ends (in place) */
static char *Trim(char *s)
{
    char *e;

    while (*s == ' ')
        s++;
    e = s + strlen(s);
    while ((e > s) && (e[-1] == ' '))
        *--e = '\0';
    return s;
}

/* Unsigned decimal number 0..999; returns 0 if s is empty or not all digits */
static uint8_t Parse_Number(const char *s, uint16_t *out)
{
    uint16_t v = 0;

    if (*s == '\0')
        return 0U;
    while (*s)
    {
        if ((*s < '0') || (*s > '9'))
            return 0U;
        v = (uint16_t)(v * 10U + (uint16_t)(*s - '0'));
        if (v > 999U)
            return 0U;
        s++;
    }
    *out = v;
    return 1U;
}

/* ============================================================================
 * DRAWING
 * ========================================================================== */

/* Bold 14x7 text starting at x (may be negative while scrolling) */
static void Draw_Bold_Text(int16_t x, uint8_t y, const char *text)
{
    for (; (*text != '\0') && (x < (int16_t)DMD_WIDTH); text++, x += BOLD_ADVANCE)
    {
        if (x > -7)
            DMD_DrawFont14x7Char((uint8_t)x, y, *text);
    }
}

static void Fill_Rows(uint8_t y0, uint8_t rows)
{
    uint8_t x, y;

    for (y = y0; y < (uint8_t)(y0 + rows); y++)
        for (x = 0; x < DMD_WIDTH; x++)
            DMD_WritePixel(x, y, 1);
}

static void Draw_Panel2(void)
{
    switch (t3_mode)
    {
    case MODE_SCROLL:
        Draw_Bold_Text(scroll_x, P2_Y, t3_sf_text);
        break;
    case MODE_SPLIT:
        DMD_DrawString(0, P2_Y, t3_sd_upper);
        DMD_DrawScrollString(scroll_x, P2_Y + 8U, t3_sd_lower);
        break;
    case MODE_LINES:
        DMD_DrawString(0, P2_Y, t3_su_text);
        DMD_DrawString(0, P2_Y + 8U, t3_sl_text);
        break;
    default:
        break;
    }
}

static void Draw_Panel3(void)
{
    switch (t4_mode)
    {
    case MODE_SCROLL:
        Draw_Bold_Text(scroll_x, P3_Y, t4_sf_text);
        break;
    case MODE_SPLIT:
        DMD_DrawString(0, P3_Y, t4_sd_upper);
        DMD_DrawScrollString(scroll_x, P3_Y + 8U, t4_sd_lower);
        break;
    case MODE_LINES:
        DMD_DrawString(t4_su_x, P3_Y + t4_su_y, t4_su_text);
        DMD_DrawString(t4_sl_x, P3_Y + t4_sl_y, t4_sl_text);
        break;
    case MODE_BIG:
        Draw_Bold_Text(t4_big_x, P3_Y + t4_big_y, t4_big_text);
        break;
    default:
        break;
    }
}

/* Redraw all HUB12 panels and publish the frame in one step */
static void Update_Display_Buffer(void)
{
    DMD_Clear();

    if (upper_test)
        Fill_Rows(GO_Y, DMD_TILE_HEIGHT);
    else if (go_active && blink_on)
        DMD_DrawBigGo();

    if (lower_test)
    {
        Fill_Rows(P2_Y, 2U * DMD_TILE_HEIGHT);
    }
    else
    {
        Draw_Panel2();
        Draw_Panel3();
    }

    DMD_Show();
}

/* Redraw the HUB75 panel */
static void Update_Stop_Panel(void)
{
    HUB75_Clear();

    if (upper_test)
        HUB75_Fill(HUB75_WHITE);
    else if (stop_active && blink_on)
        HUB75_DrawBigStop(HUB75_RED);

    HUB75_Show();
}

static void Refresh_All(void)
{
    Update_Stop_Panel();
    Update_Display_Buffer();
}

/* Width in pixels of the longest text that is scrolling, so it fully leaves
 * the panel before it starts again (no long blank gap for short text). */
static int16_t Scroll_Width(void)
{
    int16_t w = 0, t;

    if (t3_mode == MODE_SCROLL) { t = (int16_t)(strlen(t3_sf_text)  * BOLD_ADVANCE);  if (t > w) w = t; }
    if (t3_mode == MODE_SPLIT)  { t = (int16_t)(strlen(t3_sd_lower) * SMALL_ADVANCE); if (t > w) w = t; }
    if (t4_mode == MODE_SCROLL) { t = (int16_t)(strlen(t4_sf_text)  * BOLD_ADVANCE);  if (t > w) w = t; }
    if (t4_mode == MODE_SPLIT)  { t = (int16_t)(strlen(t4_sd_lower) * SMALL_ADVANCE); if (t > w) w = t; }
    return w;
}

/* ============================================================================
 * STATE CHANGES
 * ========================================================================== */

/* VRS speed 1..9 -> ms per scroll pixel (9 = 10 ms fastest, 1 = 90 ms slowest) */
static void Set_Speed(uint16_t speed)
{
    if (speed < 1U) speed = 1U;
    if (speed > 9U) speed = 9U;
    scroll_step_ms = 10U + (9U - speed) * 10U;
}

/* 0 = STOP (HUB75), 1 = GO (HUB12 panel 1); moving = blink */
static void Set_Signal(uint8_t go, uint8_t moving)
{
    stop_active  = (go == 0U);
    go_active    = (go != 0U);
    signal_blink = moving;
    blink_on     = 1U;
    upper_test   = 0U;
}

static void Clear_Text(void)
{
    t3_mode = MODE_SCROLL;
    t4_mode = MODE_SCROLL;
    t3_sf_text[0] = t3_sd_upper[0] = t3_sd_lower[0] = t3_su_text[0] = t3_sl_text[0] = '\0';
    t4_sf_text[0] = t4_sd_upper[0] = t4_sd_lower[0] = t4_su_text[0] = t4_sl_text[0] = '\0';
    t4_big_text[0] = '\0';
    lower_test = 0U;
}

static void Restore_Defaults(void)
{
    Set_Signal(0U, 0U);                         /* STOP on, GO off, no blink */
    lower_test = 0U;

    t3_mode = MODE_SCROLL;
    COPY_TEXT(t3_sf_text, default_scroll_msg);

    t4_mode = MODE_LINES;
    COPY_TEXT(t4_su_text, "FARE");  t4_su_x = 0U; t4_su_y = 0U;
    COPY_TEXT(t4_sl_text, "RS250"); t4_sl_x = 0U; t4_sl_y = 8U;

    Set_Speed(DEFAULT_SPEED);
    scroll_x = DMD_WIDTH;
}

/* ============================================================================
 * VRS UFD PROTOCOL   (fields separated by '|' = 0x7C, every command ends with \r)
 *   |C|4|1|X-Y-TEXT|n|8|   static 5x7 text on panel 3 (Y 0..7 = line 1, 8..15 = line 2)
 *   |C|4|1|X-Y-TEXT|n|5|   static big text on panel 3 (font 5 or 7)
 *   |C|4|2|TEXT|speed|f|   big scrolling text on panel 2, speed 1..9
 *   |C|4|2|TOP~BOTTOM|s|f| panel 2: static top line + scrolling bottom line
 *   |C|4|G| / |C|4|R|      relay on / off (no relay fitted: ACK only)
 *   |C|5|  upper UFD on    |C|6|  clear text    |C|7|  lower UFD on
 *   |C|8|0|m|  STOP        |C|8|1|m|  GO        m: 0 = static, 1 = moving (blink)
 *   VRS                    configuration menu
 *   '$' in text is drawn as the Rupee sign.
 * ========================================================================== */

/* "X-Y-TEXT" for panel 3 */
static uint8_t VRS_Static_Text(char *pos_text, uint16_t font)
{
    char *d1, *d2, *text;
    uint16_t x, y;

    d1 = strchr(pos_text, '-');
    if (d1 == NULL) return 0U;
    *d1 = '\0';
    d2 = strchr(d1 + 1, '-');
    if (d2 == NULL) return 0U;
    *d2 = '\0';

    if (!Parse_Number(Trim(pos_text), &x) || !Parse_Number(Trim(d1 + 1), &y))
        return 0U;
    if ((x >= DMD_WIDTH) || (y >= DMD_TILE_HEIGHT))
        return 0U;

    text = Trim(d2 + 1);
    lower_test = 0U;

    if ((font == 5U) || (font == 7U))           /* full display / rupee: big font */
    {
        t4_mode = MODE_BIG;
        COPY_TEXT(t4_big_text, text);
        t4_big_x = (uint8_t)x;
        t4_big_y = (uint8_t)y;
    }
    else                                        /* normal 5x7 line */
    {
        if (t4_mode != MODE_LINES)
        {
            t4_su_text[0] = '\0';
            t4_sl_text[0] = '\0';
        }
        t4_mode = MODE_LINES;
        if (y < 8U) { COPY_TEXT(t4_su_text, text); t4_su_x = (uint8_t)x; t4_su_y = (uint8_t)y; }
        else        { COPY_TEXT(t4_sl_text, text); t4_sl_x = (uint8_t)x; t4_sl_y = (uint8_t)y; }
    }
    return 1U;
}

/* Scrolling text for panel 2 */
static void VRS_Scroll_Text(char *text, uint16_t speed)
{
    char *tilde = strchr(text, '~');

    lower_test = 0U;
    if (tilde != NULL)
    {
        *tilde = '\0';
        COPY_TEXT(t3_sd_upper, Trim(text));
        COPY_TEXT(t3_sd_lower, Trim(tilde + 1));
        t3_mode = MODE_SPLIT;
    }
    else
    {
        COPY_TEXT(t3_sf_text, Trim(text));
        t3_mode = MODE_SCROLL;
    }
    Set_Speed(speed);
    scroll_x = DMD_WIDTH;
}

static uint8_t VRS_Command(char *frame)
{
    char *f[8];
    char *s = frame + 1;                        /* frame starts with '|' */
    uint8_t n = 0;
    uint16_t a, b;

    while (n < 8U)
    {
        char *bar = strchr(s, '|');
        if (bar == NULL) break;
        *bar = '\0';
        f[n++] = Trim(s);
        s = bar + 1;
    }
    if ((n < 2U) || (strcmp(f[0], "C") != 0))
        return 0U;

    if ((strcmp(f[1], "4") == 0) && (n >= 3U))
    {
        if (strcmp(f[2], "G") == 0) { UART1_SendString("ACK: RELAY ON (no relay fitted)\r\n");  return 1U; }
        if (strcmp(f[2], "R") == 0) { UART1_SendString("ACK: RELAY OFF (no relay fitted)\r\n"); return 1U; }

        if ((strcmp(f[2], "1") == 0) && (n >= 4U))          /* static text -> panel 3 */
        {
            b = 8U;
            if (n >= 6U) (void)Parse_Number(f[5], &b);
            if (!VRS_Static_Text(f[3], b))
            {
                UART1_SendString("ERR: BAD X-Y-TEXT\r\n");
                return 1U;
            }
            Update_Display_Buffer();
            UART1_SendString("ACK: TEXT\r\n");
            return 1U;
        }

        if ((strcmp(f[2], "2") == 0) && (n >= 4U))          /* scrolling text -> panel 2 */
        {
            a = DEFAULT_SPEED;
            if (n >= 5U) (void)Parse_Number(f[4], &a);
            VRS_Scroll_Text(f[3], a);
            Update_Display_Buffer();
            UART1_SendString("ACK: SCROLL\r\n");
            return 1U;
        }
        return 0U;
    }

    if (strcmp(f[1], "5") == 0)                             /* upper UFD all on */
    {
        upper_test = 1U;
        Refresh_All();
        UART1_SendString("ACK: UPPER UFD ON\r\n");
        return 1U;
    }

    if (strcmp(f[1], "6") == 0)                             /* clear text */
    {
        Clear_Text();
        upper_test = 0U;
        Refresh_All();
        UART1_SendString("ACK: CLEAR\r\n");
        return 1U;
    }

    if (strcmp(f[1], "7") == 0)                             /* lower UFD all on */
    {
        lower_test = 1U;
        Update_Display_Buffer();
        UART1_SendString("ACK: LOWER UFD ON\r\n");
        return 1U;
    }

    if ((strcmp(f[1], "8") == 0) && (n >= 3U))              /* STOP / GO */
    {
        b = 0U;
        if (!Parse_Number(f[2], &a) || (a > 1U))
            return 0U;
        if ((n >= 4U) && (!Parse_Number(f[3], &b) || (b > 1U)))
            return 0U;
        Set_Signal((uint8_t)a, (uint8_t)b);
        Refresh_All();
        UART1_SendString((a == 0U) ? "ACK: STOP\r\n" : "ACK: GO\r\n");
        return 1U;
    }

    return 0U;
}

/* VRS configuration menu */
static void Menu_Show(void)
{
    UART1_SendString("\r\nWelcome to VRS Solutions\r\n"
                     "Press 1 and enter to set module\r\n"
                     "Press 2 and enter to set default message\r\n"
                     "Press 3 and enter to set default settings\r\n");
    menu_state = MENU_MAIN;
}

static void Menu_Input(char *line)
{
    if (menu_state == MENU_DEFAULT_MSG)
    {
        COPY_TEXT(default_scroll_msg, line);
        COPY_TEXT(t3_sf_text, default_scroll_msg);
        t3_mode = MODE_SCROLL;
        lower_test = 0U;
        scroll_x = DMD_WIDTH;
        Update_Display_Buffer();
        UART1_SendString("ACK: DEFAULT MESSAGE SET (until power off)\r\n");
        menu_state = MENU_NONE;
        return;
    }

    menu_state = MENU_NONE;
    if (strcmp(line, "1") == 0)
    {
        UART1_SendString("MODULE: VERTICAL UFD - HUB75 32x16 STOP + HUB12 32x48 (GO / TEXT / FARE)\r\n");
    }
    else if (strcmp(line, "2") == 0)
    {
        UART1_SendString("Send default message and press enter:\r\n");
        menu_state = MENU_DEFAULT_MSG;
    }
    else if (strcmp(line, "3") == 0)
    {
        Restore_Defaults();
        Refresh_All();
        UART1_SendString("ACK: DEFAULT SETTINGS\r\n");
    }
    else
    {
        UART1_SendString("ERR: MENU\r\n");
    }
}

/* ============================================================================
 * OLD TEST COMMANDS (still accepted)
 *   |STOP| |S|  |GO| |G|  |MIX| |M|  |CLR| |C|
 *   |T3SF-..| |T3SD-a~b| |T3SU-..| |T3SL-..|   (panel 2)
 *   |T4SF-..| |T4SD-a~b| |T4SU-..| |T4SL-..|   (panel 3)
 * ========================================================================== */
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
    char *tilde;

    if (strcmp(prefix, "T3SF") == 0)
    {
        t3_mode = MODE_SCROLL;
        COPY_TEXT(t3_sf_text, payload);
        scroll_x = DMD_WIDTH;
    }
    else if (strcmp(prefix, "T4SF") == 0)
    {
        t4_mode = MODE_SCROLL;
        COPY_TEXT(t4_sf_text, payload);
        scroll_x = DMD_WIDTH;
    }
    else if (strcmp(prefix, "T3SD") == 0)
    {
        tilde = strchr(payload, '~');
        if (tilde) { *tilde = '\0'; COPY_TEXT(t3_sd_lower, tilde + 1); }
        COPY_TEXT(t3_sd_upper, payload);
        t3_mode = MODE_SPLIT;
        scroll_x = DMD_WIDTH;
    }
    else if (strcmp(prefix, "T4SD") == 0)
    {
        tilde = strchr(payload, '~');
        if (tilde) { *tilde = '\0'; COPY_TEXT(t4_sd_lower, tilde + 1); }
        COPY_TEXT(t4_sd_upper, payload);
        t4_mode = MODE_SPLIT;
        scroll_x = DMD_WIDTH;
    }
    else if (strcmp(prefix, "T3SU") == 0)
    {
        COPY_TEXT(t3_su_text, payload);
        t3_mode = MODE_LINES;
    }
    else if (strcmp(prefix, "T3SL") == 0)
    {
        COPY_TEXT(t3_sl_text, payload);
        t3_mode = MODE_LINES;
    }
    else if (strcmp(prefix, "T4SU") == 0)
    {
        COPY_TEXT(t4_su_text, payload);
        t4_su_x = 0U; t4_su_y = 0U;
        t4_mode = MODE_LINES;
    }
    else if (strcmp(prefix, "T4SL") == 0)
    {
        COPY_TEXT(t4_sl_text, payload);
        t4_sl_x = 0U; t4_sl_y = 8U;
        t4_mode = MODE_LINES;
    }
    else
    {
        return 0;
    }

    lower_test = 0U;
    return 1;
}

static uint8_t Legacy_Command(char *frame)
{
    if ((strcmp(frame, "|STOP|") == 0) || (strcmp(frame, "|S|") == 0))
    {
        Set_Signal(0U, 0U);
        Refresh_All();
        UART1_SendString("ACK: STOP\r\n");
        return 1U;
    }
    if ((strcmp(frame, "|GO|") == 0) || (strcmp(frame, "|G|") == 0))
    {
        Set_Signal(1U, 0U);
        Refresh_All();
        UART1_SendString("ACK: GO\r\n");
        return 1U;
    }
    if ((strcmp(frame, "|MIX|") == 0) || (strcmp(frame, "|M|") == 0))
    {
        Restore_Defaults();
        Refresh_All();
        UART1_SendString("ACK: MIX\r\n");
        return 1U;
    }
    if ((strcmp(frame, "|CLR|") == 0) || (strcmp(frame, "|C|") == 0))
    {
        Set_Signal(0U, 0U);
        stop_active = 0U;
        Clear_Text();
        Refresh_All();
        UART1_SendString("ACK: CLR\r\n");
        return 1U;
    }
    if ((strncmp(frame, "|T", 2) == 0) && Parse_Custom_Command(frame))
    {
        Update_Display_Buffer();
        UART1_SendString("ACK: CUSTOM CMD OK\r\n");
        return 1U;
    }
    return 0U;
}

/* ============================================================================
 * COMMAND DISPATCH
 * ========================================================================== */
static void Send_Error(const char *msg, const char *frame)
{
    UART1_SendString(msg);
    UART1_SendString(frame);
    UART1_SendString("\r\n");
}

static void Process_Line(char *line)
{
    char *frame;
    size_t len;

    line = Trim(line);
    if (*line == '\0')
        return;

    /* Configuration menu answers have no '|' */
    if ((menu_state != MENU_NONE) && (strchr(line, '|') == NULL))
    {
        Menu_Input(line);
        return;
    }
    menu_state = MENU_NONE;                     /* any '|' frame leaves the menu */

    if ((strcmp(line, "VRS") == 0) || (strcmp(line, "vrs") == 0))
    {
        Menu_Show();
        return;
    }

    /* Resync: ignore noise / partial bytes before the first '|' */
    frame = strchr(line, '|');
    len = (frame != NULL) ? strlen(frame) : 0U;
    if ((frame == NULL) || (len < 2U) || (frame[len - 1U] != '|'))
    {
        Send_Error("ERR: INVALID FRAME FORMAT: ", line);
        return;
    }

#if UART_ECHO_RX
    UART1_SendString("RX: ");
    UART1_SendString(frame);
    UART1_SendString("\r\n");
#endif

    {
        /* The parsers split the frame in place: keep a copy for the error message */
        static char copy[RX_LINE_MAX + 1U];
        COPY_TEXT(copy, frame);
        if (Legacy_Command(frame) || VRS_Command(frame))
            return;
        Send_Error("ERR: UNKNOWN: ", copy);
    }
}

/* ============================================================================
 * MAIN
 * ========================================================================== */
int main(void)
{
    static char rx_line[RX_LINE_MAX + 1U];
    uint16_t rx_len = 0;
    uint8_t  rx_discard = 0;
    uint8_t  c;
    uint32_t heartbeat_tick = 0;
    uint32_t scroll_tick = 0;
    uint32_t blink_tick = 0;

    SYS_Init();
    LED_Init();
    DMD_Init();
    HUB75_Init();

    Restore_Defaults();
    Refresh_All();

    Display_Timer_Start();      /* panels refresh from TMR0_IRQHandler from here on */
    UART1_Init();               /* bytes are received by UART13_IRQHandler from here on */

    UART1_SendString("\r\nTOLL PLAZA UFD SYSTEM READY\r\n");

    while (1)
    {
        /* 1. MATRIX SCAN ROUTINE: runs in TMR0_IRQHandler at SCAN_RATE_HZ */

        /* 2. HEARTBEAT LED BLINK */
        if ((uint32_t)(g_ticks - heartbeat_tick) >= MS_TO_TICKS(HEARTBEAT_MS))
        {
            heartbeat_tick = g_ticks;
            PF5 ^= 1;
        }

        /* 3. UART COMMANDS: one line per CR / LF */
        while (UART1_GetByte(&c))
        {
            if ((c == '\r') || (c == '\n'))
            {
                if (rx_discard)
                {
                    UART1_SendString("ERR: COMMAND TOO LONG OR RX OVERFLOW\r\n");
                }
                else if (rx_len > 0U)
                {
                    rx_line[rx_len] = '\0';
                    Process_Line(rx_line);
                }
                rx_len = 0U;
                rx_discard = 0U;
            }
            else if ((c >= 32U) && (c <= 126U))
            {
                if (rx_len < RX_LINE_MAX)
                    rx_line[rx_len++] = (char)c;
                else
                    rx_discard = 1U;            /* drop the rest of this line */
            }
            /* other control bytes are ignored */
        }
        if (rx_overflow)
        {
            rx_overflow = 0U;
            rx_discard = 1U;                    /* bytes were lost: drop the line in progress */
        }

        /* 4. STOP / GO BLINK ("moving") */
        if (signal_blink)
        {
            if ((uint32_t)(g_ticks - blink_tick) >= MS_TO_TICKS(BLINK_MS))
            {
                blink_tick = g_ticks;
                blink_on ^= 1U;
                Refresh_All();
            }
        }
        else
        {
            blink_on = 1U;
        }

        /* 5. SCROLLING TICKER */
        if ((uint32_t)(g_ticks - scroll_tick) >= MS_TO_TICKS(scroll_step_ms))
        {
            scroll_tick = g_ticks;
            scroll_x--;
            if (scroll_x < -Scroll_Width())
                scroll_x = DMD_WIDTH;
            Update_Display_Buffer();
        }
    }
}
