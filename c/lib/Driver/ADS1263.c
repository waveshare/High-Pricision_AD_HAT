/*****************************************************************************
* | File        :   ADS1263.c
* | Author      :   Waveshare team
* | Function    :   ADS1263 driver
* | Info        :
*----------------
* | This version:   V1.0
* | Date        :   2020-10-28
* | Info        :
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "ADS1263.h"

#include <sys/time.h>

UBYTE ScanMode = 0;

#define ADS1263_POLL_SLEEP_US 50U

static void ADS1263_PollSleep(void)
{
    usleep(ADS1263_POLL_SLEEP_US);
}

static uint64_t ADS1263_GetTimeUs(void)
{
    struct timeval now;

    if(gettimeofday(&now, NULL) != 0) {
        return 0U;
    }

    return ((uint64_t)now.tv_sec * 1000000ULL) + (uint64_t)now.tv_usec;
}

static UDOUBLE ADS1263_ClampElapsedUs(uint64_t elapsed_us)
{
    if(elapsed_us > (uint64_t)0xffffffffU) {
        return (UDOUBLE)0xffffffffU;
    }

    return (UDOUBLE)elapsed_us;
}

static int ADS1263_HasTimedOut(uint64_t start_time_us, UDOUBLE timeout_ms, UDOUBLE *elapsed_us)
{
    uint64_t now_us = ADS1263_GetTimeUs();
    uint64_t elapsed = now_us >= start_time_us ? now_us - start_time_us : 0U;

    if(elapsed_us != NULL) {
        *elapsed_us = ADS1263_ClampElapsedUs(elapsed);
    }

    return elapsed >= ((uint64_t)timeout_ms * 1000ULL);
}

/******************************************************************************
function:   Module reset
parameter:
Info:
******************************************************************************/
static void ADS1263_reset(void)
{
    DEV_Digital_Write(DEV_RST_PIN, 1);
    DEV_Delay_ms(300);
    DEV_Digital_Write(DEV_RST_PIN, 0);
    DEV_Delay_ms(300);
    DEV_Digital_Write(DEV_RST_PIN, 1);
    DEV_Delay_ms(300);
}

/******************************************************************************
function:   send command
parameter: 
        Cmd: command
Info:
******************************************************************************/
static void ADS1263_WriteCmd(UBYTE Cmd)
{
    DEV_Digital_Write(DEV_CS_PIN, 0);
    DEV_SPI_WriteByte(Cmd);
    DEV_Digital_Write(DEV_CS_PIN, 1);
}

/******************************************************************************
function:   Write a data to the destination register
parameter: 
        Reg : Target register
        data: Written data
Info:
******************************************************************************/
static void ADS1263_WriteReg(UBYTE Reg, UBYTE data)
{
    DEV_Digital_Write(DEV_CS_PIN, 0);
    DEV_SPI_WriteByte(CMD_WREG | Reg);
    DEV_SPI_WriteByte(0x00);
    DEV_SPI_WriteByte(data);
    DEV_Digital_Write(DEV_CS_PIN, 1);
}

/******************************************************************************
function:   Read a data from the destination register
parameter: 
        Reg : Target register
Info:
    Return the read data
******************************************************************************/
static UBYTE ADS1263_Read_data(UBYTE Reg)
{
    UBYTE temp = 0;
    DEV_Digital_Write(DEV_CS_PIN, 0);
    DEV_SPI_WriteByte(CMD_RREG | Reg);
    DEV_SPI_WriteByte(0x00);
    // DEV_Delay_ms(1);
    temp = DEV_SPI_ReadByte();
    DEV_Digital_Write(DEV_CS_PIN, 1);
    return temp;
}

/******************************************************************************
function:   Check data
parameter: 
        val : 4 bytes(ADC2 is 3 bytes) data
        byt : CRC byte
Info:
        Check success, return 0
******************************************************************************/
static UBYTE ADS1263_Checksum(UDOUBLE val, UBYTE byt)
{
    UBYTE sum = 0;
    UBYTE mask = -1;        // 8 bits mask, 0xff
    while(val) {
        sum += val & mask;  // only add the lower values
        val >>= 8;          // shift down
    }
    sum += 0x9b;
    // printf("--- %x %x--- \r\n", sum, byt);
    return sum ^ byt;       // if equal, this will be 0
}

