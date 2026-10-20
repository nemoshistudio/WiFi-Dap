/*
 * SPDX-License-Identifier: MIT
 *
 * Static STM32 DEV_ID table - see tf_stm32_id.h header for sources and limits.
 * Generated from stlink-org/stlink config/chips (one .chip file per device group) (master 2026-07-27,
 * BSD-3-Clause), cross-checked against OpenOCD master tcl/target/stm32*.cfg
 * and STM32 reference manuals.
 */
#include "tf_stm32_id.h"

#include <stdint.h>

const tf_stm32_part_t tf_stm32_part_table[] = {
    /* 0x01E  STM32WB06_WB07           WB06_WB07.chip               RM0530  */
    { 0x01E, TF_CORE_M4, 0, 0xFF, "STM32WB", "STM32WB06/WB07",
      0x40015800U, 0x00000000U, 0x1FFF7590U },
    /* 0x027  WL3x                     WL3x.chip                    RM0491  */
    { 0x027, TF_CORE_M4, 0, 0xFF, "STM32WL", "STM32WL3x",
      0x40015800U, 0x00000000U, 0x1FFF7590U },
    /* 0x028  STM32WB05                WB05.chip                    RM0491  */
    { 0x028, TF_CORE_M4, 0, 0xFF, "STM32WB", "STM32WB05",
      0x40015800U, 0x00000000U, 0x1FFF7590U },
    /* 0x032  STM32WB06_WB07           WB09.chip                    RM0505  */
    { 0x032, TF_CORE_M4, 0, 0xFF, "STM32WB", "STM32WB06/WB07",
      0x40015800U, 0x00000000U, 0x1FFF7590U },
    /* 0x410  STM32F1xx_MD             F1xx_MD.chip                 RM0008  */
    { 0x410, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F100/F101/F102/F103 (MD)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x411  STM32F2xx                F2xx.chip                    RM0033  */
    { 0x411, TF_CORE_M3, 0, 0x10, "STM32F2", "STM32F205/F207/F215/F217",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x412  STM32F1xx_LD             F1xx_LD.chip                 RM0008  */
    { 0x412, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F101/F102/F103 (LD)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x413  STM32F4x5_F4x7           F4x5_F4x7.chip               RM0090  */
    { 0x413, TF_CORE_M4, 1, 0x10, "STM32F4", "STM32F405/F407/F415/F417",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x414  F1xx_HD                  F1xx_HD.chip                 RM0008  */
    { 0x414, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F100/F101/F102/F103 (HD)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x415  STM32L47x_L48x           L47x_L48x.chip               RM0351  */
    { 0x415, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L471/L475/L476/L485/L486",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x416  STM32L1xx_Cat_1          L1xx_Cat_1.chip              RM0038  */
    { 0x416, TF_CORE_M3, 0, 0x00, "STM32L1", "STM32L151/L152 (Cat.1)",
      0xE0042000U, 0x1FF8004CU, 0x1FF80050U },
    /* 0x417  STM32L0xxx_Cat_3         L0xxx_Cat_3.chip             RM0451  */
    { 0x417, TF_CORE_M0PLUS, 0, 0x00, "STM32L0", "STM32L062/L063",
      0x40015800U, 0x1FF8007CU, 0x1FFFF7ACU },
    /* 0x418  STM32F1xx_CL             F1xx_CL.chip                 RM0008  */
    { 0x418, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F105/F107",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x419  STM32F42x_F43x           F42x_F43x.chip               RM0090  */
    { 0x419, TF_CORE_M4, 0, 0x10, "STM32F4", "STM32F427/F429/F437/F439",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x420  STM32F1xx_VL_MD_LD       F1xx_VL_MD_LD.chip           RM0041  */
    { 0x420, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F100/F101/F102/F103 (VL)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x421  STM32F446                F446.chip                    RM0390  */
    { 0x421, TF_CORE_M4, 0, 0x10, "STM32F4", "STM32F446",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x422  STM32F302_F303_358       F302_F303_F358.chip          RM0365  */
    { 0x422, TF_CORE_M4, 0, 0x00, "STM32F3", "STM32F302/F303/F358",
      0xE0042000U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x423  STM32F401xB_xC           F401xB_xC.chip               RM0368  */
    { 0x423, TF_CORE_M4, 1, 0x10, "STM32F4", "STM32F401xB/F401xC",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x425  STM32L0xxx_Cat_2         L0xxx_Cat_2.chip             RM0451  */
    { 0x425, TF_CORE_M0PLUS, 0, 0x00, "STM32L0", "STM32L031/L041/L051",
      0x40015800U, 0x1FF8007CU, 0x1FFFF7ACU },
    /* 0x427  STM32L1xx_Cat_3          L1xx_Cat_3.chip              RM0038  */
    { 0x427, TF_CORE_M3, 0, 0x00, "STM32L1", "STM32L151/L152/L162 (Cat.3)",
      0xE0042000U, 0x1FF800CCU, 0x1FF80050U },
    /* 0x428  STM32F1xx_VL_HD          F1xx_VL_HD.chip              RM0041  */
    { 0x428, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F100/F101/F102/F103 (VL-HD)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x429  STM32L1xx_Cat_2          L1xx_Cat_2.chip              RM0038  */
    { 0x429, TF_CORE_M3, 0, 0x00, "STM32L1", "STM32L151/L152 (Cat.2)",
      0xE0042000U, 0x1FF8004CU, 0x1FF80050U },
    /* 0x430  STM32F1xx_XLD            F1xx_XLD.chip                RM0008  */
    { 0x430, TF_CORE_M3, 0, 0x00, "STM32F1", "STM32F101/F102/F103/F105/F107 (XL)",
      0xE0042000U, 0x1FFFF7E0U, 0x1FFFF7E8U },
    /* 0x431  STM32F411xC_xE           F411xC_xE.chip               RM0383  */
    { 0x431, TF_CORE_M4, 1, 0x10, "STM32F4", "STM32F411",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x432  STM32F37x                F37x.chip                    RM0313  */
    { 0x432, TF_CORE_M4, 0, 0x00, "STM32F3", "STM32F373/F378",
      0xE0042000U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x433  STM32F401xD_xE           F401xD_xE.chip               RM0368  */
    { 0x433, TF_CORE_M4, 1, 0x10, "STM32F4", "STM32F401xD/F401xE",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x434  STM32F46x_F47x           F46x_F47x.chip               RM0090  */
    { 0x434, TF_CORE_M4, 0, 0x10, "STM32F4", "STM32F465/F466/F467/F468/F469/F476-F479",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x435  STM32L41x_L42x           L43x_L44x.chip               RM0392  */
    { 0x435, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L412/L422",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x436  STM32L1xx_Cat_4          L1xx_Cat_4.chip              RM0038  */
    { 0x436, TF_CORE_M3, 0, 0x00, "STM32L1", "STM32L162 (Cat.4)",
      0xE0042000U, 0x1FF800CCU, 0x1FF80050U },
    /* 0x437  STM32L1xx_Cat_5          L1xx_Cat_5.chip              RM0038  */
    { 0x437, TF_CORE_M3, 0, 0x00, "STM32L1", "STM32L151/L152/L162 (Cat.5)",
      0xE0042000U, 0x1FF800CCU, 0x1FF80050U },
    /* 0x438  STM32F303_F328_F334      F303_F328_F334.chip          RM0364  */
    { 0x438, TF_CORE_M4, 0, 0x00, "STM32F3", "STM32F303/F328/F334",
      0xE0042000U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x439  STM32F301_F302_F318      F301_F302_F318.chip          RM0365  */
    { 0x439, TF_CORE_M4, 0, 0x00, "STM32F3", "STM32F301/F302/F318",
      0xE0042000U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x440  STM32F05x                F05x.chip                    RM0091  */
    { 0x440, TF_CORE_M0, 0, 0x00, "STM32F0", "STM32F050/F051/F058",
      0x40015800U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x441  STM32F412                F412.chip                    RM0402  */
    { 0x441, TF_CORE_M4, 1, 0x10, "STM32F4", "STM32F412",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x442  STM32F09x                F09x.chip                    RM0091  */
    { 0x442, TF_CORE_M0, 0, 0x00, "STM32F0", "STM32F091/F092/F098",
      0x40015800U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x443  STM32C011xx              C011xx.chip                  RM0490  */
    { 0x443, TF_CORE_M0PLUS, 0, 0x00, "STM32C0", "STM32C011",
      0x40015800U, 0x1FFF75A0U, 0x1FFF7590U },
    /* 0x444  STM32F03x                F03x.chip                    RM0091  */
    { 0x444, TF_CORE_M0, 0, 0x00, "STM32F0", "STM32F030/F031/F038",
      0x40015800U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x445  STM32F04x                F04x.chip                    RM0091  */
    { 0x445, TF_CORE_M0, 0, 0x00, "STM32F0", "STM32F040/F041/F042/F048",
      0x40015800U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x446  STM32F302_F303_F398_HD   F302_F303_F398_HD.chip       RM0365  */
    { 0x446, TF_CORE_M4, 0, 0x00, "STM32F3", "STM32F302/F303/F398",
      0xE0042000U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x447  STM32L0xxx_Cat_5         L0xxx_Cat_5.chip             RM0451  */
    { 0x447, TF_CORE_M0PLUS, 0, 0x00, "STM32L0", "STM32L072/L073/L083",
      0x40015800U, 0x1FF8007CU, 0x1FFFF7ACU },
    /* 0x448  STM32F07x                F07x.chip                    RM0091  */
    { 0x448, TF_CORE_M0, 0, 0x00, "STM32F0", "STM32F070/F071/F072/F078",
      0x40015800U, 0x1FFFF7CCU, 0x1FFFF7ACU },
    /* 0x449  STM32F74x_F75x           F74x_F75x.chip               RM0385  */
    { 0x449, TF_CORE_M7, 0, 0x10, "STM32F7", "STM32F745/F746/F747/F750/F755/F756",
      0xE0042000U, 0x1FF0F440U, 0x1FF0F420U },
    /* 0x44C  STM32C051xx              C051xx.chip                  RM0490  */
    { 0x44C, TF_CORE_M0PLUS, 0, 0x00, "STM32C0", "STM32C051",
      0x40015800U, 0x1FFF75A0U, 0x1FFF7590U },
    /* 0x44D  STM32_C091xx_C92xx       C091xx.chip                  RM0490  */
    { 0x44D, TF_CORE_M0PLUS, 0, 0x00, "STM32C0", "STM32C091/C092",
      0x40015800U, 0x1FFF75A0U, 0x1FFF7590U },
    /* 0x44E  STM32C55x_C56x           C55x_C56x.chip               RM0522  */
    { 0x44E, TF_CORE_M33, 0, 0x00, "STM32C5", "STM32C55x/C56x",
      0xE0044000U, 0x08FFF80CU, 0x00000000U },
    /* 0x44F  STM32C53x_C54x           C53x_C54x.chip               RM0522  */
    { 0x44F, TF_CORE_M33, 0, 0x00, "STM32C5", "STM32C53x/C54x",
      0xE0044000U, 0x08FFF80CU, 0x00000000U },
    /* 0x450  STM32H74x_H75x           H74x_H75x.chip               RM0433  */
    { 0x450, TF_CORE_M7, 2, 0x00, "STM32H7", "STM32H742/H743/H745/H747/H750/H753/H755/H757",
      0x5C001000U, 0x1FF1E880U, 0x1FF1E800U },
    /* 0x451  STM32F76x_F77x           F76x_F77x.chip               RM0410  */
    { 0x451, TF_CORE_M7, 0, 0x10, "STM32F7", "STM32F765/F767/F768/F769/F777/F778/F779",
      0xE0042000U, 0x1FF0F440U, 0x1FF0F420U },
    /* 0x452  STM32F72x_F73x           F72x_F73x.chip               RM0431  */
    { 0x452, TF_CORE_M7, 0, 0x10, "STM32F7", "STM32F722/F723/F732/F733",
      0xE0042000U, 0x1FF07A20U, 0x1FF07A10U },
    /* 0x453  STM32C031xx              C031xx.chip                  RM0490  */
    { 0x453, TF_CORE_M0PLUS, 0, 0x00, "STM32C0", "STM32C031",
      0x40015800U, 0x1FFF75A0U, 0x1FFF7590U },
    /* 0x455  STM32U535_U545           U535_U545.chip               RM0456  */
    { 0x455, TF_CORE_M33, 0, 0x00, "STM32U5", "STM32U535/U545",
      0xE0044000U, 0x0BFA07A0U, 0x0BFA0700U },
    /* 0x456  STM32G05x_G06x           G05x_G06x.chip               RM0444  */
    { 0x456, TF_CORE_M0PLUS, 0, 0x00, "STM32G0", "STM32G050/G051/G061",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x457  STM32L0xxx_Cat_1         L0xxx_Cat_1.chip             RM0451  */
    { 0x457, TF_CORE_M0PLUS, 0, 0x00, "STM32L0", "STM32L011/L021/L031",
      0x40015800U, 0x1FF8007CU, 0x1FFFF7ACU },
    /* 0x458  STM32F410                F410.chip                    RM0401  */
    { 0x458, TF_CORE_M4, 0, 0x10, "STM32F4", "STM32F410",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x459  STM32U031xx              U031xx.chip                  RM0503  */
    { 0x459, TF_CORE_M0PLUS, 0, 0x00, "STM32U0", "STM32U031",
      0x40015800U, 0x1FFF3EA0U, 0x00000000U },
    /* 0x45A  STM32C59x_C5Ax           C59x_C5Ax.chip               RM0522  */
    { 0x45A, TF_CORE_M33, 0, 0x00, "STM32C5", "STM32C59x/C5Ax",
      0xE0044000U, 0x08FFF80CU, 0x00000000U },
    /* 0x460  STM32G07x_G08x           G07x_G08x.chip               RM0444  */
    { 0x460, TF_CORE_M0PLUS, 0, 0x00, "STM32G0", "STM32G070/G071/G081",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x461  STM32L496x_L4A6x         L496x_L4A6x.chip             RM0351  */
    { 0x461, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L496/L4A6",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x462  STM32L45x_L46x           L45x_L46x.chip               RM0394  */
    { 0x462, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L451/L452/L462",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x463  STM32F413_F423           F413_F423.chip               RM0430  */
    { 0x463, TF_CORE_M4, 0, 0x10, "STM32F4", "STM32F413/F423",
      0xE0042000U, 0x1FFF7A20U, 0x1FFF7A10U },
    /* 0x464  STM32L41x_L42x           L41x_L42x.chip               RM0394  */
    { 0x464, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L412/L422",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x466  STM32G03x_G04x           G03x_G04x.chip               RM0444  */
    { 0x466, TF_CORE_M0PLUS, 0, 0x00, "STM32G0", "STM32G030/G031/G041",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x467  STM32G0Bx_G0Cx           G0Bx_G0Cx.chip               RM0444  */
    { 0x467, TF_CORE_M0PLUS, 0, 0x00, "STM32G0", "STM32G0B0/G0B1/G0C1",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x468  STM32G43x_G44x           G43x_G44x.chip               RM0440  */
    { 0x468, TF_CORE_M4, 0, 0x00, "STM32G4", "STM32G431/G441",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x469  STM32G47x_G48x           G47x_G48x.chip               RM0440  */
    { 0x469, TF_CORE_M4, 0, 0x00, "STM32G4", "STM32G474/G484",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x470  STM32L4Rx                L4Rx.chip                    RM0432  */
    { 0x470, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L4R5/L4R7/L4R9/L4S5/L4S7/L4S9",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x471  STM32L4Px_L4Qx           L4Px_L4Qx.chip               RM0432  */
    { 0x471, TF_CORE_M4, 0, 0x00, "STM32L4", "STM32L4P5/L4Q5",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x472  STM32L5x2xx              L5x2xx.chip                  RM0438  */
    { 0x472, TF_CORE_M33, 0, 0x00, "STM32L5", "STM32L552/L562",
      0xE0044000U, 0x0BFA05E0U, 0x0BFA0700U },
    /* 0x476  STM32U5Fx_U5Gx           U5Fx_U5Gx.chip               RM0456  */
    { 0x476, TF_CORE_M33, 0, 0x00, "STM32U5", "STM32U5F5/U5G5",
      0xE0044000U, 0x0BFA07A0U, 0x0BFA0700U },
    /* 0x478  STM32H52_H53xx           H52_H53xx.chip               RM0481  */
    { 0x478, TF_CORE_M33, 0, 0x00, "STM32H5", "STM32H523/H533/H562/H563/H573",
      0xE0044000U, 0x08FFF80CU, 0x00000000U },
    /* 0x479  STM32G49x_G4Ax           G49x_G4Ax.chip               RM0440  */
    { 0x479, TF_CORE_M4, 0, 0x00, "STM32G4", "STM32G491/G4A1",
      0x40015800U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x480  STM32H7Ax_H7Bx           H7Ax_H7Bx.chip               RM0455  */
    { 0x480, TF_CORE_M7, 0, 0x00, "STM32H7", "STM32H7A3/H7B3/H7B0",
      0x5C001000U, 0x08FFF80CU, 0x1FF1E800U },
    /* 0x481  STM32U59x_U5Ax           U59x_U5Ax.chip               RM0456  */
    { 0x481, TF_CORE_M33, 0, 0x00, "STM32U5", "STM32U595/U5A5/U599/U5A9",
      0xE0044000U, 0x0BFA07A0U, 0x0BFA0700U },
    /* 0x482  STM32U575_U585           U575_U585.chip               RM0456  */
    { 0x482, TF_CORE_M33, 0, 0x00, "STM32U5", "STM32U575/U585",
      0xE0044000U, 0x0BFA07A0U, 0x0BFA0700U },
    /* 0x483  STM32H72x_H73x           H72x_H73x.chip               RM0468  */
    { 0x483, TF_CORE_M7, 2, 0x00, "STM32H7", "STM32H723/H725/H730/H733/H735",
      0x5C001000U, 0x1FF1E880U, 0x1FF1E800U },
    /* 0x484  STM32H5xx                H5xx.chip                    RM0481  */
    { 0x484, TF_CORE_M33, 0, 0x00, "STM32H5", "STM32H503/H533/H563/H573/H5F4",
      0xE0044000U, 0x08FFF80CU, 0x00000000U },
    /* 0x489  STM32U073xx_U083xx       U073xx_083xx.chip            RM0503  */
    { 0x489, TF_CORE_M0PLUS, 0, 0x00, "STM32U0", "STM32U073/U083",
      0x40015800U, 0x1FFF6EA0U, 0x00000000U },
    /* 0x493  STM32C071xx              C071xx.chip                  RM0490  */
    { 0x493, TF_CORE_M0PLUS, 0, 0x00, "STM32C0", "STM32C071",
      0x40015800U, 0x1FFF75A0U, 0x1FFF7590U },
    /* 0x495  STM32WBx0_WBx5           WBx0_WBx5.chip               RM0434  */
    { 0x495, TF_CORE_M4, 0, 0x00, "STM32WB", "STM32WB10/WB15/WB20/WB25/WB30/WB35",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
    /* 0x497  STM32WLEx                WLEx.chip                    RM0033  */
    { 0x497, TF_CORE_M4, 0, 0x00, "STM32WL", "STM32WLE5/WL55",
      0xE0042000U, 0x1FFF75E0U, 0x1FFF7590U },
};

const size_t tf_stm32_part_table_size =
    sizeof(tf_stm32_part_table) / sizeof(tf_stm32_part_table[0]);

const tf_stm32_part_t *tf_stm32_part_find(uint16_t dev_id)
{
    size_t lo = 0U;
    size_t hi = tf_stm32_part_table_size;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        uint16_t mid_id = tf_stm32_part_table[mid].dev_id;
        if (mid_id == dev_id) {
            return &tf_stm32_part_table[mid];
        }
        if (mid_id < dev_id) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

tf_core_class_t tf_stm32_core_from_cpuid(uint32_t cpuid)
{
    switch ((cpuid >> 4) & 0xFFFU) {
    case 0xC20U: return TF_CORE_M0;
    case 0xC60U: return TF_CORE_M0PLUS;
    case 0xC23U: return TF_CORE_M3;
    case 0xC24U: return TF_CORE_M4;
    case 0xC27U: return TF_CORE_M7;
    case 0xD21U: return TF_CORE_M33;
    default:     return TF_CORE_UNKNOWN;
    }
}

const char *tf_stm32_core_name(tf_core_class_t core)
{
    switch (core) {
    case TF_CORE_M0:     return "Cortex-M0";
    case TF_CORE_M0PLUS: return "Cortex-M0+";
    case TF_CORE_M3:     return "Cortex-M3";
    case TF_CORE_M4:     return "Cortex-M4";
    case TF_CORE_M7:     return "Cortex-M7";
    case TF_CORE_M33:    return "Cortex-M33";
    default:             return "unknown";
    }
}

/* Probe order derived from the DBGMCU base -> core mapping:
 *   Cortex-M7  : H7 (0x5C001000) then F7 (0xE0042000)
 *   Cortex-M4  : F4/L4/G4 (0xE0042000) then G0-like (0x40015800)
 *   Cortex-M3  : F1/F2/L1 (0xE0042000)
 *   Cortex-M0+ : F0/G0/L0/U0/C0 (0x40015800) then 0xE0044000
 *   Cortex-M33 : H5/L5/U5/C5 (0xE0044000) then 0x40015800
 * All four documented bases remain fallbacks, so an unexpected core/base
 * pair is still identified instead of reporting "not detected". */
size_t tf_stm32_dbgmc_probe_bases(tf_core_class_t core, const uint32_t **bases)
{
    switch (core) {
    case TF_CORE_M7:     { static const uint32_t b[] = {0x5C001000U, 0xE0042000U}; *bases = b; return 2U; }
    case TF_CORE_M4:     { static const uint32_t b[] = {0xE0042000U, 0x40015800U}; *bases = b; return 2U; }
    case TF_CORE_M3:     { static const uint32_t b[] = {0xE0042000U, 0x40015800U}; *bases = b; return 2U; }
    case TF_CORE_M33:    { static const uint32_t b[] = {0xE0044000U, 0x40015800U}; *bases = b; return 2U; }
    case TF_CORE_M0:
    case TF_CORE_M0PLUS: { static const uint32_t b[] = {0x40015800U, 0xE0044000U}; *bases = b; return 2U; }
    default:
    {
        static const uint32_t b[] = {0xE0042000U, 0x40015800U, 0x5C001000U, 0xE0044000U};
        *bases = b;
        return 4U;
    }
    }
}
