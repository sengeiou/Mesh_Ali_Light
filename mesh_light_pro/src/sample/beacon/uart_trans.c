#include <stdio.h>
#include <stdlib.h>
#include <uart_trans.h>
#include <os_msg.h>
#include <trace.h>
#include <app_msg.h>
#include <board.h>
#include <rtl876x.h>
#include <beacon_app.h>
#include <rtl876x_nvic.h>
#include <rtl876x_uart.h>
#include <rtl876x_rcc.h>
#include <rtl876x_pinmux.h>
#include <rtl876x_gpio.h>

static void *h_event_q;
static void *h_io_q;
uint8_t rx_char[42] = {0};
bool fifo_full = false;

int data_uart_send_char(int ch)
{
    UART_SendData(UART, (uint8_t *)&ch, 1);
    /* wait tx fifo empty */
    while (UART_GetFlagState(UART, UART_FLAG_THR_TSR_EMPTY) != SET);

    return ch;
}

int data_uart_vsprintf(char *buf, const char *fmt, const int *dp)
{
    char *p, *s;

    s = buf;
    for (; *fmt != '\0'; ++fmt)
    {
        if (*fmt != '%')
        {
            buf ? *s++ = *fmt : data_uart_send_char(*fmt);
            continue;
        }
        if (*++fmt == 's')
        {
            for (p = (char *)*dp++; *p != '\0'; p++)
            {
                buf ? *s++ = *p : data_uart_send_char(*p);
            }
        }
        else    /* Length of item is bounded */
        {
            char tmp[20], *q = tmp;
            int shift = 28;

            if ((*fmt  >= '0') && (*fmt  <= '9'))
            {
                int width;
                unsigned char fch = *fmt;
                for (width = 0; (fch >= '0') && (fch <= '9'); fch = *++fmt)
                {
                    width = width * 10 + fch - '0';
                }
                shift = (width - 1) * 4;
            }
            /*
             * Before each format q points to tmp buffer
             * After each format q points past end of item
             */

            if ((*fmt == 'x') || (*fmt == 'X') || (*fmt == 'p') || (*fmt == 'P'))
            {
                /* With x86 gcc, sizeof(long) == sizeof(int) */
                const long *lp = (const long *)dp;
                long h = *lp++;
                int ncase = (*fmt & 0x20);
                int alt = 0;

                dp = (const int *)lp;
                if ((*fmt == 'p') || (*fmt == 'P'))
                {
                    alt = 1;
                }
                if (alt)
                {
                    *q++ = '0';
                    *q++ = 'X' | ncase;
                }
                for (; shift >= 0; shift -= 4)
                {
                    * q++ = "0123456789ABCDEF"[(h >> shift) & 0xF] | ncase;
                }
            }
            else if (*fmt == 'd')
            {
                int i = *dp++;
                char *r;
                if (i < 0)
                {
                    *q++ = '-';
                    i = -i;
                }
                p = q;      /* save beginning of digits */
                do
                {
                    *q++ = '0' + (i % 10);
                    i /= 10;
                }
                while (i);
                /* reverse digits, stop in middle */
                r = q;      /* don't alter q */
                while (--r > p)
                {
                    i = *r;
                    *r = *p;
                    *p++ = i;
                }
            }
            else if (*fmt == 'c')
            {
                *q++ = *dp++;
            }
            else
            {
                *q++ = *fmt;
            }
            /* now output the saved string */
            for (p = tmp; p < q; ++p)
            {
                buf ? *s++ = *p : data_uart_send_char(*p);
            }
        }
    }
    if (buf)
    {
        *s = '\0';
    }
    return (s - buf);
}

/**
 * @brief  Print the trace information through data uart.
 * @param[in] fmt   Print parameters.
 * @return void
 *
 * <b>Example usage</b>
 * \code{.c}
    void test(void)
    {
        data_uart_print("GAP scan stop\r\n");
    }
 * \endcode
 */
void data_uart_print(char *fmt, ...)
{
    (void)data_uart_vsprintf(0, fmt, ((const int *)&fmt) + 1);
}

/****************************************************************************/
/* UART interrupt                                                           */
/****************************************************************************/
void UART0_Handler(void)
{
    UART_INTConfig(UART, UART_INT_ID_RX_TMEOUT, DISABLE);
    T_IO_MSG io_driver_msg_send;
    uint8_t event  = EVENT_IO_TO_APP;
    io_driver_msg_send.type = IO_MSG_TYPE_UART;
    uint32_t interrupt_id = 0;
    uint8_t length = 0;
    /* read interrupt id */
    interrupt_id = UART_GetIID(UART);

    /* disable interrupt */
    UART_INTConfig(UART, UART_INT_RD_AVA | UART_INT_LINE_STS, DISABLE);

    switch (interrupt_id)
    {
    /* tx fifo empty, not enable */
    case UART_INT_ID_TX_EMPTY:
        APP_PRINT_INFO0("Empty INT.");
        break;

    /* rx data valiable */
    case UART_INT_ID_RX_LEVEL_REACH:
        UART_ReceiveData(UART, &rx_char[0], 24);
        io_driver_msg_send.subtype = 24;
        fifo_full = true;
        break;

    case UART_INT_ID_RX_TMEOUT:
        length = UART_GetRxFIFOLen(UART);
        io_driver_msg_send.subtype = length;
        if (fifo_full)
        {
            UART_ReceiveData(UART, &rx_char[24], length);
            io_driver_msg_send.subtype = length + 24;
        }
        else
        {
            UART_ReceiveData(UART, &rx_char[0], length);
            io_driver_msg_send.subtype = length;
        }
        if (os_msg_send(h_io_q, &io_driver_msg_send, 0) == false)
        {
        }
        else if (os_msg_send(h_event_q, &event, 0) == false)
        {
        }
        fifo_full = false;
        break;

    /* receive line status interrupt */
    case UART_INT_ID_LINE_STATUS:
        {
            //DBG_DIRECT("Line status error!!!!\n");
        }
        break;

    case UART_INT_ID_MODEM_STATUS:
        break;

    default:
        break;
    }

    /* enable interrupt again */
    UART_INTConfig(UART, UART_INT_RD_AVA, ENABLE);

    return;
}

