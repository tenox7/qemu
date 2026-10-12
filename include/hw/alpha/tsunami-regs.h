#ifndef _ALPHA_TSUNAMI_REGS_H
#define _ALPHA_TSUNAMI_REGS_H

#include "hw/core/registerfields.h"

/*
 * Tsunami register definitions.
 */

REG64(CCHIP_CSC, 0x0000)
    FIELD(CSC, RES_63, 63, 1)
    FIELD(CSC, RES_62, 62, 1)
    FIELD(CSC, P1W, 61, 1)
    FIELD(CSC, P0W, 60, 1)
    FIELD(CSC, RES_59, 59, 1)
    FIELD(CSC, PBQMAX, 56, 3)
    FIELD(CSC, RES_55, 55, 1)
    FIELD(CSC, PRQMAX, 52, 3)
    FIELD(CSC, RES_51, 51, 1)
    FIELD(CSC, PDTMAX, 48, 3)
    FIELD(CSC, RES_47, 47, 1)
    FIELD(CSC, FPQPMAX, 44, 3)
    FIELD(CSC, RES_43, 43, 1)
    FIELD(CSC, FPQCMAX, 40, 3)
    FIELD(CSC, AXD, 39, 1)
    FIELD(CSC, TPQMMAX, 36, 3)
    FIELD(CSC, B3D, 35, 1)
    FIELD(CSC, B2D, 34, 1)
    FIELD(CSC, B1D, 33, 1)
    FIELD(CSC, FTI, 32, 1)
    FIELD(CSC, EFT, 31, 1)
    FIELD(CSC, QDI, 28, 3)
    FIELD(CSC, FET, 26, 2)
    FIELD(CSC, QPM, 25, 1)
    FIELD(CSC, PME, 24, 1)
    FIELD(CSC, RES_22, 22, 2)
    FIELD(CSC, DRTP, 20, 2)
    FIELD(CSC, DWFP, 18, 2)
    FIELD(CSC, DWTP, 16, 2)
    FIELD(CSC, RES_15, 15, 1)
    FIELD(CSC, P1P, 14, 1)
    FIELD(CSC, IDDW, 12, 2)
    FIELD(CSC, IDDR, 9, 3)
    FIELD(CSC, AW, 8, 1)
    FIELD(CSC, FW, 7, 1)
    FIELD(CSC, SFD, 6, 1)
    FIELD(CSC, SED, 4, 2)
    FIELD(CSC, C1CFP, 3, 1)
    FIELD(CSC, C0CFP, 2, 1)
    FIELD(CSC, BC, 0, 2)
#define CSC_FTI_DISABLED            0
#define CSC_FTI_ENABLED             1
#define CSC_EFT_0_CYCLES            0
#define CSC_EFT_1_CYCLES            1
#define CSC_QDI_DISABLE_DRAINING    0
#define CSC_QDI_1024_CYCLES         1
#define CSC_QDI_256_CYCLES          2
#define CSC_QDI_64_CYCLES           3
#define CSC_QDI_16_CYCLES           4
#define CSC_QDI_1_CYCLES            5
#define CSC_FET_1_CYCLES            0
#define CSC_FET_2_CYCLES            1
#define CSC_FET_3_CYCLES            2
#define CSC_PME_DISABLED            0
#define CSC_PME_ENABLED             1
#define CSC_QPM_ROUND_ROBIN         0
#define CSC_QPM_MODIFIED_RR         1
#define CSC_DRTP_2_CYCLES           0   /* (rev 0 Dchip) */
#define CSC_DRTP_3_CYCLES           1
#define CSC_DRTP_4_CYCLES           2
#define CSC_DRTP_5_CYCLES           3
#define CSC_DWFP_2_CYCLES           0
#define CSC_DWFP_3_CYCLES           1   /* (rev 0 Dchip) */
#define CSC_DWFP_4_CYCLES           2
#define CSC_DWFP_5_CYCLES           3
#define CSC_DWTP_2_CYCLES           0
#define CSC_DWTP_3_CYCLES           1
#define CSC_DWTP_4_CYCLES           2   /* (rev 0 Dchip) */
#define CSC_DWTP_5_CYCLES           3
#define CSC_IDDW_3_CYCLES           0
#define CSC_IDDW_4_CYCLES           1
#define CSC_IDDW_5_CYCLES           2
#define CSC_IDDW_6_CYCLES           3
#define CSC_IDDR_5_CYCLES           0
#define CSC_IDDR_6_CYCLES           1
#define CSC_IDDR_7_CYCLES           2
#define CSC_IDDR_8_CYCLES           3
#define CSC_IDDR_9_CYCLES           4
#define CSC_IDDR_10_CYCLES          5
#define CSC_IDDR_11_CYCLES          6
#define CSC_AW_16_BYTES             0
#define CSC_AW_32_BYTES             1
#define CSC_SFD_2_CYCLES            0
#define CSC_SFD_3_CYCLES            1
#define CSC_SED_2_CYCLES            0
#define CSC_SED_3_CYCLES            1
#define CSC_SED_4_CYCLES            2
#define CSC_SED_5_CYCLES            3
#define CSC_BC_2D_1M                0   /* 2 Dchips, 1 memory bus */
#define CSC_BC_4D_1M                1   /* 4 Dchips, 1 memory bus */
#define CSC_BC_4D_2M                2   /* 4 Dchips, 2 memory buses */
#define CSC_BC_8D_2M                3   /* 8 Dchips, 1 memory buses */

