/*******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the LIN Communication Example
*              for ModusToolbox. The same source implements both the LIN
*              master node and the LIN slave node; the role is selected at
*              compile time with the LIN_MASTER_NODE macro.
*
* Related Document: See README.md
*
*
*******************************************************************************
* (c) 2026, Infineon Technologies AG, or an affiliate of Infineon
* Technologies AG. All rights reserved.
* This software, associated documentation and materials ("Software") is
* owned by Infineon Technologies AG or one of its affiliates ("Infineon")
* and is protected by and subject to worldwide patent protection, worldwide
* copyright laws, and international treaty provisions. Therefore, you may use
* this Software only as provided in the license agreement accompanying the
* software package from which you obtained this Software. If no license
* agreement applies, then any use, reproduction, modification, translation, or
* compilation of this Software is prohibited without the express written
* permission of Infineon.
*
* Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
* IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
* THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
* SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
* Infineon reserves the right to make changes to the Software without notice.
* You are responsible for properly designing, programming, and testing the
* functionality and safety of your intended application of the Software, as
* well as complying with any legal requirements related to its use. Infineon
* does not guarantee that the Software will be free from intrusion, data theft
* or loss, or other breaches ("Security Breaches"), and Infineon shall have
* no liability arising out of any Security Breaches. Unless otherwise
* explicitly approved by Infineon, the Software may not be used in any
* application where a failure of the Product or any consequences of the use
* thereof can reasonably be expected to result in personal injury.
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cy_pdl.h"
#include "cy_lin.h"
#include "cybsp.h"
#include <string.h>

/******************************************************************************
* Macros
*******************************************************************************/
/* Node role selection:
 *      1 - build this application as the LIN master node
 *      0 - build this application as the LIN slave node
 * It can also be overridden from the Makefile, for example:
 *      DEFINES=LIN_MASTER_NODE=1 */
#ifndef LIN_MASTER_NODE
#define LIN_MASTER_NODE         (0u)
#endif

/* Board LIN transceiver is wired to Port 7 (P7.0 RX / P7.1 TX / P7.2 EN) =
 * lin[0] channel 4. Port 7 only supports LIN on channel 4, so LIN is configured
 * here in source rather than via the Device Configurator. */
#define LIN_CHANNEL             (LIN0_CH4)
#define LIN_IRQ_SRC             (lin_0_interrupts_4_IRQn)
#define LIN_NVIC_MUX            (NvicMux3_IRQn)
#define LIN_PCLK                (PCLK_LIN0_CLOCK_CH_EN4)

/* Peripheral clock divider for the LIN channel.
 * LIN oversampling is fixed at 16, so baudrate = LIN input clock / 16.
 * 80 MHz / 260 / 16 = 19231 baud (matches the reference LIN configuration
 * and the 19200 baud setting used by PLIN). */
#define LIN_CLK_DIV_TYPE        (CY_SYSCLK_DIV_16_BIT)
#define LIN_CLK_DIV_NUM         (0u)
#define LIN_CLK_DIV_VAL         (259u)

#if (LIN_MASTER_NODE)
/* SysTick reload value for the master schedule (one LIN frame sent per tick). */
#define SYSTICK_RELOAD_VAL      (8000000UL)
#endif

/*******************************************************************************
* Data Types
*******************************************************************************/
/* Port configuration */
typedef struct
{
    GPIO_PRT_Type* portReg;
    uint8_t pinNum;
    cy_stc_gpio_pin_config_t cfg;
} stc_pin_config;

#if (LIN_MASTER_NODE)
/* Master state type */
typedef enum
{
    LIN_STATE_IDLE,
    LIN_STATE_TX_HEADER,
    LIN_STATE_TX_HEADER_TX_RESPONSE,
    LIN_STATE_TX_HEADER_RX_RESPONSE
} en_lin_node_state_t;

/* LIN message direction type */
typedef enum
{
    LIN_TX_HEADER,
    LIN_RX_RESPONSE,
    LIN_TX_RESPONSE,
} lin_response_direction;
#else
/* Slave state type */
typedef enum
{
    LIN_STATE_IDLE,
    LIN_STATE_RX_HEADER,
    LIN_STATE_TX_RESPONSE,
    LIN_STATE_RX_RESPONSE
} en_lin_node_state_t;