/**
 * @brief  Initializes the Data UART.
 *
 * When data uart receives data, data uart will send an event IO_UART_MSG_TYPE to evt_queue_handle and send the data to io_queue_handle.
 * @param[in] event_queue_handle   Event queue handle which is created by APP.
 * @param[in] io_queue_handle      IO message queue handle which is created by APP.
 * @return void
 *
 * <b>Example usage</b>
 * \code{.c}
    void app_main_task(void *p_param)
    {
        char event;

        os_msg_queue_create(&io_queue_handle, MAX_NUMBER_OF_IO_MESSAGE, sizeof(T_IO_MSG));
        os_msg_queue_create(&evt_queue_handle, MAX_NUMBER_OF_EVENT_MESSAGE, sizeof(unsigned char));

        gap_start_bt_stack(evt_queue_handle, io_queue_handle, MAX_NUMBER_OF_GAP_MESSAGE);

        data_uart_init(evt_queue_handle, io_queue_handle);
        ......
    }
    void app_handle_io_msg(T_IO_MSG io_msg)
    {
        uint16_t msg_type = io_msg.type;
        uint8_t rx_char;

        switch (msg_type)
        {
        case IO_MSG_TYPE_UART:
            // We handle user command informations from Data UART in this branch.
            rx_char = (uint8_t)io_msg.subtype;
            user_cmd_collect(&user_cmd_if, &rx_char, sizeof(rx_char), user_cmd_table);
            break;
        default:
            break;
        }
    }
 * \endcode
 */
void data_uart_init(void *event_queue_handle, void *io_queue_handle)
{

    h_event_q = event_queue_handle;
    h_io_q = io_queue_handle;

    RCC_PeriphClockCmd(APBPeriph_UART0, APBPeriph_UART0_CLOCK, ENABLE);
    Pinmux_Config(DATA_UART_TX_PIN, UART0_TX);
    Pinmux_Config(DATA_UART_RX_PIN, UART0_RX);
    Pad_Config(DATA_UART_TX_PIN, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_NONE, PAD_OUT_ENABLE,
               PAD_OUT_HIGH);
    Pad_Config(DATA_UART_RX_PIN, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE,
               PAD_OUT_LOW);

    /* uart init */
    UART_InitTypeDef uartInitStruct;
    UART_StructInit(&uartInitStruct);
    uartInitStruct.rxTriggerLevel = 24;
    uartInitStruct.div = 20;
    uartInitStruct.ovsr = 12;
    uartInitStruct.ovsr_adj = 0x252;
    uartInitStruct.wordLen = UART_WROD_LENGTH_8BIT;
    uartInitStruct.idle_time = UART_RX_IDLE_4BYTE;
    UART_Init(UART, &uartInitStruct);
    UART_INTConfig(UART, UART_INT_RD_AVA, ENABLE);

    /*  Enable UART IRQ  */
    NVIC_InitTypeDef nvic_init_struct;
    nvic_init_struct.NVIC_IRQChannel         = UART0_IRQn;
    nvic_init_struct.NVIC_IRQChannelCmd      = ENABLE;
    nvic_init_struct.NVIC_IRQChannelPriority = 5;
    NVIC_Init(&nvic_init_struct);

    //data_uart_print("Line status error!!!!\n");
}

void data_ctrl_init(void)
{
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    Pinmux_Config(Int_Pin, DWGPIO);
    Pad_Config(Int_Pin, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);

    GPIO_InitTypeDef Gpio_Struct;
    GPIO_StructInit(&Gpio_Struct);

    Gpio_Struct.GPIO_Pin = GPIO_Int_Pin;
    Gpio_Struct.GPIO_Mode = GPIO_Mode_IN;
    Gpio_Struct.GPIO_ITCmd = ENABLE;
    Gpio_Struct.GPIO_ITTrigger = GPIO_INT_Trigger_EDGE;
    Gpio_Struct.GPIO_ITPolarity = GPIO_INT_POLARITY_ACTIVE_LOW;
    Gpio_Struct.GPIO_ITDebounce = GPIO_INT_DEBOUNCE_ENABLE;
    Gpio_Struct.GPIO_DebounceTime = 20;
    GPIO_Init(&Gpio_Struct);
    GPIO_MaskINTConfig(GPIO_Int_Pin, DISABLE);
    GPIO_INTConfig(GPIO_Int_Pin, ENABLE);

    NVIC_InitTypeDef NVIC_InitStruct;
    NVIC_InitStruct.NVIC_IRQChannel = GPIO_Int_Pin_IRQ;
    NVIC_InitStruct.NVIC_IRQChannelPriority = 3;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}