/******************************************************************************
function:   Waiting for a busy end
parameter: 
Info:
    Timeout indicates that the operation is not working properly.
******************************************************************************/
static void ADS1263_WaitDRDY(void)
{
    // printf("ADS1263_WaitDRDY \r\n");
    UDOUBLE i = 0;
    while(1) {
        if(DEV_Digital_Read(DEV_DRDY_PIN) == 0)
            break;
        if(i >= 4000000) {
            printf("Time Out ...\r\n"); 
            break;
        }
    }   
    // printf("ADS1263_WaitDRDY Release \r\n");
}

/*
 * Optional DRDY wait hook. Strong implementations may use a libgpiod
 * edge-event wait so the host does not have to poll the DRDY pin.
 *
 * Return value contract (matches src/ads1263_drdy_event.h):
 *   0  = falling edge observed (DRDY went LOW)
 *   1  = shutdown wakeup; caller must abort the current read
 *   2  = no event within timeout_us; outer deadline check keeps ownership
 *  -1  = error
 *
 * The default weak implementation falls back to a short cooperative sleep so
 * builds without a strong override (PC host, missing libgpiod) keep working
 * exactly like the original poll loop.
 */
__attribute__((weak)) int ADS1263_DrdyWaitOnce(uint32_t timeout_us);

__attribute__((weak)) int ADS1263_DrdyWaitOnce(uint32_t timeout_us)
{
    (void)timeout_us;
    ADS1263_PollSleep();
    return 2;
}

static ADS1263_CHANNEL_STATUS ADS1263_WaitDRDY_Timeout(
    UDOUBLE timeout_ms,
    UDOUBLE *wait_time_us)
{
    uint64_t start_time_us = ADS1263_GetTimeUs();

    if(wait_time_us != NULL) {
        *wait_time_us = 0U;
    }

    while(DEV_Digital_Read(DEV_DRDY_PIN) != 0) {
        UDOUBLE elapsed_us = 0U;
        uint64_t now_us;
        uint64_t total_us;
        uint32_t remaining_us;
        int hook_result;

        now_us = ADS1263_GetTimeUs();
        total_us = (uint64_t)timeout_ms * 1000ULL;
        if((now_us - start_time_us) >= total_us) {
            (void)ADS1263_HasTimedOut(start_time_us, timeout_ms, wait_time_us);
            return ADS1263_CHANNEL_STATUS_DRDY_TIMEOUT;
        }
        remaining_us = (uint32_t)(total_us - (now_us - start_time_us));

        hook_result = ADS1263_DrdyWaitOnce(remaining_us);
        if(hook_result == 1) {
            (void)ADS1263_HasTimedOut(start_time_us, timeout_ms, &elapsed_us);
            if(wait_time_us != NULL) {
                *wait_time_us = elapsed_us;
            }
            return ADS1263_CHANNEL_STATUS_SHUTDOWN;
        }
        if(hook_result < 0) {
            (void)ADS1263_HasTimedOut(start_time_us, timeout_ms, wait_time_us);
            return ADS1263_CHANNEL_STATUS_DRDY_TIMEOUT;
        }
        /* hook_result 0 (edge) or 2 (no event yet) — re-check pin level. */
        if(ADS1263_HasTimedOut(start_time_us, timeout_ms, wait_time_us)) {
            return ADS1263_CHANNEL_STATUS_DRDY_TIMEOUT;
        }
    }

    if(wait_time_us != NULL) {
        (void)ADS1263_HasTimedOut(start_time_us, timeout_ms, wait_time_us);
    }

    return ADS1263_CHANNEL_STATUS_OK;
}

/******************************************************************************
function:  Read device ID
parameter: 
Info:
******************************************************************************/
UBYTE ADS1263_ReadChipID(void)
{
    UBYTE id;
    id = ADS1263_Read_data(REG_ID);
    return id>>5;
}