/* LIN message direction type */
typedef enum
{
    LIN_RX_RESPONSE,
    LIN_TX_RESPONSE,
} lin_response_direction;
#endif

/* LIN message type configuration */
typedef struct
{
    uint8_t id;
    lin_response_direction responseDirection;
    cy_en_lin_checksum_type_t checksumType;
    uint8_t dataLength;
    uint8_t dataBuffer[LIN_DATA_LENGTH_MAX];
} lin_message_context;

/*******************************************************************************
* Global Variables
*******************************************************************************/
/* LIN port configuration (lin[0] channel 4 on Port 7) */
static const stc_pin_config lin_pin_cfg[] =
{
    /* LIN RX - P7.0 */
    {
        .portReg = GPIO_PRT7,
        .pinNum  = 0u,
        {
            .outVal    = 0,
            .driveMode = CY_GPIO_DM_HIGHZ,
            .hsiom     = P7_0_LIN0_LIN_RX4,
        }
    },
    /* LIN TX - P7.1 */
    {
        .portReg = GPIO_PRT7,
        .pinNum  = 1u,
        {
            .outVal    = 1,
            .driveMode = CY_GPIO_DM_STRONG,
            .hsiom     = P7_1_LIN0_LIN_TX4,
        }
    },
    /* LIN EN - P7.2 */
    {
        .portReg = GPIO_PRT7,
        .pinNum  = 2u,
        {
            .outVal    = 1,
#if (LIN_MASTER_NODE)
            .driveMode = CY_GPIO_DM_STRONG,
#else
            .driveMode = CY_GPIO_DM_PULLUP,
#endif
            .hsiom     = P7_2_LIN0_LIN_EN4,
        }
    }
};

static const cy_stc_lin_config_t lin_config =
{
#if (LIN_MASTER_NODE)
    .masterMode               = true,   /* Master mode */
    .breakFieldLength         = 13u,    /* Master break field length */
#else
    .masterMode               = false,  /* Slave mode */
    .breakFieldLength         = 11u,    /* Slave break detection threshold */
#endif
    .linTransceiverAutoEnable = true,
    .breakDelimiterLength     = LIN_BREAK_DILIMITER_LENGTH_1BITS,
    .stopBit                  = LIN_ONE_STOP_BIT,
    .filterEnable             = true
};

static const cy_stc_sysint_t lin_irq_cfg =
{
    /* On M4CPUSS v2 the interrupt source packs the NVIC mux channel (bits 16+)
     * with the system interrupt source (bits 0-15). */
    .intrSrc      = (IRQn_Type)(((uint32_t)LIN_NVIC_MUX << CY_SYSINT_INTRSRC_MUXIRQ_SHIFT) | (uint32_t)LIN_IRQ_SRC),
    .intrPriority = 3u,
};

#if (LIN_MASTER_NODE)
/* LIN schedule table. The master sends one frame per tick, cycling through it. */
lin_message_context msgContext[] =
{
    {0x01, LIN_RX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 8u,},
    {0x02, LIN_TX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 8u,},
    {0x10, LIN_RX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 1u,},
    {0x11, LIN_TX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 1u,},
    {0x20, LIN_TX_HEADER,   LIN_CHECKSUM_TYPE_EXTENDED, 8u,},
};
uint8_t scheduleIdx;
#else
lin_message_context msgContext[] =
{
    {0x01, LIN_TX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 8u,},
    {0x02, LIN_RX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 8u,},
    {0x10, LIN_TX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 1u,},
    {0x11, LIN_RX_RESPONSE, LIN_CHECKSUM_TYPE_EXTENDED, 1u,},
};
uint8_t currentMsgIdx;
#endif
en_lin_node_state_t lin_state;

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void PortInit(void);
#if (LIN_MASTER_NODE)
static void SchedulerInit(void);
static void LIN0_TickHandler(void);
#endif
void LIN0_IntHandler(void);

/*******************************************************************************
* Function Definitions
*******************************************************************************/

