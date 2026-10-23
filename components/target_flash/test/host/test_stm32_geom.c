/*
 * Host-side regression test for the pure STM32 flash geometry helpers
 * (components/target_flash/src/tf_stm32_geom.c).
 *
 * Covers the STM32G4 page/bank geometry used by the new G4 flash driver
 * (2 KiB pages, FLASH_OPTR.DBANK bit 22, page >= bank1 -> CR.BKER = 1), the
 * L4/G0/WB/WL sub-families, the F0/F1/F3 page geometry and the F2/F4/F7
 * sector maps.  No target hardware / no ESP-IDF needed.
 *
 * Usage: components/target_flash/test/host/run.sh
 */
#include <stdio.h>
#include <string.h>

#include "tf_stm32_geom.h"
#include "tf_stm32_id.h"

#define KIB 1024U
#define G4_DBANK_BIT (1U << 22)
#define G0_DBANK_BIT (1U << 21)
#define G0_NSWAP_BIT (1U << 20) /* nSWAP_BANK: 1 = no bank swap */
#define L4_DBANK_BIT (1U << 21)
#define L4R_DBANK_BIT (1U << 22)

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

static int g4_ok(void)
{
    tf_page_geom_t g;
    uint32_t pnb;
    bool bank2;
    uint32_t first;
    uint32_t last;

    printf("STM32G4 geometry\n");
    check("G431 128K single bank", tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x468U, 128U * KIB, 0U, &g) &&
                                   g.page_size == 2U * KIB && g.num_pages == 64U &&
                                   !g.dual_bank && g.bank1_pages == 64U,
          NULL);
    check("G431 flash too large rejected",
          !tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x468U, 256U * KIB, 0U, &g), NULL);
    check("G474 512K single-bank mode 4K pages",
          tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x469U, 512U * KIB, 0U, &g) &&
              g.page_size == 4U * KIB && g.num_pages == 128U && !g.dual_bank,
          NULL);
    check("G474 512K DBANK mode 2K pages dual bank",
          tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x469U, 512U * KIB, G4_DBANK_BIT, &g) &&
              g.page_size == 2U * KIB && g.num_pages == 256U &&
              g.bank1_pages == 128U && g.dual_bank && g.bker_bit == 11U,
          NULL);
    check("G474 trimmed dual-bank size rejected",
          !tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x469U, 256U * KIB, G4_DBANK_BIT, &g), NULL);
    check("G491 512K single bank 2K pages",
          tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x479U, 512U * KIB, 0U, &g) &&
              g.page_size == 2U * KIB && g.num_pages == 256U && !g.dual_bank,
          NULL);
    check("G4 probe addresses",
          g.idcode_addr == TF_STM32_DBGMCU_IDCODE_E0042000 &&
              g.fsize_addr == TF_STM32_FSIZE_1FFF75E0 &&
              g.uid_addr == TF_STM32_UID_1FFF7590 &&
              g.regs_base == TF_STM32_FLASH_REGS_40022000 &&
              g.optr_addr == 0x40022020U,
          NULL);

    /* G474 dual-bank page -> CR.PNB/BKER mapping: page>=128 -> bank 2, page-128. */
    check("G4 dual page 0 -> PNB 0 bank1",
          tf_stm32_page_geom(TF_PAGE_KIND_G4, 0x469U, 512U * KIB, G4_DBANK_BIT, &g) &&
              tf_stm32_page_cr_fields(&g, 0U, &pnb, &bank2) && pnb == 0U && !bank2,
          NULL);
    check("G4 dual page 127 -> PNB 127 bank1",
          tf_stm32_page_cr_fields(&g, 127U, &pnb, &bank2) && pnb == 127U && !bank2, NULL);
    check("G4 dual page 128 -> PNB 0 bank2 BKER",
          tf_stm32_page_cr_fields(&g, 128U, &pnb, &bank2) && pnb == 0U && bank2, NULL);
    check("G4 dual page 255 -> PNB 127 bank2 BKER",
          tf_stm32_page_cr_fields(&g, 255U, &pnb, &bank2) && pnb == 127U && bank2, NULL);
    check("G4 dual page 256 rejected",
          !tf_stm32_page_cr_fields(&g, 256U, &pnb, &bank2), NULL);
    check("G4 page offset 128 -> 0x40000",
          tf_stm32_page_offset(&g, 128U) == 128U * 2048U, NULL);
    check("G4 cover 0..4K -> pages 0..1",
          tf_stm32_page_cover(&g, 0U, 4096U, &first, &last) && first == 0U && last == 1U, NULL);
    check("G4 cover across bank boundary -> pages 127..128",
          tf_stm32_page_cover(&g, 127U * 2048U, 2U * 2048U, &first, &last) &&
              first == 127U && last == 128U,
          NULL);
    check("G4 cover past flash rejected",
          !tf_stm32_page_cover(&g, 512U * KIB - 2048U, 4096U, &first, &last), NULL);
    return failures;
}