/******************************************************************************
function:  Setting mode
parameter: 
    Mode : 0 Single-ended input
           1 channel1 Differential input
Info:
******************************************************************************/
void ADS1263_SetMode(UBYTE Mode)
{
    if(Mode == 0) {
        ScanMode = 0;
    }else {
        ScanMode = 1;
    }
}

/******************************************************************************
function:  Configure ADC gain and sampling speed
parameter: 
    gain : Enumeration type gain
    drate: Enumeration type sampling speed
Info:
******************************************************************************/
static void ADS1263_ConfigADC1(ADS1263_GAIN gain, ADS1263_DRATE drate, ADS1263_DELAY delay,
    ADS1263_ADC1_FILTER filter, ADS1263_ADC1_REF reference)
{
    UBYTE MODE2 = 0x80;             //0x80:PGA bypassed, 0x00:PGA enabled
    MODE2 |= (gain << 4) | drate;
    ADS1263_WriteReg(REG_MODE2, MODE2);
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_MODE2) == MODE2)
        printf("REG_MODE2 success \r\n");
    else
        printf("REG_MODE2 unsuccess \r\n");
    
    UBYTE REFMUX = (UBYTE)reference;
    ADS1263_WriteReg(REG_REFMUX, REFMUX);
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_REFMUX) == REFMUX)
        printf("REG_REFMUX success \r\n");
    else
        printf("REG_REFMUX unsuccess \r\n");
    
    UBYTE MODE0 = delay;
    ADS1263_WriteReg(REG_MODE0, MODE0); 
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_MODE0) == MODE0)
        printf("REG_MODE0 success \r\n");
    else
        printf("REG_MODE0 unsuccess \r\n");
    
    UBYTE MODE1 = filter;
    ADS1263_WriteReg(REG_MODE1, MODE1); 
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_MODE1) == MODE1)
        printf("REG_MODE1 success \r\n");
    else
        printf("REG_MODE1 unsuccess \r\n");
}

/******************************************************************************
function:  Configure ADC gain and sampling speed
parameter: 
    gain : Enumeration type gain
    drate: Enumeration type sampling speed
Info:
******************************************************************************/
static void ADS1263_ConfigADC2(ADS1263_ADC2_GAIN gain, ADS1263_ADC2_DRATE drate, ADS1263_DELAY delay)
{
    UBYTE ADC2CFG = 0x20;               //REF, 0x20:VAVDD and VAVSS, 0x00:+-2.5V
    ADC2CFG |= (drate << 6) | gain;
    ADS1263_WriteReg(REG_ADC2CFG, ADC2CFG);
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_ADC2CFG) == ADC2CFG)
        printf("REG_ADC2CFG success \r\n");
    else
        printf("REG_ADC2CFG unsuccess \r\n");
    
    UBYTE MODE0 = delay;
    ADS1263_WriteReg(REG_MODE0, MODE0); 
    DEV_Delay_ms(1);
    if(ADS1263_Read_data(REG_MODE0) == MODE0)
        printf("REG_MODE0 success \r\n");
    else
        printf("REG_MODE0 unsuccess \r\n");
}

/******************************************************************************
function:  Device initialization
parameter: 
Info:
******************************************************************************/
UBYTE ADS1263_init_ADC1(ADS1263_DRATE rate)
{
    return ADS1263_init_ADC1_Filter_Ref(
        rate,
        ADS1263_FILTER_FIR,
        ADS1263_REF_AVDD_AVSS,
        ADS1263_DELAY_8d7us);
}

UBYTE ADS1263_init_ADC1_Filter(ADS1263_DRATE rate, ADS1263_ADC1_FILTER filter)
{
    return ADS1263_init_ADC1_Filter_Ref(
        rate,
        filter,
        ADS1263_REF_AVDD_AVSS,
        ADS1263_DELAY_8d7us);
}

