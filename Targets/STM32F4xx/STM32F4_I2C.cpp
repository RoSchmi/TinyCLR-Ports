// Copyright Microsoft Corporation
// Copyright Oberon microsystems, Inc
// Copyright GHI Electronics, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "STM32F4.h"

void STM32F4_I2c_StartTransaction(int32_t port_id);
void STM32F4_I2c_StopTransaction(int32_t port_id);

static const STM32F4_Gpio_Pin g_STM32F4_I2c_Scl_Pins[] = STM32F4_I2C_SCL_PINS;
static const STM32F4_Gpio_Pin g_STM32F4_I2c_Sda_Pins[] = STM32F4_I2C_SDA_PINS;

static const int TOTAL_I2C_CONTROLLERS = SIZEOF_ARRAY(g_STM32F4_I2c_Scl_Pins);

static I2C_TypeDef* g_STM32_I2c_Port[TOTAL_I2C_CONTROLLERS];

#define I2C_TRANSACTION_TIMEOUT 2000 // 2 seconds

struct STM32F4_I2c_Configuration {

    int32_t                  address;
    uint8_t                  clockRate;     // primary clock factor to generate the i2c clock
    uint8_t                  clockRate2;   // additional clock factors, if more than one is needed for the clock (optional)
};
struct STM32F4_I2c_Transaction {
    bool                        isReadTransaction;
    bool                        repeatedStart;
    bool                        isDone;

    uint8_t                     *buffer;

    size_t                      bytesToTransfer;
    size_t                      bytesTransferred;

    TinyCLR_I2c_TransferStatus  result;
};

static STM32F4_I2c_Configuration g_I2cConfiguration[TOTAL_I2C_CONTROLLERS];
static STM32F4_I2c_Transaction   *g_currentI2cTransactionAction[TOTAL_I2C_CONTROLLERS];
static STM32F4_I2c_Transaction   g_ReadI2cTransactionAction[TOTAL_I2C_CONTROLLERS];
static STM32F4_I2c_Transaction   g_WriteI2cTransactionAction[TOTAL_I2C_CONTROLLERS];

static uint8_t i2cProviderDefs[TOTAL_I2C_CONTROLLERS * sizeof(TinyCLR_I2c_Provider)];
static TinyCLR_I2c_Provider* i2cProvider[TOTAL_I2C_CONTROLLERS];
static TinyCLR_Api_Info i2cApi;

const TinyCLR_Api_Info* STM32F4_I2c_GetApi() {
    for (auto i = 0; i < TOTAL_I2C_CONTROLLERS; i++) {
        i2cProvider[i] = (TinyCLR_I2c_Provider*)(i2cProviderDefs + (i * sizeof(TinyCLR_I2c_Provider)));
        i2cProvider[i]->Parent = &i2cApi;
        i2cProvider[i]->Index = i;
        i2cProvider[i]->Acquire = &STM32F4_I2c_Acquire;
        i2cProvider[i]->Release = &STM32F4_I2c_Release;
        i2cProvider[i]->SetActiveSettings = &STM32F4_I2c_SetActiveSettings;
        i2cProvider[i]->Read = &STM32F4_I2c_Read;
        i2cProvider[i]->Write = &STM32F4_I2c_Write;
        i2cProvider[i]->WriteRead = &STM32F4_I2c_WriteRead;
    }

    i2cApi.Author = "GHI Electronics, LLC";
    i2cApi.Name = "GHIElectronics.TinyCLR.NativeApis.STM32F4.I2cProvider";
    i2cApi.Type = TinyCLR_Api_Type::I2cProvider;
    i2cApi.Version = 0;
    i2cApi.Count = TOTAL_I2C_CONTROLLERS;
    i2cApi.Implementation = (i2cApi.Count > 1) ? i2cProvider : (TinyCLR_I2c_Provider**)&i2cProviderDefs;

    if (TOTAL_I2C_CONTROLLERS > 0)
        g_STM32_I2c_Port[0] = I2C1;

    if (TOTAL_I2C_CONTROLLERS > 1)
        g_STM32_I2c_Port[1] = I2C2;

    if (TOTAL_I2C_CONTROLLERS > 2)
        g_STM32_I2c_Port[2] = I2C3;

    return &i2cApi;
}

void STM32F4_I2C_ER_Interrupt(int32_t port_id) {// Error Interrupt Handler
    INTERRUPT_STARTED_SCOPED(isr);

    g_STM32_I2c_Port[port_id]->SR1 = 0; // reset errors

    if (g_currentI2cTransactionAction[port_id] != nullptr)
        g_currentI2cTransactionAction[port_id]->result = TinyCLR_I2c_TransferStatus::SlaveAddressNotAcknowledged;

    STM32F4_I2c_StopTransaction(port_id);
}