static int l4_g0_ok(void)
{
    tf_page_geom_t g;
    uint32_t pnb;
    bool bank2;

    printf("STM32L4/G0/WB/WL geometry\n");
    check("L476 1M always dual bank 2K pages",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x415U, 1024U * KIB, 0U, &g) &&
              g.dual_bank && g.page_size == 2U * KIB && g.num_pages == 512U &&
              g.bank1_pages == 256U,
          NULL);
    check("L476 512K single bank",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x415U, 512U * KIB, 0U, &g) &&
              !g.dual_bank && g.num_pages == 256U,
          NULL);
    check("L476 512K DBANK bit -> dual bank",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x415U, 512U * KIB, L4_DBANK_BIT, &g) &&
              g.dual_bank && g.bank1_pages == 128U,
          NULL);
    check("L433 256K single bank", tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x435U,
                                                      256U * KIB, 0U, &g) &&
                                       !g.dual_bank && g.num_pages == 128U,
          NULL);
    check("L4R/S 2M DBANK bit -> 4K dual pages",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x470U, 2048U * KIB, L4R_DBANK_BIT, &g) &&
              g.dual_bank && g.page_size == 4U * KIB && g.num_pages == 512U &&
              g.bank1_pages == 256U,
          NULL);
    check("L4R/S 2M single bank 8K pages",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x470U, 2048U * KIB, 0U, &g) &&
              !g.dual_bank && g.page_size == 8U * KIB && g.num_pages == 256U,
          NULL);
    check("L4R/S 1M DB1M bit -> 4K dual pages",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x470U, 1024U * KIB, L4_DBANK_BIT, &g) &&
              g.dual_bank && g.page_size == 4U * KIB && g.num_pages == 256U,
          NULL);
    check("L4P/Q 1M DBANK -> dual 4K",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x471U, 1024U * KIB, L4R_DBANK_BIT, &g) &&
              g.dual_bank && g.page_size == 4U * KIB,
          NULL);
    check("L4 dual bank page 256 -> bank2 PNB 0",
          tf_stm32_page_geom(TF_PAGE_KIND_L4, 0x415U, 1024U * KIB, 0U, &g) &&
              tf_stm32_page_cr_fields(&g, 256U, &pnb, &bank2) && pnb == 0U && bank2,
          NULL);
    check("G030 64K single 2K pages",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x466U, 64U * KIB, 0U, &g) &&
              !g.dual_bank && g.page_size == 2U * KIB && g.num_pages == 32U,
          NULL);
    /* G0B/G0C: 2 KiB pages in both bank modes, always 2 banks above 128 KiB,
     * nSWAP_BANK (OPTR bit20, active low) swaps the bank address mapping and
     * bank-2 pages use the global 256+page-in-bank PNB encoding (Zephyr). */
    check("G0B/C 512K dual bank, no swap, BKER 13",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U, 512U * KIB,
                             G0_NSWAP_BIT, &g) &&
              g.dual_bank && !g.bank_swap && g.page_size == 2U * KIB &&
              g.num_pages == 256U && g.bank1_pages == 128U && g.bker_bit == 13U &&
              g.bsy2_shift == 17U && g.cfgbsy_shift == 18U &&
              g.bank2_pnb_base == 256U && g.pnb_mask == (0x3FFU << 3) &&
              g.idcode_addr == TF_STM32_DBGMCU_IDCODE_40015800,
          NULL);
    check("G0B/C 256K is still dual bank (2 x 64 pages)",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U, 256U * KIB, G0_NSWAP_BIT, &g) &&
              g.dual_bank && g.num_pages == 128U && g.bank1_pages == 64U,
          NULL);
    check("G0B/C 128K single bank", tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U,
                                                      128U * KIB, G0_NSWAP_BIT, &g) &&
                                       !g.dual_bank && g.num_pages == 64U,
          NULL);
    check("G0B/C DUAL_BANK option does not change the mapping",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U, 512U * KIB,
                             G0_NSWAP_BIT | G0_DBANK_BIT, &g) &&
              g.dual_bank && !g.bank_swap,
          NULL);
    /* No-swap page -> CR fields: bank2 PNB = 256 + page-in-bank, BKER set. */
    check("G0B/C no-swap page 128 -> PNB 256 bank2",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U, 512U * KIB, G0_NSWAP_BIT, &g) &&
              tf_stm32_page_cr_fields(&g, 128U, &pnb, &bank2) && pnb == 256U && bank2,
          NULL);
    check("G0B/C no-swap page 255 -> PNB 383 bank2",
          tf_stm32_page_cr_fields(&g, 255U, &pnb, &bank2) && pnb == 383U && bank2, NULL);
    check("G0B/C no-swap page 127 -> PNB 127 bank1",
          tf_stm32_page_cr_fields(&g, 127U, &pnb, &bank2) && pnb == 127U && !bank2, NULL);
    /* Swap enabled (nSWAP_BANK=0): lower addresses map to bank 2. */
    check("G0B/C swap page 0 -> PNB 256 bank2",
          tf_stm32_page_geom(TF_PAGE_KIND_G0, 0x467U, 512U * KIB, 0U, &g) &&
              g.bank_swap && tf_stm32_page_cr_fields(&g, 0U, &pnb, &bank2) &&
              pnb == 256U && bank2,
          NULL);
    check("G0B/C swap page 128 -> PNB 0 bank1",
          tf_stm32_page_cr_fields(&g, 128U, &pnb, &bank2) && pnb == 0U && !bank2, NULL);
    check("WB5x 1M single 4K pages @0x58004000",
          tf_stm32_page_geom(TF_PAGE_KIND_WB, 0x495U, 1024U * KIB, 0U, &g) &&
              !g.dual_bank && g.page_size == 4U * KIB && g.num_pages == 256U &&
              g.regs_base == TF_STM32_FLASH_REGS_58004000 &&
              g.optr_addr == 0x58004020U,
          NULL);
    check("WLEx 256K single 2K pages @0x58004000",
          tf_stm32_page_geom(TF_PAGE_KIND_WL, 0x497U, 256U * KIB, 0U, &g) &&
              !g.dual_bank && g.page_size == 2U * KIB && g.num_pages == 128U &&
              g.regs_base == TF_STM32_FLASH_REGS_58004000,
          NULL);
    check("WB 512K not modelled for 1M group", tf_stm32_page_geom(TF_PAGE_KIND_WB,
                                                                 0x495U, 2048U * KIB,
                                                                 0U, &g) == 0,
          NULL);
    return failures;
}