REG64(CCHIP_MTR, 0x0040)
    FIELD(MTR, RES_46, 46, 18)
    FIELD(MTR, MPH, 40, 6)
    FIELD(MTR, PHCW, 36, 4)
    FIELD(MTR, PHCR, 32, 4)
    FIELD(MTR, RES_30, 30, 2)
    FIELD(MTR, RI, 24, 6)
    FIELD(MTR, RES_21, 21, 3)
    FIELD(MTR, MPD, 20, 1)
    FIELD(MTR, RES_17, 17, 3)
    FIELD(MTR, RRD, 16, 1)
    FIELD(MTR, RES_14, 14, 2)
    FIELD(MTR, RPT, 12, 2)
    FIELD(MTR, RES_10, 10, 2)
    FIELD(MTR, RPW, 8, 2)
    FIELD(MTR, RES_7, 7, 1)
    FIELD(MTR, IRD, 4, 3)
    FIELD(MTR, RES_3, 3, 1)
    FIELD(MTR, CAT, 2, 1)
    FIELD(MTR, RES_1, 1, 1)
    FIELD(MTR, RCD, 0, 1)
#define MTR_MPD_NO_DELAY            0
#define MTR_MPD_ONE_PIPELINE_STAGE  1
#define MTR_RRD_2_CYCLES            0
#define MTR_RRD_3_CYCLES            1
#define MTR_RPT_2_CYCLES            0
#define MTR_RPT_3_CYCLES            1
#define MTR_RPT_4_CYCLES            2
#define MTR_RPW_4_CYCLES            0
#define MTR_RPW_5_CYCLES            1
#define MTR_RPW_6_CYCLES            2
#define MTR_RPW_7_CYCLES            3
#define MTR_IRD_0_CYCLES            0
#define MTR_IRD_1_CYCLES            1
#define MTR_IRD_2_CYCLES            2
#define MTR_IRD_3_CYCLES            3
#define MTR_IRD_4_CYCLES            4
#define MTR_IRD_5_CYCLES            5
#define MTR_CAT_2_CYCLES            0
#define MTR_CAT_3_CYCLES            1
#define MTR_RCD_2_CYCLES            0
#define MTR_RCD_3_CYCLES            1

REG64(CCHIP_MISC, 0x0080)
    FIELD(MISC, RES_44, 44, 20)
    FIELD(MISC, DEVSUP, 40, 4)
    FIELD(MISC, REV, 32, 8)
    FIELD(MISC, NXS, 29, 3)
    FIELD(MISC, NXM, 28, 1)
    FIELD(MISC, RES_25, 25, 3)
    FIELD(MISC, ACL, 24, 1)
    FIELD(MISC, ABT, 20, 4)
    FIELD(MISC, ABW, 16, 4)
    FIELD(MISC, IPREQ, 12, 4)
    FIELD(MISC, IPINTR, 8, 4)
    FIELD(MISC, ITINTR, 4, 4)
    FIELD(MISC, RES_2, 2, 2)
    FIELD(MISC, CPUID, 0, 2)