void STM32F4_I2C_EV_Interrupt(int32_t port_id) {// Event Interrupt Handler
    INTERRUPT_STARTED_SCOPED(isr);

    auto& I2Cx = g_STM32_I2c_Port[port_id];

    STM32F4_I2c_Transaction *transaction = g_currentI2cTransactionAction[port_id];

    int todo = transaction->bytesToTransfer;
    int sr1 = I2Cx->SR1;  // read status register
    int sr2 = I2Cx->SR2;  // clear ADDR bit
    int cr1 = I2Cx->CR1;  // initial control register

    if (transaction->isReadTransaction) { // read transaction
        if (sr1 & I2C_SR1_SB) { // start bit
            if (todo == 1) {
                I2Cx->CR1 = (cr1 &= ~I2C_CR1_ACK); // last byte nack
            }
            else if (todo == 2) {
                I2Cx->CR1 = (cr1 |= I2C_CR1_POS); // prepare 2nd byte nack
            }
            uint8_t addr = g_I2cConfiguration[port_id].address << 1; // address bits
            I2Cx->DR = addr + 1; // send header byte with read bit;
        }
        else {
            if (sr1 & I2C_SR1_ADDR) { // address sent
                if (todo == 1) {
                    I2Cx->CR1 = (cr1 |= I2C_CR1_STOP); // send stop after single byte
                }
                else if (todo == 2) {
                    I2Cx->CR1 = (cr1 &= ~I2C_CR1_ACK); // last byte nack
                }
            }
            else {
                while (sr1 & I2C_SR1_RXNE) { // data available
                    if (todo == 2) { // 2 bytes remaining
                        I2Cx->CR1 = (cr1 |= I2C_CR1_STOP); // stop after last byte
                    }
                    else if (todo == 3) { // 3 bytes remaining
                        if (!(sr1 & I2C_SR1_BTF)) break; // assure 2 bytes are received
                        I2Cx->CR1 = (cr1 &= ~I2C_CR1_ACK); // last byte nack
                    }
                    uint8_t data = I2Cx->DR; // read data
                    transaction->buffer[transaction->bytesTransferred] = data; // save data
                    transaction->bytesTransferred++;
                    transaction->bytesToTransfer = --todo; // update todo
                    sr1 = I2Cx->SR1;  // update status register copy
                }
            }
            if (todo == 1) {
                I2Cx->CR2 |= I2C_CR2_ITBUFEN; // enable I2C_SR1_RXNE interrupt
            }
        }
    }
    else { // write transaction
        if (sr1 & I2C_SR1_SB) { // start bit
            uint8_t addr = g_I2cConfiguration[port_id].address << 1; // address bits
            I2Cx->DR = addr; // send header byte with write bit;
        }
        else {
            while (todo && (sr1 & I2C_SR1_TXE)) {
                I2Cx->DR = transaction->buffer[transaction->bytesTransferred]; // next data byte;
                transaction->bytesTransferred++;
                transaction->bytesToTransfer = --todo; // update todo
                sr1 = I2Cx->SR1;  // update status register copy
            }
            if (!(sr1 & I2C_SR1_BTF)) todo++; // last byte not yet sent
        }
    }

    if (todo == 0) { // all received or all sent
        if (transaction->repeatedStart) { // start next unit
            I2Cx->CR2 &= ~I2C_CR2_ITBUFEN; // disable I2C_SR1_RXNE interrupt
            I2Cx->CR1 = I2C_CR1_PE | I2C_CR1_START | I2C_CR1_ACK; // send restart

            g_currentI2cTransactionAction[port_id] = &g_ReadI2cTransactionAction[port_id];
        }
        else {
            STM32F4_I2c_StopTransaction(port_id);
        }
    }
}

void STM32F4_I2C1_ER_Interrupt(void *param) {
    STM32F4_I2C_ER_Interrupt(0);
}

void STM32F4_I2C2_ER_Interrupt(void *param) {
    STM32F4_I2C_ER_Interrupt(1);
}

void STM32F4_I2C3_ER_Interrupt(void *param) {
    STM32F4_I2C_ER_Interrupt(2);
}

void STM32F4_I2C1_EV_Interrupt(void *param) {
    STM32F4_I2C_EV_Interrupt(0);
}

void STM32F4_I2C2_EV_Interrupt(void *param) {
    STM32F4_I2C_EV_Interrupt(1);
}

void STM32F4_I2C3_EV_Interrupt(void *param) {
    STM32F4_I2C_EV_Interrupt(2);
}