static int f1_ok(void)
{
    tf_f1_geom_t g;

    printf("STM32F0/F1/F3 geometry\n");
    check("F103 MD 1K pages",
          tf_stm32_f1_geom(0x410U, 128U * KIB, &g) && g.page_size == KIB &&
              g.num_pages == 128U && !g.dual_bank,
          NULL);
    check("F103 LD 1K pages", tf_stm32_f1_geom(0x412U, 32U * KIB, &g) &&
                                  g.page_size == KIB && g.num_pages == 32U,
          NULL);
    check("F103 HD 2K pages", tf_stm32_f1_geom(0x414U, 512U * KIB, &g) &&
                                  g.page_size == 2U * KIB && g.num_pages == 256U,
          NULL);
    check("F103 CL 2K pages", tf_stm32_f1_geom(0x418U, 256U * KIB, &g) &&
                                  g.page_size == 2U * KIB,
          NULL);
    check("F100 VL MD 1K pages", tf_stm32_f1_geom(0x420U, 128U * KIB, &g) &&
                                     g.page_size == KIB,
          NULL);
    check("F1 XL 1M dual bank: bank2 @0x08080000, regs +0x40",
          tf_stm32_f1_geom(0x430U, 1024U * KIB, &g) && g.dual_bank &&
              g.page_size == 2U * KIB && g.num_pages == 512U &&
              g.bank1_pages == 256U && g.bank2_addr == 0x08080000U &&
              g.bank_reg_offset == 0x40U,
          NULL);
    check("F1 XL 512K single bank", tf_stm32_f1_geom(0x430U, 512U * KIB, &g) &&
                                        !g.dual_bank && g.num_pages == 256U,
          NULL);
    check("F303 2K pages", tf_stm32_f1_geom(0x422U, 256U * KIB, &g) &&
                               g.page_size == 2U * KIB,
          NULL);
    check("F0 1K pages", tf_stm32_f1_geom(0x440U, 64U * KIB, &g) &&
                             g.page_size == KIB && g.num_pages == 64U,
          NULL);
    check("F0 2K pages", tf_stm32_f1_geom(0x448U, 128U * KIB, &g) &&
                             g.page_size == 2U * KIB,
          NULL);
    check("F1 too-large flash rejected", !tf_stm32_f1_geom(0x410U, 256U * KIB, &g), NULL);
    return failures;
}