UBYTE ADS1263_init_ADC1_Filter_Ref(
    ADS1263_DRATE rate,
    ADS1263_ADC1_FILTER filter,
    ADS1263_ADC1_REF reference,
    ADS1263_DELAY delay)
{
    ADS1263_reset();
    if(ADS1263_ReadChipID() == 1) {
        printf("ID Read success \r\n");
    }
    else {
        printf("ID Read failed \r\n");
        return 1;
    }
    ADS1263_WriteCmd(CMD_STOP1);
    ADS1263_ConfigADC1(ADS1263_GAIN_1, rate, delay, filter, reference);
    ADS1263_WriteCmd(CMD_START1);
    return 0;
}
UBYTE ADS1263_init_ADC2(ADS1263_ADC2_DRATE rate)
{
    ADS1263_reset();
    if(ADS1263_ReadChipID() == 1) {
        printf("ID Read success \r\n");
    }
    else {
        printf("ID Read failed \r\n");
        return 1;
    }
    ADS1263_WriteCmd(CMD_STOP2);
    ADS1263_ConfigADC2(ADS1263_ADC2_GAIN_1, rate, ADS1263_DELAY_35us);
    return 0;
}

/******************************************************************************
function:  Set the channel to be read
parameter: 
    Channal : Set channel number
Info:
******************************************************************************/
static void ADS1263_SetChannal(UBYTE Channal)
{
    if(Channal > 10) {
        return ;
    }
    UBYTE INPMUX = (Channal << 4) | 0x0a;       //0x0a:VCOM as Negative Input
    ADS1263_WriteReg(REG_INPMUX, INPMUX);
    if(ADS1263_Read_data(REG_INPMUX) == INPMUX) {
        // printf("ADS1263_ADC1_SetChannal success \r\n");
    } else {
        printf("ADS1263_ADC1_SetChannal unsuccess \r\n");
    }
}

/******************************************************************************
function:  Set the channel to be read
parameter: 
    Channal : Set channel number
Info:
******************************************************************************/
static void ADS1263_SetChannal_ADC2(UBYTE Channal)
{
    if(Channal > 10) {
        return ;
    }
    UBYTE INPMUX = (Channal << 4) | 0x0a;       //0x0a:VCOM as Negative Input
    ADS1263_WriteReg(REG_ADC2MUX, INPMUX);
    if(ADS1263_Read_data(REG_ADC2MUX) == INPMUX) {
        // printf("ADS1263_ADC2_SetChannal success \r\n");
    } else {
        printf("ADS1263_ADC2_SetChannal unsuccess \r\n");
    }
}

/******************************************************************************
function:  Set the channel to be read
parameter: 
    Channal : Set channel number
Info:
******************************************************************************/
void ADS1263_SetDiffChannal(UBYTE Channal)
{
    UBYTE INPMUX;
    if (Channal == 0) {
        INPMUX = (0<<4) | 1;    //DiffChannal   AIN0-AIN1
    } else if(Channal == 1) {
        INPMUX = (2<<4) | 3;    //DiffChannal   AIN2-AIN3
    } else if(Channal == 2) {
        INPMUX = (4<<4) | 5;    //DiffChannal   AIN4-AIN5
    } else if(Channal == 3) {
        INPMUX = (6<<4) | 7;    //DiffChannal   AIN6-AIN7
    } else if(Channal == 4) {
        INPMUX = (8<<4) | 9;    //DiffChannal   AIN8-AIN9
    }
    ADS1263_WriteReg(REG_INPMUX, INPMUX);   
    if(ADS1263_Read_data(REG_INPMUX) == INPMUX) {
        // printf("ADS1263_SetDiffChannal success \r\n");
    } else {
        printf("ADS1263_SetDiffChannal unsuccess \r\n");
    }
}

/******************************************************************************
function:  Set the channel to be read
parameter: 
    Channal : Set channel number
Info:
******************************************************************************/
void ADS1263_SetDiffChannal_ADC2(UBYTE Channal)
{
    UBYTE INPMUX;
    if (Channal == 0) {
        INPMUX = (0<<4) | 1;    //DiffChannal   AIN0-AIN1
    } else if(Channal == 1) {
        INPMUX = (2<<4) | 3;    //DiffChannal   AIN2-AIN3
    } else if(Channal == 2) {
        INPMUX = (4<<4) | 5;    //DiffChannal   AIN4-AIN5
    } else if(Channal == 3) {
        INPMUX = (6<<4) | 7;    //DiffChannal   AIN6-AIN7
    } else if(Channal == 4) {
        INPMUX = (8<<4) | 9;    //DiffChannal   AIN8-AIN9
    }
    ADS1263_WriteReg(REG_ADC2MUX, INPMUX);  
    if(ADS1263_Read_data(REG_ADC2MUX) == INPMUX) {
        // printf("ADS1263_SetDiffChannal_ADC2 success \r\n");
    } else {
        printf("ADS1263_SetDiffChannal_ADC2 unsuccess \r\n");
    }
}