void STM32F4_I2c_StartTransaction(int32_t port_id) {
    auto& I2Cx = g_STM32_I2c_Port[port_id];

    uint32_t ccr = g_I2cConfiguration[port_id].clockRate + (g_I2cConfiguration[port_id].clockRate2 << 8);
    if (I2Cx->CCR != ccr) { // set clock rate and rise time
        uint32_t trise;
        if (ccr & I2C_CCR_FS) { // fast => 0.3ns rise time
            trise = STM32F4_APB1_CLOCK_HZ / (1000 * 3333) + 1; // PCLK1 / 3333kHz
        }
        else { // slow => 1.0ns rise time
            trise = STM32F4_APB1_CLOCK_HZ / (1000 * 1000) + 1; // PCLK1 / 1000kHz
        }
        I2Cx->CR1 = 0; // disable peripheral
        I2Cx->CCR = ccr;
        I2Cx->TRISE = trise;
    }

    I2Cx->CR1 = I2C_CR1_PE; // enable and reset special flags
    I2Cx->SR1 = 0; // reset error flags
    I2Cx->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITERREN; // enable interrupts
    I2Cx->CR1 = I2C_CR1_PE | I2C_CR1_START | I2C_CR1_ACK; // send start
}

void STM32F4_I2c_StopTransaction(int32_t port_id) {
    auto& I2Cx = g_STM32_I2c_Port[port_id];

    if (I2Cx->SR2 & I2C_SR2_BUSY && !(I2Cx->CR1 & I2C_CR1_STOP)) {
        I2Cx->CR1 |= I2C_CR1_STOP; // send stop
    }

    I2Cx->CR2 &= ~(I2C_CR2_ITBUFEN | I2C_CR2_ITEVTEN | I2C_CR2_ITERREN); // disable interrupts

    g_currentI2cTransactionAction[port_id]->isDone = true;
}

