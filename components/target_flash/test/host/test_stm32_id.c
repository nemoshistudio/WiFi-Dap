/*
 * Host-side regression test for the pure STM32 DBGMCU DEV_ID table and the
 * flash-size/candidate narrowing helpers (components/target_flash/src/
 * tf_stm32_id.c).  No target hardware / no ESP-IDF needed.
 *
 * Usage: components/target_flash/test/host/run.sh
 */
#include <stdio.h>
#include <string.h>

#include "tf_stm32_id.h"

static int checks;
static int failures;
static void check(const char *name, int cond, const char *detail)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        failures++;
        printf("  FAIL %s%s%s\n", name, detail ? " :: " : "", detail ? detail : "");
    }
}

static int core_ok(void)
{
    check("core M0  cpuid 0x410CC200", tf_stm32_core_from_cpuid(0x410CC200U) == TF_CORE_M0, NULL);
    check("core M0+ cpuid 0x410CC600", tf_stm32_core_from_cpuid(0x410CC600U) == TF_CORE_M0PLUS, NULL);
    check("core M3  cpuid 0x410FC231", tf_stm32_core_from_cpuid(0x410FC231U) == TF_CORE_M3, NULL);
    check("core M4  cpuid 0x410FC241", tf_stm32_core_from_cpuid(0x410FC241U) == TF_CORE_M4, NULL);
    check("core M7  cpuid 0x411FC271", tf_stm32_core_from_cpuid(0x411FC271U) == TF_CORE_M7, NULL);
    check("core M33 cpuid 0x410FD213", tf_stm32_core_from_cpuid(0x410FD213U) == TF_CORE_M33, NULL);
    check("core unknown", tf_stm32_core_from_cpuid(0x00000000U) == TF_CORE_UNKNOWN, NULL);
    check("core name M7", strcmp(tf_stm32_core_name(TF_CORE_M7), "Cortex-M7") == 0, NULL);
    check("core name unknown", strcmp(tf_stm32_core_name(TF_CORE_UNKNOWN), "unknown") == 0, NULL);
    return failures;
}