#define MISC_REV_TSUNAMI            1
#define MISC_REV_TYPHOON            8
#define MISC_NXS_CPU0               0
#define MISC_NXS_CPU1               1
#define MISC_NXS_CPU2               2
#define MISC_NXS_CPU3               3
#define MISC_NXS_PCHIP0             4
#define MISC_NXS_PCHIP1             5

REG64(CCHIP_MPD, 0x00c0)
    FIELD(MPD, RES_4, 4, 60)
    FIELD(MPD, DR, 3, 1)
    FIELD(MPD, CKR, 2, 1)
    FIELD(MPD, DS, 1, 1)
    FIELD(MPD, CKS, 0, 1)

REG64(CCHIP_AAR0, 0x0100)
REG64(CCHIP_AAR1, 0x0140)
REG64(CCHIP_AAR2, 0x0180)
REG64(CCHIP_AAR3, 0x01c0)
    FIELD(AAR, RES_35, 35, 29)
    FIELD(AAR, ADDR, 24, 11)
    FIELD(AAR, RES_17, 17, 7)
    FIELD(AAR, DBG, 16, 1)
    FIELD(AAR, ASIZ, 12, 4)
    FIELD(AAR, RES_10, 10, 2)
    FIELD(AAR, TSA, 9, 1)
    FIELD(AAR, SA, 8, 1)
    FIELD(AAR, RES_4, 4, 4)
    FIELD(AAR, ROWS, 2, 2)
    FIELD(AAR, BNKS, 0, 2)
#define AAR_DBG_DISABLED            0
#define AAR_DBG_ENABLED             1
#define AAR_ASIZ_DISABLED           0
#define AAR_ASIZ_16MB               1
#define AAR_ASIZ_32MB               2
#define AAR_ASIZ_64MB               3
#define AAR_ASIZ_128MB              4
#define AAR_ASIZ_256MB              5
#define AAR_ASIZ_512MB              6
#define AAR_ASIZ_1GB                7
#define AAR_ASIZ_2GB                8
#define AAR_ASIZ_4GB                9
#define AAR_ASIZ_8GB                10
#define AAR_TSA_DISABLED            0
#define AAR_TSA_ENABLED             1
#define AAR_SA_DISABLED             0
#define AAR_SA_ENABLED              1
#define AAR_ROWS_11_BITS            0
#define AAR_ROWS_12_BITS            1
#define AAR_ROWS_13_BITS            2
#define AAR_BNKS_1_BITS             0
#define AAR_BNKS_2_BITS             1
#define AAR_BNKS_3_BITS             2

REG64(CCHIP_DIM0, 0x0200)
REG64(CCHIP_DIM1, 0x0240)
REG64(CCHIP_DIM2, 0x0600)
REG64(CCHIP_DIM3, 0x0640)

REG64(CCHIP_DIR0, 0x0280)
REG64(CCHIP_DIR1, 0x02c0)
REG64(CCHIP_DIR2, 0x0680)
REG64(CCHIP_DIR3, 0x06c0)
    FIELD(DIR, ERR, 58, 6)
    FIELD(DIR, RES_56, 56, 2)
    FIELD(DIR, DEV, 0, 56)
#define DIR_ERR_NXM                 0x20
#define DIR_ERR_PCHIP0              0x10
#define DIR_ERR_PCHIP1              0x80
#define DIR_ERR_NONE                0x00

REG64(CCHIP_DRIR, 0x0300)

REG64(CCHIP_PRBEN, 0x0340)
    FIELD(PRBE, RES_2, 4, 60)
    FIELD(PRBE, PRBEN3, 3, 1)
    FIELD(PRBE, PRBEN2, 2, 1)
    FIELD(PRBE, PRBEN1, 1, 1)
    FIELD(PRBE, PRBEN0, 0, 1)
#define PRBE_PRBEN_DISABLED         0
#define PRBE_PRBEN_ENABLED          1

REG64(CCHIP_IIC0, 0x0380)
REG64(CCHIP_IIC1, 0x03c0)
REG64(CCHIP_IIC2, 0x0700)
REG64(CCHIP_IIC3, 0x0740)
    FIELD(IIC, RES_25, 25, 39)
    FIELD(IIC, OF, 24, 1)
    FIELD(IIC, ICNT, 0, 24)
#define IIC_OF_POSITIVE             0
#define IIC_OF_NEGATIVE             1