TinyCLR_Result STM32F4_I2c_Read(const TinyCLR_I2c_Provider* self, uint8_t* buffer, size_t& length, TinyCLR_I2c_TransferStatus& result) {
    int32_t timeout = I2C_TRANSACTION_TIMEOUT;
    int32_t port_id = self->Index;

    g_ReadI2cTransactionAction[port_id].isReadTransaction = true;
    g_ReadI2cTransactionAction[port_id].buffer = buffer;
    g_ReadI2cTransactionAction[port_id].bytesToTransfer = length;
    g_ReadI2cTransactionAction[port_id].isDone = false;
    g_ReadI2cTransactionAction[port_id].repeatedStart = false;
    g_ReadI2cTransactionAction[port_id].bytesTransferred = 0;

    g_currentI2cTransactionAction[port_id] = &g_ReadI2cTransactionAction[port_id];

    STM32F4_I2c_StartTransaction(port_id);

    while (g_currentI2cTransactionAction[port_id]->isDone == false && timeout > 0) {
        STM32F4_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (g_currentI2cTransactionAction[port_id]->bytesTransferred == length)
        result = TinyCLR_I2c_TransferStatus::FullTransfer;
    else if (g_currentI2cTransactionAction[port_id]->bytesTransferred < length && g_currentI2cTransactionAction[port_id]->bytesTransferred > 0)
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;

    length = g_currentI2cTransactionAction[port_id]->bytesTransferred;

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result STM32F4_I2c_Write(const TinyCLR_I2c_Provider* self, const uint8_t* buffer, size_t& length, TinyCLR_I2c_TransferStatus& result) {
    int32_t timeout = I2C_TRANSACTION_TIMEOUT;
    int32_t port_id = self->Index;

    g_WriteI2cTransactionAction[port_id].isReadTransaction = false;
    g_WriteI2cTransactionAction[port_id].buffer = (uint8_t*)buffer;
    g_WriteI2cTransactionAction[port_id].bytesToTransfer = length;
    g_WriteI2cTransactionAction[port_id].isDone = false;
    g_WriteI2cTransactionAction[port_id].repeatedStart = false;
    g_WriteI2cTransactionAction[port_id].bytesTransferred = 0;

    g_currentI2cTransactionAction[port_id] = &g_WriteI2cTransactionAction[port_id];

    STM32F4_I2c_StartTransaction(port_id);

    while (g_currentI2cTransactionAction[port_id]->isDone == false && timeout > 0) {
        STM32F4_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (g_currentI2cTransactionAction[port_id]->bytesTransferred == length)
        result = TinyCLR_I2c_TransferStatus::FullTransfer;
    else if (g_currentI2cTransactionAction[port_id]->bytesTransferred < length && g_currentI2cTransactionAction[port_id]->bytesTransferred > 0)
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;

    length = g_currentI2cTransactionAction[port_id]->bytesTransferred;

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result STM32F4_I2c_WriteRead(const TinyCLR_I2c_Provider* self, const uint8_t* writeBuffer, size_t& writeLength, uint8_t* readBuffer, size_t& readLength, TinyCLR_I2c_TransferStatus& result) {
    int32_t timeout = I2C_TRANSACTION_TIMEOUT;
    int32_t port_id = self->Index;

    g_WriteI2cTransactionAction[port_id].isReadTransaction = false;
    g_WriteI2cTransactionAction[port_id].buffer = (uint8_t*)writeBuffer;
    g_WriteI2cTransactionAction[port_id].bytesToTransfer = writeLength;
    g_WriteI2cTransactionAction[port_id].isDone = false;
    g_WriteI2cTransactionAction[port_id].repeatedStart = true;
    g_WriteI2cTransactionAction[port_id].bytesTransferred = 0;

    g_ReadI2cTransactionAction[port_id].isReadTransaction = true;
    g_ReadI2cTransactionAction[port_id].buffer = readBuffer;
    g_ReadI2cTransactionAction[port_id].bytesToTransfer = readLength;
    g_ReadI2cTransactionAction[port_id].isDone = false;
    g_ReadI2cTransactionAction[port_id].repeatedStart = false;
    g_ReadI2cTransactionAction[port_id].bytesTransferred = 0;

    g_currentI2cTransactionAction[port_id] = &g_WriteI2cTransactionAction[port_id];

    STM32F4_I2c_StartTransaction(port_id);

    while (g_currentI2cTransactionAction[port_id]->isDone == false && timeout > 0) {
        STM32F4_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (g_WriteI2cTransactionAction[port_id].bytesTransferred != writeLength) {
        writeLength = g_WriteI2cTransactionAction[port_id].bytesTransferred;
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;
    }
    else {
        readLength = g_ReadI2cTransactionAction[port_id].bytesTransferred;

        if (g_currentI2cTransactionAction[port_id]->bytesTransferred == readLength)
            result = TinyCLR_I2c_TransferStatus::FullTransfer;
        else if (g_currentI2cTransactionAction[port_id]->bytesTransferred < readLength && g_currentI2cTransactionAction[port_id]->bytesTransferred > 0)
            result = TinyCLR_I2c_TransferStatus::PartialTransfer;
    }

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result STM32F4_I2c_SetActiveSettings(const TinyCLR_I2c_Provider* self, int32_t slaveAddress, TinyCLR_I2c_BusSpeed busSpeed) {
    uint32_t rateKhz;
    uint32_t ccr;

    int32_t port_id = self->Index;

    if (busSpeed == TinyCLR_I2c_BusSpeed::FastMode)
        rateKhz = 400; // FastMode
    else if (busSpeed == TinyCLR_I2c_BusSpeed::StandardMode)
        rateKhz = 100; // StandardMode
    else
        return TinyCLR_Result::NotSupported;

    if (rateKhz <= 100) { // slow clock
        ccr = (STM32F4_APB1_CLOCK_HZ / 1000 / 2 - 1) / rateKhz + 1; // round up
        if (ccr > 0xFFF) ccr = 0xFFF; // max divider
    }
    else { // fast clock
        ccr = (STM32F4_APB1_CLOCK_HZ / 1000 / 3 - 1) / rateKhz + 1; // round up
        ccr |= 0x8000; // set fast mode (duty cycle 1:2)
    }

    g_I2cConfiguration[port_id].clockRate = (uint8_t)ccr; // low byte
    g_I2cConfiguration[port_id].clockRate2 = (uint8_t)(ccr >> 8); // high byte
    g_I2cConfiguration[port_id].address = slaveAddress;

    return TinyCLR_Result::Success;
}

TinyCLR_Result STM32F4_I2c_Acquire(const TinyCLR_I2c_Provider* self) {
    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    int32_t port_id = self->Index;

    auto& I2Cx = g_STM32_I2c_Port[port_id];

    auto& scl = g_STM32F4_I2c_Scl_Pins[port_id];
    auto& sda = g_STM32F4_I2c_Sda_Pins[port_id];

    if (!STM32F4_GpioInternal_OpenPin(sda.number) || !STM32F4_GpioInternal_OpenPin(scl.number))
        return TinyCLR_Result::SharingViolation;

    STM32F4_GpioInternal_ConfigurePin(sda.number, STM32F4_Gpio_PortMode::AlternateFunction, STM32F4_Gpio_OutputType::OpenDrain, STM32F4_Gpio_OutputSpeed::VeryHigh, STM32F4_Gpio_PullDirection::PullUp, sda.alternateFunction);
    STM32F4_GpioInternal_ConfigurePin(scl.number, STM32F4_Gpio_PortMode::AlternateFunction, STM32F4_Gpio_OutputType::OpenDrain, STM32F4_Gpio_OutputSpeed::VeryHigh, STM32F4_Gpio_PullDirection::PullUp, scl.alternateFunction);

    RCC->APB1ENR |= (port_id == 0 ? RCC_APB1ENR_I2C1EN : port_id == 1 ? RCC_APB1ENR_I2C2EN : RCC_APB1ENR_I2C3EN);

    RCC->APB1RSTR = (port_id == 0 ? RCC_APB1RSTR_I2C1RST : port_id == 1 ? RCC_APB1RSTR_I2C2RST : RCC_APB1RSTR_I2C3RST);

    switch (port_id) {
    case 0:
        STM32F4_InterruptInternal_Activate(I2C1_EV_IRQn, (uint32_t*)&STM32F4_I2C1_EV_Interrupt, 0);
        STM32F4_InterruptInternal_Activate(I2C1_ER_IRQn, (uint32_t*)&STM32F4_I2C1_ER_Interrupt, 0);
        break;

    case 1:

        STM32F4_InterruptInternal_Activate(I2C2_EV_IRQn, (uint32_t*)&STM32F4_I2C2_EV_Interrupt, 0);
        STM32F4_InterruptInternal_Activate(I2C2_ER_IRQn, (uint32_t*)&STM32F4_I2C2_ER_Interrupt, 0);
        break;

    case 2:
        STM32F4_InterruptInternal_Activate(I2C3_EV_IRQn, (uint32_t*)&STM32F4_I2C3_EV_Interrupt, 0);
        STM32F4_InterruptInternal_Activate(I2C3_ER_IRQn, (uint32_t*)&STM32F4_I2C3_ER_Interrupt, 0);
        break;
    }

    RCC->APB1RSTR = 0;

    I2Cx->CR2 = STM32F4_APB1_CLOCK_HZ / 1000000; // APB1 clock in MHz
    I2Cx->CCR = (STM32F4_APB1_CLOCK_HZ / 1000 / 2 - 1) / 100 + 1; // 100KHz
    I2Cx->TRISE = STM32F4_APB1_CLOCK_HZ / (1000 * 1000) + 1; // 1ns;
    I2Cx->OAR1 = 0x4000; // init address register

    I2Cx->CR1 = I2C_CR1_PE; // enable peripheral

    return TinyCLR_Result::Success;
}

TinyCLR_Result STM32F4_I2c_Release(const TinyCLR_I2c_Provider* self) {
    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    int32_t port_id = self->Index;

    auto& I2Cx = g_STM32_I2c_Port[port_id];
    auto& scl = g_STM32F4_I2c_Scl_Pins[port_id];
    auto& sda = g_STM32F4_I2c_Sda_Pins[port_id];

    STM32F4_InterruptInternal_Deactivate(port_id == 0 ? I2C1_EV_IRQn : port_id == 1 ? I2C2_EV_IRQn : I2C3_EV_IRQn);
    STM32F4_InterruptInternal_Deactivate(port_id == 0 ? I2C1_ER_IRQn : port_id == 1 ? I2C2_ER_IRQn : I2C3_ER_IRQn);

    I2Cx->CR1 = 0; // disable peripheral

    RCC->APB1ENR &= (port_id == 0 ? ~RCC_APB1ENR_I2C1EN : port_id == 1 ? ~RCC_APB1ENR_I2C2EN : ~RCC_APB1ENR_I2C3EN);

    STM32F4_GpioInternal_ClosePin(sda.number);
    STM32F4_GpioInternal_ClosePin(scl.number);

    return TinyCLR_Result::Success;
}

void STM32F4_I2c_Reset() {
    for (auto i = 0; i < TOTAL_I2C_CONTROLLERS; i++) {
        STM32F4_I2c_Release(i2cProvider[i]);
        g_I2cConfiguration[i].address = 0;
        g_I2cConfiguration[i].clockRate = 0;
        g_I2cConfiguration[i].clockRate2 = 0;

        g_ReadI2cTransactionAction[i].bytesToTransfer = 0;
        g_ReadI2cTransactionAction[i].bytesTransferred = 0;

        g_WriteI2cTransactionAction[i].bytesToTransfer = 0;
        g_WriteI2cTransactionAction[i].bytesTransferred = 0;
    }
}