static int table_ok(void)
{
    size_t i;
    check("table size is 80 groups", tf_stm32_part_table_size == 80U, NULL);
    for (i = 0U; i < tf_stm32_part_table_size; i++) {
        const tf_stm32_part_t *p = &tf_stm32_part_table[i];
        if (i > 0U && p->dev_id <= tf_stm32_part_table[i - 1U].dev_id) {
            check("table sorted + unique", 0, "table order broken");
            return failures;
        }
        if (p->dev_id == 0U || p->dev_id == 0xFFFU) {
            check("table dev_id sane", 0, "bad dev_id in table");
            return failures;
        }
    }
    check("table sorted + unique", 1, NULL);
    for (i = 0U; i < tf_stm32_part_table_size; i++) {
        const tf_stm32_part_t *p = &tf_stm32_part_table[i];
        if (tf_stm32_part_find(p->dev_id) != p) {
            check("lookup finds every entry", 0, p->series);
            return failures;
        }
    }
    check("lookup finds every entry", 1, NULL);
    check("lookup miss 0x000", tf_stm32_part_find(0x000U) == NULL, NULL);
    check("lookup miss 0x7FF", tf_stm32_part_find(0x7FFU) == NULL, NULL);
    check("0x410 series STM32F1", strcmp(tf_stm32_part_find(0x410U)->series, "STM32F1") == 0, NULL);
    check("0x483 series STM32H7", strcmp(tf_stm32_part_find(0x483U)->series, "STM32H7") == 0, NULL);
    check("0x450 flash ops H7", tf_stm32_part_find(0x450U)->flash_ops == TF_STM32_FLASH_OPS_H7, NULL);
    check("0x413 flash ops F4", tf_stm32_part_find(0x413U)->flash_ops == TF_STM32_FLASH_OPS_F4, NULL);
    check("0x410 flash ops F1X", tf_stm32_part_find(0x410U)->flash_ops == TF_STM32_FLASH_OPS_F1X, NULL);
    check("0x449 flash ops F7", tf_stm32_part_find(0x449U)->flash_ops == TF_STM32_FLASH_OPS_F7, NULL);
    check("0x452 flash ops F7", tf_stm32_part_find(0x452U)->flash_ops == TF_STM32_FLASH_OPS_F7, NULL);
    check("0x468 flash ops G4", tf_stm32_part_find(0x468U)->flash_ops == TF_STM32_FLASH_OPS_G4, NULL);
    check("0x469 flash ops G4", tf_stm32_part_find(0x469U)->flash_ops == TF_STM32_FLASH_OPS_G4, NULL);
    check("0x479 flash ops G4", tf_stm32_part_find(0x479U)->flash_ops == TF_STM32_FLASH_OPS_G4, NULL);
    check("0x415 flash ops L4", tf_stm32_part_find(0x415U)->flash_ops == TF_STM32_FLASH_OPS_L4, NULL);
    check("0x470 flash ops L4", tf_stm32_part_find(0x470U)->flash_ops == TF_STM32_FLASH_OPS_L4, NULL);
    check("0x456 flash ops G0", tf_stm32_part_find(0x456U)->flash_ops == TF_STM32_FLASH_OPS_G0, NULL);
    check("0x467 flash ops G0", tf_stm32_part_find(0x467U)->flash_ops == TF_STM32_FLASH_OPS_G0, NULL);
    check("0x495 flash ops WB", tf_stm32_part_find(0x495U)->flash_ops == TF_STM32_FLASH_OPS_WB, NULL);
    check("0x497 flash ops WL", tf_stm32_part_find(0x497U)->flash_ops == TF_STM32_FLASH_OPS_WL, NULL);
    /* Intentionally identified-only: dual-bank 2 MiB sector parts and the
     * PECR-based L0/L1 and TrustZone L5/U5 families. */
    check("0x419 (F42x/F43x 2MiB) no flash ops", tf_stm32_part_find(0x419U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x434 (F46x/F47x 2MiB) no flash ops", tf_stm32_part_find(0x434U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x451 (F76x/F77x 2MiB) no flash ops", tf_stm32_part_find(0x451U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x416 (L1) no flash ops", tf_stm32_part_find(0x416U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x417 (L0) no flash ops", tf_stm32_part_find(0x417U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x472 (L5) no flash ops", tf_stm32_part_find(0x472U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    check("0x482 (U5) no flash ops", tf_stm32_part_find(0x482U)->flash_ops == TF_STM32_FLASH_OPS_NONE, NULL);
    /* Every flash_ops entry must be a known enum value and have a program unit. */
    for (i = 0U; i < tf_stm32_part_table_size; i++) {
        const tf_stm32_part_t *p = &tf_stm32_part_table[i];
        if (p->flash_ops != TF_STM32_FLASH_OPS_NONE &&
            tf_stm32_flash_program_unit(p->flash_ops) == 0U) {
            check("flash_ops has program unit", 0, p->series);
            return failures;
        }
    }
    check("flash_ops has program unit", 1, NULL);
    return failures;
}

static int program_unit_ok(void)
{
    check("program unit F1X 2", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_F1X) == 2U, NULL);
    check("program unit F4 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_F4) == 8U, NULL);
    check("program unit F7 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_F7) == 8U, NULL);
    check("program unit G4 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_G4) == 8U, NULL);
    check("program unit L4 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_L4) == 8U, NULL);
    check("program unit G0 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_G0) == 8U, NULL);
    check("program unit WB 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_WB) == 8U, NULL);
    check("program unit WL 8", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_WL) == 8U, NULL);
    check("program unit H7 32", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_H7) == 32U, NULL);
    check("program unit NONE 0", tf_stm32_flash_program_unit(TF_STM32_FLASH_OPS_NONE) == 0U, NULL);
    return failures;
}

static int bases_ok(void)
{
    const uint32_t *bases = NULL;
    size_t n;
    n = tf_stm32_dbgmc_probe_bases(TF_CORE_M7, &bases);
    check("M7 bases starts 0x5C001000", n == 2U && bases[0] == 0x5C001000U, NULL);
    n = tf_stm32_dbgmc_probe_bases(TF_CORE_M3, &bases);
    check("M3 bases starts 0xE0042000", n == 2U && bases[0] == 0xE0042000U, NULL);
    n = tf_stm32_dbgmc_probe_bases(TF_CORE_M0PLUS, &bases);
    check("M0+ bases starts 0x40015800", n == 2U && bases[0] == 0x40015800U, NULL);
    n = tf_stm32_dbgmc_probe_bases(TF_CORE_UNKNOWN, &bases);
    check("unknown core has 4 fallbacks", n == 4U, NULL);
    return failures;
}

static int density_ok(void)
{
    check("density 16K -> x4", strcmp(tf_stm32_flash_density_code(16U * 1024U), "x4") == 0, NULL);
    check("density 32K -> x6", strcmp(tf_stm32_flash_density_code(32U * 1024U), "x6") == 0, NULL);
    check("density 64K -> x8", strcmp(tf_stm32_flash_density_code(64U * 1024U), "x8") == 0, NULL);
    check("density 128K -> xB", strcmp(tf_stm32_flash_density_code(128U * 1024U), "xB") == 0, NULL);
    check("density 256K -> xC", strcmp(tf_stm32_flash_density_code(256U * 1024U), "xC") == 0, NULL);
    check("density 512K -> xE", strcmp(tf_stm32_flash_density_code(512U * 1024U), "xE") == 0, NULL);
    check("density 1024K -> xG", strcmp(tf_stm32_flash_density_code(1024U * 1024U), "xG") == 0, NULL);
    check("density 2048K -> xI", strcmp(tf_stm32_flash_density_code(2048U * 1024U), "xI") == 0, NULL);
    check("density 0 -> NULL", tf_stm32_flash_density_code(0U) == NULL, NULL);
    check("density nonstandard -> NULL", tf_stm32_flash_density_code(100U * 1024U) == NULL, NULL);
    check("density applies F1 0x410", tf_stm32_flash_density_applies(0x410U), NULL);
    check("density applies H7 0x483", tf_stm32_flash_density_applies(0x483U), NULL);
    check("density applies F4 0x413", tf_stm32_flash_density_applies(0x413U), NULL);
    check("density not applies WB 0x495", !tf_stm32_flash_density_applies(0x495U), NULL);
    check("density not applies WL 0x497", !tf_stm32_flash_density_applies(0x497U), NULL);
    return failures;
}

static int group_ok(void)
{
    char buf[96];
    const tf_stm32_part_t *f103 = tf_stm32_part_find(0x410U);
    const tf_stm32_part_t *h743 = tf_stm32_part_find(0x450U);
    const tf_stm32_part_t *h723 = tf_stm32_part_find(0x483U);
    const char *s;

    s = tf_stm32_model_group(0x410U, 64U * 1024U, f103->model, buf, sizeof(buf));
    check("F1 group unchanged with 64K", strcmp(s, "STM32F100/F101/F102/F103 (MD)") == 0, s);

    s = tf_stm32_model_group(0x483U, 1024U * 1024U, h723->model, buf, sizeof(buf));
    check("H723 group <=1MiB", strcmp(s, "STM32H723/H725/H730/H733/H735") == 0, s);

    s = tf_stm32_model_group(0x450U, 2048U * 1024U, h743->model, buf, sizeof(buf));
    check("H743 group 2MiB drops H750", strcmp(s, "STM32H743/H745/H747/H750/H755/H757") == 0, s);

    s = tf_stm32_model_group(0x450U, 128U * 1024U, h743->model, buf, sizeof(buf));
    check("H750 value line 128K", strcmp(s, "STM32H750") == 0, s);

    s = tf_stm32_model_group(0x450U, 1024U * 1024U, h743->model, buf, sizeof(buf));
    check("H743 group 1MiB keeps table list", strcmp(s, h743->model) == 0, s);
    return failures;
}

static int candidates_ok(void)
{
    char buf[96];
    tf_narrow_t c = TF_NARROW_UNKNOWN;
    const tf_stm32_part_t *f103 = tf_stm32_part_find(0x410U);
    const tf_stm32_part_t *h743 = tf_stm32_part_find(0x450U);
    const tf_stm32_part_t *wb = tf_stm32_part_find(0x495U);
    const char *s;

    /* task requirement example: F103C8T6 = DEV_ID 0x410 + FSIZE 64 KiB ->
     * narrowed to the x8 density class, family F100/F101/F102/F103 still open. */
    s = tf_stm32_model_candidates(0x410U, 64U * 1024U, f103->model, buf, sizeof(buf), &c);
    check("F1 64K candidates", strcmp(s, "STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]") == 0, s);
    check("F1 64K confidence flash-narrowed", c == TF_NARROW_FLASH, NULL);

    s = tf_stm32_model_candidates(0x410U, 128U * 1024U, f103->model, buf, sizeof(buf), &c);
    check("F1 128K candidates xB", strcmp(s, "STM32F100/F101/F102/F103 (MD) [xB, 128 KiB]") == 0, s);
    check("F1 128K confidence flash-narrowed", c == TF_NARROW_FLASH, NULL);

    s = tf_stm32_model_candidates(0x410U, 0U, f103->model, buf, sizeof(buf), &c);
    check("F1 no FSIZE -> group", strcmp(s, "STM32F100/F101/F102/F103 (MD)") == 0, s);
    check("F1 no FSIZE confidence group", c == TF_NARROW_GROUP, NULL);

    s = tf_stm32_model_candidates(0x450U, 2048U * 1024U, h743->model, buf, sizeof(buf), &c);
    check("H743 2MiB candidates", strcmp(s, "STM32H743/H745/H747/H750/H755/H757 [xI, 2048 KiB]") == 0, s);

    s = tf_stm32_model_candidates(0x495U, 512U * 1024U, wb->model, buf, sizeof(buf), &c);
    check("WB 512K -> KiB only (no density letter)",
          strcmp(s, "STM32WB10/WB15/WB20/WB25/WB30/WB35 [512 KiB]") == 0, s);
    check("WB 512K confidence flash-narrowed", c == TF_NARROW_FLASH, NULL);

    s = tf_stm32_model_candidates(0x495U, 0U, wb->model, buf, sizeof(buf), &c);
    check("WB no FSIZE -> group", strcmp(s, "STM32WB10/WB15/WB20/WB25/WB30/WB35") == 0, s);
    check("WB no FSIZE confidence group", c == TF_NARROW_GROUP, NULL);
    return failures;
}

int main(void)
{
    printf("tf_stm32_id host tests\n");
    core_ok();
    table_ok();
    program_unit_ok();
    bases_ok();
    density_ok();
    group_ok();
    candidates_ok();
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