REG64(CCHIP_MPR0, 0x0400)
REG64(CCHIP_MPR1, 0x0440)
REG64(CCHIP_MPR2, 0x0480)
REG64(CCHIP_MPR3, 0x04c0)
    FIELD(MPR, RES_13, 13, 51)
    FIELD(MPR, MPRDAT, 0, 13)

REG64(CCHIP_TTR, 0x0580)
    FIELD(TTR, RES_15, 15, 49)
    FIELD(TTR, ID, 12, 3)
    FIELD(TTR, RES_10, 10, 2)
    FIELD(TTR, IRT, 8, 2)
    FIELD(TTR, RES_6, 6, 2)
    FIELD(TTR, IS, 4, 2)
    FIELD(TTR, RES_2, 2, 2)
    FIELD(TTR, AH, 1, 1)
    FIELD(TTR, AS, 0, 1)
#define TTR_IRT_1_CYCLES            0
#define TTR_IRT_2_CYCLES            1
#define TTR_IRT_3_CYCLES            2
#define TTR_IRT_4_CYCLES            3
#define TTR_IS_1_CYCLES             0
#define TTR_IS_2_CYCLES             1
#define TTR_IS_3_CYCLES             2
#define TTR_IS_4_CYCLES             3
#define TTR_AH_1_CYCLES             0
#define TTR_AH_2_CYCLES             1
#define TTR_AS_1_CYCLES             0
#define TTR_AS_2_CYCLES             1

REG64(CCHIP_TDR, 0x05c0)
    FIELD(TDR, WH3, 63, 1)
    FIELD(TDR, WP3, 60, 3)
    FIELD(TDR, RES_58, 58, 2)
    FIELD(TDR, WS3, 56, 2)
    FIELD(TDR, RES_55, 55, 1)
    FIELD(TDR, RD3, 52, 3)
    FIELD(TDR, RA3, 48, 4)
    FIELD(TDR, WH2, 47, 1)
    FIELD(TDR, WP2, 44, 3)
    FIELD(TDR, RES_42, 42, 2)
    FIELD(TDR, WS2, 40, 2)
    FIELD(TDR, RES_39, 39, 1)
    FIELD(TDR, RD2, 36, 3)
    FIELD(TDR, RA2, 32, 4)
    FIELD(TDR, WH1, 31, 1)
    FIELD(TDR, WP1, 28, 3)
    FIELD(TDR, RES_26, 26, 2)
    FIELD(TDR, WS1, 24, 2)
    FIELD(TDR, RES_23, 23, 1)
    FIELD(TDR, RD1, 20, 3)
    FIELD(TDR, RA1, 16, 4)
    FIELD(TDR, WH0, 15, 1)
    FIELD(TDR, WP0, 12, 3)
    FIELD(TDR, RES_10, 10, 2)
    FIELD(TDR, WS0, 8, 2)
    FIELD(TDR, RES_7, 7, 1)
    FIELD(TDR, RD0, 4, 3)
    FIELD(TDR, RA0, 0, 4)

REG64(CCHIP_PWR, 0x0780)
    FIELD(PWR, RES_1, 1, 63)
    FIELD(PWR, SR, 0, 1)
#define PWR_SR_NORMAL               0
#define PWR_SR_SELF_REFRESH         1

REG64(CCHIP_CMONCTLA, 0x0C00)
    FIELD(CMONCTLA, RES_62, 62, 2)
    FIELD(CMONCTLA, MSK23, 52, 10)
    FIELD(CMONCTLA, RES_50, 50, 2)
    FIELD(CMONCTLA, MSK01, 40, 10)
    FIELD(CMONCTLA, STKDIS3, 39, 1)
    FIELD(CMONCTLA, STKDIS2, 38, 1)
    FIELD(CMONCTLA, STKDIS1, 37, 1)
    FIELD(CMONCTLA, STKDIS0, 36, 1)
    FIELD(CMONCTLA, RES_34, 34, 2)
    FIELD(CMONCTLA, SLCTMBL, 32, 2)
    FIELD(CMONCTLA, SLCT3, 24, 8)
    FIELD(CMONCTLA, SLCT2, 16, 8)
    FIELD(CMONCTLA, SLCT1, 8, 8)
    FIELD(CMONCTLA, SLCT0, 0, 8)