/******************************************************************************
function:  Read ADC data
parameter: 
Info:
******************************************************************************/
static UDOUBLE ADS1263_Read_ADC1_Data(void)
{
    UDOUBLE read = 0;
    UBYTE buf[4] = {0, 0, 0, 0};
    UBYTE Status, CRC;
    DEV_Digital_Write(DEV_CS_PIN, 0);
    do {
        DEV_SPI_WriteByte(CMD_RDATA1);
        // DEV_Delay_ms(10);
        Status = DEV_SPI_ReadByte();
    }while((Status & 0x40) == 0);
    
    buf[0] = DEV_SPI_ReadByte();
    buf[1] = DEV_SPI_ReadByte();
    buf[2] = DEV_SPI_ReadByte();
    buf[3] = DEV_SPI_ReadByte();
    CRC = DEV_SPI_ReadByte();
    DEV_Digital_Write(DEV_CS_PIN, 1);
    read |= ((UDOUBLE)buf[0] << 24);
    read |= ((UDOUBLE)buf[1] << 16);
    read |= ((UDOUBLE)buf[2] << 8);
    read |= (UDOUBLE)buf[3];
    // printf("%x %x %x %x %x %x\r\n", Status, buf[0], buf[1], buf[2], buf[3], CRC);
    if(ADS1263_Checksum(read, CRC) != 0)
        printf("ADC1 Data read error! \r\n");
    return read;
}

static ADS1263_CHANNEL_STATUS ADS1263_Read_ADC1_Data_Timeout(
    UDOUBLE timeout_ms,
    UDOUBLE *Value)
{
    UDOUBLE read = 0;
    UBYTE buf[4] = {0, 0, 0, 0};
    UBYTE Status, CRC;
    uint64_t start_time_us;

    if(Value == NULL) {
        return ADS1263_CHANNEL_STATUS_INVALID_ARGUMENT;
    }

    DEV_Digital_Write(DEV_CS_PIN, 0);
    start_time_us = ADS1263_GetTimeUs();
    do {
        DEV_SPI_WriteByte(CMD_RDATA1);
        Status = DEV_SPI_ReadByte();
        if(ADS1263_HasTimedOut(start_time_us, timeout_ms, NULL)) {
            DEV_Digital_Write(DEV_CS_PIN, 1);
            return ADS1263_CHANNEL_STATUS_DATA_TIMEOUT;
        }
        ADS1263_PollSleep();
    }while((Status & 0x40) == 0);

    buf[0] = DEV_SPI_ReadByte();
    buf[1] = DEV_SPI_ReadByte();
    buf[2] = DEV_SPI_ReadByte();
    buf[3] = DEV_SPI_ReadByte();
    CRC = DEV_SPI_ReadByte();
    DEV_Digital_Write(DEV_CS_PIN, 1);
    read |= ((UDOUBLE)buf[0] << 24);
    read |= ((UDOUBLE)buf[1] << 16);
    read |= ((UDOUBLE)buf[2] << 8);
    read |= (UDOUBLE)buf[3];
    if(ADS1263_Checksum(read, CRC) != 0)
        printf("ADC1 Data read error! \r\n");

    *Value = read;
    return ADS1263_CHANNEL_STATUS_OK;
}