/*******************************************************************************
* Function Name: main
*********************************************************************************
* Summary:
* This is the main function for CPU. Depending on LIN_MASTER_NODE it runs this
* node either as a LIN master or as a LIN slave at ~19.2 kbps. It...
*    1. Initializes the device, the LIN peripheral clock and the LIN pins.
*    2. Initializes the LIN channel and its interrupt.
*    3. Master only: starts the SysTick based schedule that sends one LIN frame
*       per tick.
*
* Master behavior:
*      - Sends 5 IDs in total, one ID per schedule tick.
*      - Master-to-slave response data is copied from the slave response
*        (slave responses with ID 0x01/0x10 are copied into the master
*        responses with ID 0x02/0x11).
*
* Slave behavior:
*      - Master node sends 5 type header:
*        0x02 and 0x11 are master tx response to this node
*        0x01 and 0x10 are slave tx response to master node
*        0x20 is master tx response to other node (this node ignores it)
*      - Slave node copies rx response to tx response (RX ID - 1 = TX ID)
*
* For the message definition, refer to the msgContext array.
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* LIN baudrate setting.
     * LIN IP does oversampling with a fixed count of 16.
     * Therefore LIN baudrate = LIN input clock / 16. */
    Cy_SysClk_PeriphAssignDivider(LIN_PCLK, LIN_CLK_DIV_TYPE, LIN_CLK_DIV_NUM);
    Cy_SysClk_PeriphSetDivider(LIN_CLK_DIV_TYPE, LIN_CLK_DIV_NUM, LIN_CLK_DIV_VAL);
    Cy_SysClk_PeriphEnableDivider(LIN_CLK_DIV_TYPE, LIN_CLK_DIV_NUM);

    /* Initialize LIN pins (P7.0/P7.1/P7.2) */
    PortInit();

#if (LIN_MASTER_NODE)
    /* Register LIN interrupt handler and enable interrupt */
    Cy_SysInt_Init(&lin_irq_cfg, &LIN0_IntHandler);
    NVIC_SetPriority(LIN_NVIC_MUX, 3);
    NVIC_ClearPendingIRQ(LIN_NVIC_MUX);
    NVIC_EnableIRQ(LIN_NVIC_MUX);

    /* Initialize LIN as master */
    Cy_LIN_Init(LIN_CHANNEL, &lin_config);
    lin_state = LIN_STATE_IDLE;

    /* Start the master schedule */
    SchedulerInit();
#else
    /* Initialize LIN as slave */
    Cy_LIN_Init(LIN_CHANNEL, &lin_config);
    lin_state = LIN_STATE_IDLE;

    /* Register LIN interrupt handler and enable interrupt */
    Cy_SysInt_Init(&lin_irq_cfg, &LIN0_IntHandler);
    NVIC_SetPriority(LIN_NVIC_MUX, 3);
    NVIC_ClearPendingIRQ(LIN_NVIC_MUX);
    NVIC_EnableIRQ(LIN_NVIC_MUX);

    /* LIN operation */
    lin_state = LIN_STATE_RX_HEADER;
    Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_HEADER_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
    Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_RX_HEADER_RX_RESPONSE);
#endif

    for (;;)
    {
        /* Do nothing, all operations are handled at IRQ handler */
    }
}

/* Initialize LIN pins */
static void PortInit(void)
{
    for (uint8_t i = 0; i < (sizeof(lin_pin_cfg) / sizeof(lin_pin_cfg[0])); i++)
    {
        Cy_GPIO_Pin_Init(lin_pin_cfg[i].portReg, lin_pin_cfg[i].pinNum, &lin_pin_cfg[i].cfg);
    }
}

#if (LIN_MASTER_NODE)

/* Initialize SysTick for the master schedule */
static void SchedulerInit(void)
{
    Cy_SysTick_Init(CY_SYSTICK_CLOCK_SOURCE_CLK_CPU, SYSTICK_RELOAD_VAL);
    Cy_SysTick_SetCallback(0, LIN0_TickHandler);
    Cy_SysTick_Enable();
}