#define CMONCTLA_STKDIS_ALL_ONES    0
#define CMONCTLA_STKDIS_WRAPS       1
#define CMONCTLA_SLCTMBL_MGROUP0    0
#define CMONCTLA_SLCTMBL_MGROUP1    1
#define CMONCTLA_SLCTMBL_MGROUP2    2
#define CMONCTLA_SLCTMBL_MGROUP3    3

REG64(CCHIP_CMONCTLB, 0x0C40)
    FIELD(CMONCTLB, RES_62, 62, 2)
    FIELD(CMONCTLB, MTE3, 52, 10)
    FIELD(CMONCTLB, RES_50, 50, 2)
    FIELD(CMONCTLB, MTE2, 40, 10)
    FIELD(CMONCTLB, RES_38, 38, 2)
    FIELD(CMONCTLB, MTE1, 28, 10)
    FIELD(CMONCTLB, RES_26, 26, 2)
    FIELD(CMONCTLB, MTE0, 16, 10)
    FIELD(CMONCTLB, RES_1, 1, 15)
    FIELD(CMONCTLB, DIS, 0, 1)
#define CMONCTLB_DIS_IN_USE         0
#define CMONCTLB_DIS_STATIC         1

REG64(CCHIP_CMONCNT01, 0x0C80)
    FIELD(CMONCNT01, ECNT1, 32, 32)
    FIELD(CMONCNT01, ECNT0, 0, 32)

REG64(CCHIP_CMONCNT23, 0x0CC0)
    FIELD(CMONCNT23, ECNT3, 32, 32)
    FIELD(CMONCNT23, ECNT2, 0, 32)

REG64(DCHIP_DSC, 0x0800)
    FIELD(DSC, RES_7, 7, 1)
    FIELD(DSC, P1P, 6, 1)
    FIELD(DSC, C3CFP, 5, 1)
    FIELD(DSC, C2CFP, 4, 1)
    FIELD(DSC, C1CFP, 3, 1)
    FIELD(DSC, C0CFP, 2, 1)
    FIELD(DSC, BC, 0, 2)

REG64(DCHIP_STR, 0x0840)
    FIELD(STR, RES_7, 6, 2)
    FIELD(STR, IDDW, 4, 2)
    FIELD(STR, IDDR, 1, 3)
    FIELD(STR, AW, 0, 1)

REG64(DCHIP_DREV, 0x0880)
    FIELD(DREV, RES_60, 60, 4)
    FIELD(DREV, REV7, 56, 4)
    FIELD(DREV, RES_52, 52, 4)
    FIELD(DREV, REV6, 48, 4)
    FIELD(DREV, RES_44, 44, 4)
    FIELD(DREV, REV5, 40, 4)
    FIELD(DREV, RES_36, 36, 4)
    FIELD(DREV, REV4, 32, 4)
    FIELD(DREV, RES_28, 28, 4)
    FIELD(DREV, REV3, 24, 4)
    FIELD(DREV, RES_20, 20, 4)
    FIELD(DREV, REV2, 16, 4)
    FIELD(DREV, RES_12, 12, 4)
    FIELD(DREV, REV1, 8, 4)
    FIELD(DREV, RES_4, 4, 4)
    FIELD(DREV, REV0, 0, 4)

REG64(DCHIP_DSC2, 0x08c0)
    FIELD(DSC2, RES_5, 5, 59)
    FIELD(DSC2, RES_2, 2, 3)
    FIELD(DSC2, P1W, 1, 1)
    FIELD(DSC2, P0W, 0, 1)

REG64(PCHIP_WSBA0, 0x0000)
REG64(PCHIP_WSBA1, 0x0040)
REG64(PCHIP_WSBA2, 0x0080)
REG64(PCHIP_WSBA3, 0x00c0)
    FIELD(WSBA, RES_32, 32, 32)
    FIELD(WSBA, ADDR, 20, 12)
    FIELD(WSBA, RES_2, 2, 18)
    FIELD(WSBA, SG, 1, 1)
    FIELD(WSBA, ENA, 0, 1)
#define WSBA_ENA_DISABLE            0
#define WSBA_ENA_ENABLE             1
#define WSBA_SG_DISABLE             0
#define WSBA_SG_ENABLE              1
    FIELD(WSBA3, RES_40, 40, 24)
    FIELD(WSBA3, DAC, 39, 1)
    FIELD(WSBA3, RES_32, 32, 7)
    FIELD(WSBA3, ADDR, 20, 12)
    FIELD(WSBA3, RES_2, 2, 18)
    FIELD(WSBA3, SG, 1, 1)
    FIELD(WSBA3, ENA, 0, 1)