/******************************************************************************
function:  Read ADC data
parameter: 
Info:
******************************************************************************/
static UDOUBLE ADS1263_Read_ADC2_Data(void)
{
    UDOUBLE read = 0;
    UBYTE buf[4] = {0, 0, 0, 0};
    UBYTE Status, CRC;
    
    DEV_Digital_Write(DEV_CS_PIN, 0);
    do {
        DEV_SPI_WriteByte(CMD_RDATA2);
        // DEV_Delay_ms(10);
        Status = DEV_SPI_ReadByte();
    }while((Status & 0x80) == 0);
    
    buf[0] = DEV_SPI_ReadByte();
    buf[1] = DEV_SPI_ReadByte();
    buf[2] = DEV_SPI_ReadByte();
    buf[3] = DEV_SPI_ReadByte();
    CRC = DEV_SPI_ReadByte();
    DEV_Digital_Write(DEV_CS_PIN, 1);
    read |= ((UDOUBLE)buf[0] << 16);
    read |= ((UDOUBLE)buf[1] << 8);
    read |= (UDOUBLE)buf[2];
    // printf("%x %x %x %x %x\r\n", Status, buf[0], buf[1], buf[2], CRC);
    if(ADS1263_Checksum(read, CRC) != 0)
        printf("ADC2 Data read error! \r\n");
    return read;
}

/******************************************************************************
function:  Read ADC specified channel data
parameter: 
    Channel: Channel number
Info:
******************************************************************************/
UDOUBLE ADS1263_GetChannalValue(UBYTE Channel)
{
    UDOUBLE Value = 0;
    if(ScanMode == 0) {// 0  Single-ended input  10 channel1 Differential input  5 channe 
        if(Channel>10) {
            return 0;
        }
        ADS1263_SetChannal(Channel);
        // DEV_Delay_ms(2);
        // ADS1263_WriteCmd(CMD_START1);
        // DEV_Delay_ms(2);
        ADS1263_WaitDRDY();
        Value = ADS1263_Read_ADC1_Data();
    } else {
        if(Channel>4) {
            return 0;
        }
        ADS1263_SetDiffChannal(Channel);
        // DEV_Delay_ms(2);
        // ADS1263_WriteCmd(CMD_START1);
        // DEV_Delay_ms(2);
        ADS1263_WaitDRDY();
        Value = ADS1263_Read_ADC1_Data();
    }
    // printf("Get IN%d value success \r\n", Channel);
    return Value;
}

ADS1263_CHANNEL_STATUS ADS1263_GetChannalValue_Timeout(
    UBYTE Channel,
    UDOUBLE timeout_ms,
    UDOUBLE *Value,
    UDOUBLE *wait_time_us)
{
    ADS1263_CHANNEL_STATUS status;

    if(Value == NULL || timeout_ms == 0U) {
        return ADS1263_CHANNEL_STATUS_INVALID_ARGUMENT;
    }

    *Value = 0U;
    if(wait_time_us != NULL) {
        *wait_time_us = 0U;
    }

    if(ScanMode == 0) {
        if(Channel > 10) {
            return ADS1263_CHANNEL_STATUS_INVALID_ARGUMENT;
        }
        ADS1263_SetChannal(Channel);
    } else {
        if(Channel > 4) {
            return ADS1263_CHANNEL_STATUS_INVALID_ARGUMENT;
        }
        ADS1263_SetDiffChannal(Channel);
    }

    status = ADS1263_WaitDRDY_Timeout(timeout_ms, wait_time_us);
    if(status != ADS1263_CHANNEL_STATUS_OK) {
        return status;
    }

    return ADS1263_Read_ADC1_Data_Timeout(timeout_ms, Value);
}

/******************************************************************************
function:  Read ADC specified channel data
parameter: 
    Channel: Channel number
Info:
******************************************************************************/
UDOUBLE ADS1263_GetChannalValue_ADC2(UBYTE Channel)
{
    UDOUBLE Value = 0;
    if(ScanMode == 0) {// 0  Single-ended input  10 channel1 Differential input  5 channe 
        if(Channel>10) {
            return 0;
        }
        ADS1263_SetChannal_ADC2(Channel);
        // DEV_Delay_ms(2);
        ADS1263_WriteCmd(CMD_START2);
        // DEV_Delay_ms(2);
        Value = ADS1263_Read_ADC2_Data();
    } else {
        if(Channel>4) {
            return 0;
        }
        ADS1263_SetDiffChannal_ADC2(Channel);
        // DEV_Delay_ms(2);
        ADS1263_WriteCmd(CMD_START2);
        // DEV_Delay_ms(2);
        Value = ADS1263_Read_ADC2_Data();
    }
    // printf("Get IN%d value success \r\n", Channel);
    return Value;
}