static int sector_ok(void)
{
    tf_sector_t s[32];
    size_t n;

    printf("STM32F2/F4/F7 sector maps\n");
    n = tf_stm32_sector_map(0x411U, 1024U * KIB, s, 32U);
    check("F2 1M has 12 sectors", n == 12U, NULL);
    check("F2 1M sector layout",
          n == 12U && s[0].offset == 0U && s[0].size == 16U * KIB &&
              s[3].offset == 48U * KIB && s[4].offset == 64U * KIB &&
              s[4].size == 64U * KIB && s[5].offset == 128U * KIB &&
              s[5].size == 128U * KIB && s[11].offset == 896U * KIB &&
              s[11].size == 128U * KIB,
          NULL);
    n = tf_stm32_sector_map(0x413U, 512U * KIB, s, 32U);
    check("F407 512K has 8 sectors", n == 8U && s[7].offset == 384U * KIB &&
                                         s[7].size == 128U * KIB,
          NULL);
    n = tf_stm32_sector_map(0x423U, 256U * KIB, s, 32U);
    check("F401 256K clips last sector",
          n == 6U && s[5].offset == 128U * KIB && s[5].size == 128U * KIB, NULL);
    n = tf_stm32_sector_map(0x449U, 1024U * KIB, s, 32U);
    check("F74x/F75x 1M 32+128+256K sectors",
          n == 8U && s[0].size == 32U * KIB && s[4].offset == 128U * KIB &&
              s[4].size == 128U * KIB && s[5].offset == 256U * KIB &&
              s[5].size == 256U * KIB && s[7].offset == 768U * KIB &&
              s[7].size == 256U * KIB,
          NULL);
    n = tf_stm32_sector_map(0x452U, 512U * KIB, s, 32U);
    check("F72x/F73x 512K 16+64+128K sectors",
          n == 8U && s[0].size == 16U * KIB && s[4].size == 64U * KIB &&
              s[5].size == 128U * KIB,
          NULL);
    check("F42x/F43x 2M dual-bank unsupported",
          tf_stm32_sector_map(0x419U, 2048U * KIB, s, 32U) == 0U, NULL);
    check("F46x/F47x 2M dual-bank unsupported",
          tf_stm32_sector_map(0x434U, 2048U * KIB, s, 32U) == 0U, NULL);
    check("F76x/F77x 2M dual-bank unsupported",
          tf_stm32_sector_map(0x451U, 2048U * KIB, s, 32U) == 0U, NULL);
    check("F7 1M max flash", tf_stm32_sector_max_flash(0x449U) == 1024U * KIB, NULL);
    check("unknown DEV_ID sector map empty",
          tf_stm32_sector_map(0x123U, 512U * KIB, s, 32U) == 0U, NULL);
    return failures;
}

int main(void)
{
    printf("tf_stm32_geom host tests\n");
    g4_ok();
    l4_g0_ok();
    f1_ok();
    sector_ok();
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