#define WSBA3_DAC_DISABLE           0
#define WSBA3_DAC_ENABLE            1

REG64(PCHIP_WSM0, 0x0100)
REG64(PCHIP_WSM1, 0x0140)
REG64(PCHIP_WSM2, 0x0180)
REG64(PCHIP_WSM3, 0x01c0)
    FIELD(WSM, RES_32, 32, 32)
    FIELD(WSM, AM, 20, 12)
    FIELD(WSM, RES_0, 0, 20)

REG64(PCHIP_TBA0, 0x0200)
REG64(PCHIP_TBA1, 0x0240)
REG64(PCHIP_TBA2, 0x0280)
REG64(PCHIP_TBA3, 0x02c0)
    FIELD(TBA, RES_35, 35, 29)
    FIELD(TBA, ADDR, 10, 25)
    FIELD(TBA, ADDR_DAC, 22, 13)
    FIELD(TBA, RES_0, 0, 10)

REG64(PCHIP_PCTL, 0x0300)
    FIELD(PCTL, RES_48, 48, 16)
    FIELD(PCTL, PID, 46, 2)
    FIELD(PCTL, RPP, 45, 1)
    FIELD(PCTL, PTEVRFY, 44, 1)
    FIELD(PCTL, FDWDIS, 43, 1)
    FIELD(PCTL, FDSDIS, 42, 1)
    FIELD(PCTL, PCLKX, 40, 2)
    FIELD(PCTL, PTPMAX, 36, 4)
    FIELD(PCTL, CRQMAX, 32, 4)
    FIELD(PCTL, REV, 24, 8)
    FIELD(PCTL, CDQMAX, 20, 4)
    FIELD(PCTL, PADM, 19, 1)
    FIELD(PCTL, ECCEN, 18, 1)
    FIELD(PCTL, RES_16, 16, 2)
    FIELD(PCTL, PPRI, 15, 1)
    FIELD(PCTL, PRIGRP, 8, 7)
    FIELD(PCTL, ARBENA, 7, 1)
    FIELD(PCTL, MWIN, 6, 1)
    FIELD(PCTL, HOLE, 5, 1)
    FIELD(PCTL, TGTLAT, 4, 1)
    FIELD(PCTL, CHAINDIS, 3, 1)
    FIELD(PCTL, THDIS, 2, 1)
    FIELD(PCTL, FBTB, 1, 1)
    FIELD(PCTL, FDSC, 0, 1)
#define PCTL_RPP_NOT_PRESENT        0
#define PCTL_RPP_PRESENT            1
#define PCTL_PTEVRFY_DISABLE        0
#define PCTL_PTEVRFY_ENABLE         1
#define PCTL_FDWDIS_NORMAL          0
#define PCTL_FDWDIS_TEST            1
#define PCTL_FDSDIS_NORMAL          0
#define PCTL_FDSDIS_TEST            1
#define PCTL_PCLKX_6_TIMES          0
#define PCTL_PCLKX_4_TIMES          1
#define PCTL_PCLKX_5_TIMES          2
#define PCTL_PADM_8_8               0
#define PCTL_PADM_4_4               1
#define PCTL_ECCEN_DISABLE          0
#define PCTL_ECCEN_ENABLE           1
#define PCTL_PPRI_LOW               0
#define PCTL_PPRI_HIGH              1
#define PCTL_PCLKX_6X               0
#define PCTL_PCLKX_4X               1
#define PCTL_PCLKX_5X               2
#define PCTL_PRIGRP_PCI_LOW         0x00
#define PCTL_PRIGRP_PCI0_HIGH       0x01
#define PCTL_PRIGRP_PCI1_HIGH       0x02
#define PCTL_PRIGRP_PCI2_HIGH       0x04
#define PCTL_PRIGRP_PCI3_HIGH       0x08
#define PCTL_PRIGRP_PCI4_HIGH       0x10
#define PCTL_PRIGRP_PCI5_HIGH       0x20
#define PCTL_PRIGRP_PCI6_HIGH       0x40
#define PCTL_ARBENA_DISABLE         0
#define PCTL_ARBENA_ENABLE          1
#define PCTL_MWIN_DISABLE           0
#define PCTL_MWIN_ENABLE            1
#define PCTL_HOLE_DISABLE           0
#define PCTL_HOLE_ENABLE            1
#define PCTL_TGTLAT_DISABLE         0
#define PCTL_TGTLAT_ENABLE          1
#define PCTL_CHAINDIS_DISABLE       0
#define PCTL_CHAINDIS_ENABLE        1
#define PCTL_THDIS_NORMAL           0
#define PCTL_THDIS_TEST             1
#define PCTL_FBTB_DISABLE           0
#define PCTL_FBTB_ENABLE            1
#define PCTL_FDSC_DISABLE           0
#define PCTL_FDSC_ENABLE            1