/******************************************************************************
function:  Read data from all channels
parameter: 
    ADC_Value : ADC Value
Info:
******************************************************************************/
void ADS1263_GetAll(UBYTE *List, UDOUBLE *Value, int Number)
{
    UBYTE i;
    for(i = 0; i<Number; i++) {
         Value[i] = ADS1263_GetChannalValue(List[i]);
        // ADS1263_WriteCmd(CMD_STOP1);
        // DEV_Delay_ms(20);
    }
}

/******************************************************************************
function:  Read data from all channels
parameter: 
    ADC_Value : ADC Value
Info:
******************************************************************************/
void ADS1263_GetAll_ADC2(UDOUBLE *ADC_Value)
{
    UBYTE i;
    for(i = 0; i<10; i++) {
        ADC_Value[i] = ADS1263_GetChannalValue_ADC2(i);
        ADS1263_WriteCmd(CMD_STOP2);
        // DEV_Delay_ms(20);
    }
    // printf("----------Read ADC2 value success----------\r\n");
}

/******************************************************************************
function:  RTD Test function
parameter: 
    delay : Conversion delay
    gain :  Conversion gain
    drate : speed
Info:
******************************************************************************/
UDOUBLE ADS1263_RTD(ADS1263_DELAY delay, ADS1263_GAIN gain, ADS1263_DRATE drate)
{
    UDOUBLE Value;

    //MODE0 (CHOP OFF)
    UBYTE MODE0 = delay;
    ADS1263_WriteReg(REG_MODE0, MODE0);
    DEV_Delay_ms(1);
    
    //(IDACMUX) IDAC2 AINCOM,IDAC1 AIN3
    UBYTE IDACMUX = (0x0a<<4) | 0x03;
    ADS1263_WriteReg(REG_IDACMUX, IDACMUX);
    DEV_Delay_ms(1);
    
    //((IDACMAG)) IDAC2 = IDAC1 = 250uA
    UBYTE IDACMAG = (0x03<<4) | 0x03;
    ADS1263_WriteReg(REG_IDACMAG, IDACMAG);
    DEV_Delay_ms(1);
    
    UBYTE MODE2 = (gain << 4) | drate;
    ADS1263_WriteReg(REG_MODE2, MODE2);
    DEV_Delay_ms(1);
    
    //INPMUX (AINP = AIN7, AINN = AIN6)
    UBYTE INPMUX = (0x07<<4) | 0x06;
    ADS1263_WriteReg(REG_INPMUX, INPMUX);
    DEV_Delay_ms(1);
    
    // REFMUX AIN4 AIN5
    UBYTE REFMUX = (0x03<<3) | 0x03;
    ADS1263_WriteReg(REG_REFMUX, REFMUX);
    DEV_Delay_ms(1);
    
    //Read one conversion
    ADS1263_WriteCmd(CMD_START1);
    DEV_Delay_ms(10);
    ADS1263_WaitDRDY();
    Value = ADS1263_Read_ADC1_Data();
    ADS1263_WriteCmd(CMD_STOP1);

    return Value;
}

/******************************************************************************
function:  DAC Test function
parameter: 
    volt :          output volt value
    isPositive :    postive or negative
    isOpen :        open or close
Info:
******************************************************************************/
void ADS1263_DAC(ADS1263_DAC_VOLT volt, UBYTE isPositive, UBYTE isOpen)
{
    UBYTE Reg, Value;
    
    if(isPositive)
        Reg = REG_TDACP;        // IN6
    else
        Reg = REG_TDACN;        // IN7
    
    if(isOpen)
        Value = volt | 0x80; 
    else 
        Value = 0x00;
    
    ADS1263_WriteReg(Reg, Value);
}