/* Master schedule handler: sends one LIN frame per tick */
static void LIN0_TickHandler(void)
{
    /* Disable the channel to clear pending status, then re-enable */
    Cy_LIN_Disable(LIN_CHANNEL);
    Cy_LIN_Enable(LIN_CHANNEL);

    switch (msgContext[scheduleIdx].responseDirection)
    {
    case LIN_TX_RESPONSE:
        /* Response direction = master to slave */
        Cy_LIN_SetDataLength(LIN_CHANNEL, msgContext[scheduleIdx].dataLength);
        Cy_LIN_SetChecksumType(LIN_CHANNEL, msgContext[scheduleIdx].checksumType);
        Cy_LIN_SetHeader(LIN_CHANNEL, msgContext[scheduleIdx].id);
        Cy_LIN_WriteData(LIN_CHANNEL, msgContext[scheduleIdx].dataBuffer, msgContext[scheduleIdx].dataLength);
        Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_TX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_MASTER);
        lin_state = LIN_STATE_TX_HEADER_TX_RESPONSE;
        Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_TX_HEADER_TX_RESPONSE);
        break;
    case LIN_RX_RESPONSE:
        /* Response direction = slave to master */
        Cy_LIN_SetDataLength(LIN_CHANNEL, msgContext[scheduleIdx].dataLength);
        Cy_LIN_SetChecksumType(LIN_CHANNEL, msgContext[scheduleIdx].checksumType);
        Cy_LIN_SetHeader(LIN_CHANNEL, msgContext[scheduleIdx].id);
        Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_MASTER);
        lin_state = LIN_STATE_TX_HEADER_RX_RESPONSE;
        Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_TX_HEADER_RX_RESPONSE);
        break;
    case LIN_TX_HEADER:
        /* Header only (response is provided by another node) */
        Cy_LIN_SetHeader(LIN_CHANNEL, msgContext[scheduleIdx].id);
        Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_TX_HEADER_DONE | LIN_INTR_ALL_ERROR_MASK_MASTER);
        lin_state = LIN_STATE_TX_HEADER;
        Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_TX_HEADER);
        break;
    default:
        break;
    }
    scheduleIdx = (scheduleIdx + 1) % (sizeof(msgContext) / sizeof(msgContext[0]));
}

/* LIN0 IRQ Handler (master) */
void LIN0_IntHandler(void)
{
    uint32_t maskStatus;
    cy_en_lin_status_t apiResponse;

    Cy_LIN_GetInterruptMaskedStatus(LIN_CHANNEL, &maskStatus);
    Cy_LIN_ClearInterrupt(LIN_CHANNEL, maskStatus);    /* Clear all accepted interrupt. */
    Cy_LIN_SetInterruptMask(LIN_CHANNEL, 0uL);         /* Disable all interrupt. */

    if ((maskStatus & LIN_INTR_ALL_ERROR_MASK_MASTER) != 0u)
    {
        /* There are some errors. Handle if needed. Wait for the next tick. */
        lin_state = LIN_STATE_IDLE;

        /* Disable the channel to reset LIN status, then re-enable */
        Cy_LIN_Disable(LIN_CHANNEL);
        Cy_LIN_Enable(LIN_CHANNEL);
    }
    else
    {
        switch (lin_state)
        {
        case LIN_STATE_TX_HEADER:
            /* Tx header complete with no error */
            break;
        case LIN_STATE_TX_HEADER_TX_RESPONSE:
            /* Tx response complete with no error */
            break;
        case LIN_STATE_TX_HEADER_RX_RESPONSE:
            /* Tx header and rx response complete with no error */
            while (1)
            {
                apiResponse = Cy_LIN_ReadData(LIN_CHANNEL, msgContext[scheduleIdx].dataBuffer);
                if (apiResponse == CY_LIN_SUCCESS)
                {
                    break;
                }
            }
            /* For testing: set rx data to tx data. Rx ID + 1 => Tx ID */
            memcpy(msgContext[scheduleIdx + 1].dataBuffer, msgContext[scheduleIdx].dataBuffer, LIN_DATA_LENGTH_MAX);
            break;
        default:
            break;
        }
        /* Wait for next tick. */
        lin_state = LIN_STATE_IDLE;
    }
}

#else /* LIN slave node */

