// Copyright Microsoft Corporation
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

#include "LPC24.h"

#define PCON_REG                (*(volatile unsigned char *)0xE01FC0C0)
#define EXT_INTERRUPT_REG       (*(volatile uint32_t*)0xE01FC140)
#define EMC_DYNAMIC_CTRL_REG    (*(volatile uint32_t*)0xFFE08020)
#define EMC_STATUS_REG          (*(volatile uint32_t*)0xFFE08004)
#define EMC_SELF_REFRESH_MODE   4
#define INTERRUPT_WAKEUP_REG    (*(volatile uint32_t*)0xE01FC144)
#define GPIO_INTERRUPT          ( 1 << 7 | 1 << 8)

#define TOTAL_POWER_CONTROLLERS 1

static void(*PowerStopHandler)();
static void(*PowerRestartHandler)();

struct PowerState {
    uint32_t controllerIndex;
    bool tableInitialized;
};

const char* powerApiNames[TOTAL_POWER_CONTROLLERS] = {
    "GHIElectronics.TinyCLR.NativeApis.LPC24.PowerController\\0"
};

static TinyCLR_Power_Controller powerControllers[TOTAL_POWER_CONTROLLERS];
static TinyCLR_Api_Info powerApi[TOTAL_POWER_CONTROLLERS];
static PowerState powerStates[TOTAL_POWER_CONTROLLERS];

void LPC24_Power_EnsureTableInitialized() {
    for (auto i = 0; i < TOTAL_POWER_CONTROLLERS; i++) {
        if (powerStates[i].tableInitialized)
            continue;

        powerControllers[i].ApiInfo = &powerApi[i];
        powerControllers[i].Initialize = &LPC24_Power_Initialize;
        powerControllers[i].Uninitialize = &LPC24_Power_Uninitialize;
        powerControllers[i].Reset = &LPC24_Power_Reset;
        powerControllers[i].SetLevel = &LPC24_Power_SetLevel;

        powerApi[i].Author = "GHI Electronics, LLC";
        powerApi[i].Name = powerApiNames[i];
        powerApi[i].Type = TinyCLR_Api_Type::PowerController;
        powerApi[i].Version = 0;
        powerApi[i].Implementation = &powerControllers[i];
        powerApi[i].State = &powerStates[i];

        powerStates[i].controllerIndex = i;
        powerStates[i].tableInitialized = true;
    }
}

const TinyCLR_Api_Info* LPC24_Power_GetRequiredApi() {
    LPC24_Power_EnsureTableInitialized();

    return &powerApi[0];
}

void LPC24_Power_AddApi(const TinyCLR_Api_Manager* apiManager) {
    LPC24_Power_EnsureTableInitialized();

    for (auto i = 0; i < TOTAL_POWER_CONTROLLERS; i++) {
        apiManager->Add(apiManager, &powerApi[i]);
    }

    apiManager->SetDefaultName(apiManager, TinyCLR_Api_Type::PowerController, powerApi[0].Name);
}

void LPC24_Power_SetHandlers(void(*stop)(), void(*restart)()) {
    PowerStopHandler = stop;
    PowerRestartHandler = restart;
}

bool LPC24_Emc_IsSelfRefreshMode() {
    return (EMC_STATUS_REG & EMC_SELF_REFRESH_MODE) == EMC_SELF_REFRESH_MODE;
}

void LPC24_Emc_ClearSelfRefreshMode() {
    EMC_DYNAMIC_CTRL_REG &= ~EMC_SELF_REFRESH_MODE;
}

void LPC24_Emc_EnterSelfRefreshMode() {
    EMC_DYNAMIC_CTRL_REG |= EMC_SELF_REFRESH_MODE;
}

extern "C" void SystemInit();
TinyCLR_Result LPC24_Power_SetLevel(const TinyCLR_Power_Controller* self, TinyCLR_Power_Level level, TinyCLR_Power_WakeSource wakeSource, uint64_t data) {
    switch (level) {
    case TinyCLR_Power_Level::Sleep1: // Sleep
    case TinyCLR_Power_Level::Sleep2: // Sleep
    case TinyCLR_Power_Level::Sleep3: // Sleep
    case TinyCLR_Power_Level::Off:    // Off

        INTERRUPT_WAKEUP_REG = (uint32_t)(GPIO_INTERRUPT);

        TinyCLR_UsbClient_Uninitialize(nullptr);

        EXT_INTERRUPT_REG = EXT_INTERRUPT_REG; // clear pending interrupt

        // SDRAM enable self mode
        LPC24_Emc_EnterSelfRefreshMode();
        while (!LPC24_Emc_IsSelfRefreshMode());

        if (level == TinyCLR_Power_Level::Off)
            PCON_REG |= 6; // off
        else
            PCON_REG |= 2; // Power-down mode

        SystemInit();

        // Clear EINT flags
        EXT_INTERRUPT_REG = EXT_INTERRUPT_REG;
        INTERRUPT_WAKEUP_REG = 0;

        TinyCLR_UsbClient_Initialize(nullptr);
        break;

    case TinyCLR_Power_Level::Custom: // Custom
        //TODO
        return TinyCLR_Result::NotSupported;

    case TinyCLR_Power_Level::Idle:   // Idle
        PCON_REG |= 1;

        return TinyCLR_Result::Success;

    case TinyCLR_Power_Level::Active: // Active
    default:
        // Highest performance
        return TinyCLR_Result::Success;
    }

    return TinyCLR_Result::Success;
}

TinyCLR_Result LPC24_Power_Reset(const TinyCLR_Power_Controller* self, bool runCoreAfter) {
#if defined RAM_BOOTLOADER_HOLD_VALUE && defined RAM_BOOTLOADER_HOLD_ADDRESS && RAM_BOOTLOADER_HOLD_ADDRESS > 0
    if (!runCoreAfter) {
        //See section 1.9 of UM10211.pdf. A write-back buffer holds the last written value. Two writes guarantee it'll appear after a reset.
        *((volatile uint32_t*)RAM_BOOTLOADER_HOLD_ADDRESS) = RAM_BOOTLOADER_HOLD_VALUE;
        *((volatile uint32_t*)RAM_BOOTLOADER_HOLD_ADDRESS) = RAM_BOOTLOADER_HOLD_VALUE;
    }
#endif

    LPC24XX_WATCHDOG& WTDG = LPC24XX::WTDG();

    // disable interrupts
    DISABLE_INTERRUPTS_SCOPED(irq);
    // set the smallest value
    WTDG.WDTC = 0xFF;

    // assure its enabled (and counter is zero)
    WTDG.WDMOD = LPC24XX_WATCHDOG::WDMOD__WDEN | LPC24XX_WATCHDOG::WDMOD__WDRESET;

    WTDG.WDFEED = LPC24XX_WATCHDOG::WDFEED_reload_1;
    WTDG.WDFEED = LPC24XX_WATCHDOG::WDFEED_reload_2;

    while (1); // wait for reset

    return TinyCLR_Result::InvalidOperation;
}

TinyCLR_Result LPC24_Power_Initialize(const TinyCLR_Power_Controller* self) {
    return TinyCLR_Result::Success;
}

TinyCLR_Result LPC24_Power_Uninitialize(const TinyCLR_Power_Controller* self) {
    return TinyCLR_Result::Success;
}