REG64(PCHIP_PLAT, 0x0340)
    FIELD(PLAT, RES_48, 16, 48)
    FIELD(PLAT, LAT, 8, 8)
    FIELD(PLAT, RES_0, 0, 8)

REG64(PCHIP_RESV, 0x0380)

REG64(PCHIP_PERROR, 0x03c0)
    FIELD(PERROR, SYN, 56, 8)
    FIELD(PERROR, CMD, 52, 4)
    FIELD(PERROR, INV, 51, 1)
    FIELD(PERROR, ADDR, 16, 35)
    FIELD(PERROR, RES_12, 12, 4)
    FIELD(PERROR, CRE, 11, 1)
    FIELD(PERROR, UECC, 10, 1)
    FIELD(PERROR, RES_9, 9, 1)
    FIELD(PERROR, NDS, 8, 1)
    FIELD(PERROR, RDPE, 7, 1)
    FIELD(PERROR, TA, 6, 1)
    FIELD(PERROR, APE, 5, 1)
    FIELD(PERROR, SGE, 4, 1)
    FIELD(PERROR, DCRTO, 3, 1)
    FIELD(PERROR, PERR, 2, 1)
    FIELD(PERROR, SERR, 1, 1)
    FIELD(PERROR, LOST, 0, 1)
#define PERROR_SGE_VALID            0
#define PERROR_SGE_INVALID          1
#define PERROR_CMD_DMA_READ         0b0000
#define PERROR_CMD_DMA_RMW          0b0001
#define PERROR_CMD_SGTE_READ        0b0011
#define PERROR_INFO_VALID           0
#define PERROR_INFO_INVALID         1
#define PERROR_LOST_NOT_LOST        0
#define PERROR_LOST_LOST            1

REG64(PCHIP_PERRMASK, 0x0400)
    FIELD(PERRMASK, RES_12, 12, 52)
    FIELD(PERRMASK, MASK, 0, 12)

REG64(PCHIP_PERRSET, 0x0440)
    FIELD(PERRSET, INFO, 16, 48)
    FIELD(PERRSET, RES_12, 12, 4)
    FIELD(PERRSET, SET, 0, 12)

REG64(PCHIP_TLBIV, 0x0480)
    FIELD(TLBIV, RES_28, 28, 36)
    FIELD(TLBIV, DAC, 27, 1)
    FIELD(TLBIV, RES_20, 20, 7)
    FIELD(TLBIV, ADDR, 4, 16)
    FIELD(TLBIV, RES_0, 0, 4)
#define TLBIV_DAC_DISABLE           0
#define TLBIV_DAC_ENABLE            1

REG64(PCHIP_TLBIA, 0x04c0)

REG64(PCHIP_PMONCTL, 0x0500)
    FIELD(PMONCTL, RES_18, 18, 46)
    FIELD(PMONCTL, STKDIS1, 17, 1)
    FIELD(PMONCTL, STKDIS0, 16, 1)
    FIELD(PMONCTL, SLCT1, 8, 8)
    FIELD(PMONCTL, SLCT0, 0, 8)
#define PMONCTL_STKDIS_STICKS_1S    0
#define PMONCTL_STKDIS_WRAPS_1S     1

REG64(PCHIP_PMONCNT, 0x0540)
    FIELD(PMONCNT, CNT1, 32, 32)
    FIELD(PMONCNT, CNT0, 0, 32)

REG64(PCHIP_SPRST, 0x0800)

#endif /* _ALPHA_TSUNAMI_REGS_H */