/* LIN0 IRQ Handler (slave) */
void LIN0_IntHandler(void)
{
    uint32_t maskStatus;
    cy_en_lin_status_t apiResponse;

    Cy_LIN_GetInterruptMaskedStatus(LIN_CHANNEL, &maskStatus);
    Cy_LIN_ClearInterrupt(LIN_CHANNEL, maskStatus);    /* Clear all accepted interrupt. */

    if ((maskStatus & LIN_INTR_ALL_ERROR_MASK_SLAVE) != 0u)
    {
        /* There are some errors */
        /* Handle error if needed. */

        /* Disable the channel to reset LIN status */
        Cy_LIN_Disable(LIN_CHANNEL);
        /* Re-enable the channel */
        Cy_LIN_Enable(LIN_CHANNEL);
        /* Re-enable header RX */
        lin_state = LIN_STATE_RX_HEADER;
        Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_HEADER_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
        Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_RX_HEADER_RX_RESPONSE);
    }
    else
    {
        bool acceptedId = false;
        uint8_t id, parity;
        switch (lin_state)
        {
        case LIN_STATE_RX_HEADER:
            /* Rx header complete with no error */
            Cy_LIN_GetHeader(LIN_CHANNEL, &id, &parity);
            /* Analyze ID */
            for (uint8_t i = 0; i < (sizeof(msgContext) / sizeof(msgContext[0])); i++)
            {
                if (id == msgContext[i].id)
                {
                    currentMsgIdx = i;
                    acceptedId = true;
                    break;
                }
            }
            if (acceptedId)
            {
                /* Setup checksum type and data length */
                Cy_LIN_SetDataLength(LIN_CHANNEL, msgContext[currentMsgIdx].dataLength);
                Cy_LIN_SetChecksumType(LIN_CHANNEL, msgContext[currentMsgIdx].checksumType);
                if (msgContext[currentMsgIdx].responseDirection == LIN_TX_RESPONSE)
                {
                    Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_TX_RESPONSE_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
                    Cy_LIN_WriteData(LIN_CHANNEL, msgContext[currentMsgIdx].dataBuffer, msgContext[currentMsgIdx].dataLength);
                    lin_state = LIN_STATE_TX_RESPONSE;
                    Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_TX_RESPONSE);
                }
                else
                {
                    lin_state = LIN_STATE_RX_RESPONSE;
                }
            }
            else
            {
                /* Message to be ignored */
                /* Disable the channel to reset LIN status */
                Cy_LIN_Disable(LIN_CHANNEL);
                /* Re-enable the channel */
                Cy_LIN_Enable(LIN_CHANNEL);
                /* Re-enable header RX */
                lin_state = LIN_STATE_RX_HEADER;
                Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_HEADER_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
                Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_RX_HEADER_RX_RESPONSE);
            }
            break;
        case LIN_STATE_TX_RESPONSE:
            /* Tx response complete with no error */
            /* If RX_DONE interrupt also occurs, a response collision occurred */
            if ((maskStatus & LIN_INTR_RX_RESPONSE_DONE) != 0)
            {
                /* Data collision occurs */
                /* Do error handling if needed */
            }
            /* Re-enable header RX */
            lin_state = LIN_STATE_RX_HEADER;
            Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_HEADER_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
            Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_RX_HEADER_RX_RESPONSE);
            break;
        case LIN_STATE_RX_RESPONSE:
            /* Rx response complete with no error */
            while (1)
            {
                apiResponse = Cy_LIN_ReadData(LIN_CHANNEL, msgContext[currentMsgIdx].dataBuffer);
                if (apiResponse == CY_LIN_SUCCESS)
                {
                    break;
                }
            }
            /* For testing: set rx data to tx data. Rx ID - 1 => Tx ID */
            memcpy(msgContext[currentMsgIdx - 1].dataBuffer, msgContext[currentMsgIdx].dataBuffer, LIN_DATA_LENGTH_MAX);
            /* Re-enable header RX */
            lin_state = LIN_STATE_RX_HEADER;
            Cy_LIN_SetInterruptMask(LIN_CHANNEL, LIN_INTR_RX_HEADER_DONE | LIN_INTR_RX_RESPONSE_DONE | LIN_INTR_ALL_ERROR_MASK_SLAVE);
            Cy_LIN_SetCmd(LIN_CHANNEL, LIN_CMD_RX_HEADER_RX_RESPONSE);
            break;
        default:
            break;
        }
    }
}

#endif /* LIN_MASTER_NODE */

/* [] END OF FILE */
