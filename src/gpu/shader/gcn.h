#pragma once

// GCN 1.1 ("Sea Islands", the PS4 GPU) shader instruction set: encodings,
// opcodes, operand codes and a decoded-instruction form.
//
// Opcode numbers follow AMD's "Sea Islands Series Instruction Set
// Architecture" reference. Every VALU opcode is normalized to its VOP3
// number (VOPC 0-255, VOP2 256+, VOP3-only 320+, VOP1 384+) so VOP1/VOP2/VOPC
// and their VOP3 forms share one translation path.

#include <string>
#include <vector>

#include "common/types.h"

namespace Gpu::Shader::Gcn {

enum class Enc : u8 {
    Unknown,
    SOP2,
    SOPK,
    SOP1,
    SOPC,
    SOPP,
    SMRD,
    VOP2,
    VOP1,
    VOPC,
    VOP3,
    VINTRP,
    DS,
    MUBUF,
    MTBUF,
    MIMG,
    EXP,
};

// ---- operand codes (9-bit source / 7-bit scalar destination) ----
namespace Operand {
constexpr u16 SgprMax = 103;
constexpr u16 VccLo = 106;
constexpr u16 VccHi = 107;
constexpr u16 M0 = 124;
constexpr u16 ExecLo = 126;
constexpr u16 ExecHi = 127;
constexpr u16 Zero = 128;
constexpr u16 IntPosMax = 192; // 129..192 = 1..64
constexpr u16 IntNegMax = 208; // 193..208 = -1..-16
constexpr u16 FloatHalf = 240; // 0.5, -0.5, 1, -1, 2, -2, 4, -4 (240..247)
constexpr u16 FloatNegFour = 247;
constexpr u16 Vccz = 251;
constexpr u16 Execz = 252;
constexpr u16 Scc = 253;
constexpr u16 Literal = 255;
constexpr u16 Vgpr0 = 256;
} // namespace Operand

// ---- scalar ALU ----
#define GCN_SOP2(X)                                                                                \
    X(S_ADD_U32, 0) X(S_SUB_U32, 1) X(S_ADD_I32, 2) X(S_SUB_I32, 3) X(S_ADDC_U32, 4)               \
    X(S_SUBB_U32, 5) X(S_MIN_I32, 6) X(S_MIN_U32, 7) X(S_MAX_I32, 8) X(S_MAX_U32, 9)               \
    X(S_CSELECT_B32, 10) X(S_CSELECT_B64, 11) X(S_AND_B32, 14) X(S_AND_B64, 15)                    \
    X(S_OR_B32, 16) X(S_OR_B64, 17) X(S_XOR_B32, 18) X(S_XOR_B64, 19) X(S_ANDN2_B32, 20)           \
    X(S_ANDN2_B64, 21) X(S_ORN2_B32, 22) X(S_ORN2_B64, 23) X(S_NAND_B32, 24) X(S_NAND_B64, 25)     \
    X(S_NOR_B32, 26) X(S_NOR_B64, 27) X(S_XNOR_B32, 28) X(S_XNOR_B64, 29) X(S_LSHL_B32, 30)        \
    X(S_LSHL_B64, 31) X(S_LSHR_B32, 32) X(S_LSHR_B64, 33) X(S_ASHR_I32, 34) X(S_ASHR_I64, 35)      \
    X(S_BFM_B32, 36) X(S_BFM_B64, 37) X(S_MUL_I32, 38) X(S_BFE_U32, 39) X(S_BFE_I32, 40)           \
    X(S_BFE_U64, 41) X(S_BFE_I64, 42) X(S_CBRANCH_G_FORK, 43) X(S_ABSDIFF_I32, 44)

#define GCN_SOPK(X)                                                                                \
    X(S_MOVK_I32, 0) X(S_CMOVK_I32, 2) X(S_CMPK_EQ_I32, 3) X(S_CMPK_LG_I32, 4)                     \
    X(S_CMPK_GT_I32, 5) X(S_CMPK_GE_I32, 6) X(S_CMPK_LT_I32, 7) X(S_CMPK_LE_I32, 8)                \
    X(S_CMPK_EQ_U32, 9) X(S_CMPK_LG_U32, 10) X(S_CMPK_GT_U32, 11) X(S_CMPK_GE_U32, 12)             \
    X(S_CMPK_LT_U32, 13) X(S_CMPK_LE_U32, 14) X(S_ADDK_I32, 15) X(S_MULK_I32, 16)                  \
    X(S_CBRANCH_I_FORK, 17) X(S_GETREG_B32, 18) X(S_SETREG_B32, 19) X(S_SETREG_IMM32_B32, 21)

#define GCN_SOP1(X)                                                                                \
    X(S_MOV_B32, 3) X(S_MOV_B64, 4) X(S_CMOV_B32, 5) X(S_CMOV_B64, 6) X(S_NOT_B32, 7)              \
    X(S_NOT_B64, 8) X(S_WQM_B32, 9) X(S_WQM_B64, 10) X(S_BREV_B32, 11) X(S_BREV_B64, 12)           \
    X(S_BCNT0_I32_B32, 13) X(S_BCNT0_I32_B64, 14) X(S_BCNT1_I32_B32, 15) X(S_BCNT1_I32_B64, 16)    \
    X(S_FF0_I32_B32, 17) X(S_FF0_I32_B64, 18) X(S_FF1_I32_B32, 19) X(S_FF1_I32_B64, 20)            \
    X(S_FLBIT_I32_B32, 21) X(S_FLBIT_I32_B64, 22) X(S_FLBIT_I32, 23) X(S_FLBIT_I32_I64, 24)        \
    X(S_SEXT_I32_I8, 25) X(S_SEXT_I32_I16, 26) X(S_BITSET0_B32, 27) X(S_BITSET0_B64, 28)           \
    X(S_BITSET1_B32, 29) X(S_BITSET1_B64, 30) X(S_GETPC_B64, 31) X(S_SETPC_B64, 32)                \
    X(S_SWAPPC_B64, 33) X(S_RFE_B64, 34) X(S_AND_SAVEEXEC_B64, 36) X(S_OR_SAVEEXEC_B64, 37)        \
    X(S_XOR_SAVEEXEC_B64, 38) X(S_ANDN2_SAVEEXEC_B64, 39) X(S_ORN2_SAVEEXEC_B64, 40)               \
    X(S_NAND_SAVEEXEC_B64, 41) X(S_NOR_SAVEEXEC_B64, 42) X(S_XNOR_SAVEEXEC_B64, 43)                \
    X(S_QUADMASK_B32, 44) X(S_QUADMASK_B64, 45) X(S_MOVRELS_B32, 46) X(S_MOVRELS_B64, 47)          \
    X(S_MOVRELD_B32, 48) X(S_MOVRELD_B64, 49) X(S_CBRANCH_JOIN, 50) X(S_ABS_I32, 52)               \
    X(S_MOV_FED_B32, 53)

#define GCN_SOPC(X)                                                                                \
    X(S_CMP_EQ_I32, 0) X(S_CMP_LG_I32, 1) X(S_CMP_GT_I32, 2) X(S_CMP_GE_I32, 3)                    \
    X(S_CMP_LT_I32, 4) X(S_CMP_LE_I32, 5) X(S_CMP_EQ_U32, 6) X(S_CMP_LG_U32, 7)                    \
    X(S_CMP_GT_U32, 8) X(S_CMP_GE_U32, 9) X(S_CMP_LT_U32, 10) X(S_CMP_LE_U32, 11)                  \
    X(S_BITCMP0_B32, 12) X(S_BITCMP1_B32, 13) X(S_BITCMP0_B64, 14) X(S_BITCMP1_B64, 15)            \
    X(S_SETVSKIP, 16)

#define GCN_SOPP(X)                                                                                \
    X(S_NOP, 0) X(S_ENDPGM, 1) X(S_BRANCH, 2) X(S_CBRANCH_SCC0, 4) X(S_CBRANCH_SCC1, 5)            \
    X(S_CBRANCH_VCCZ, 6) X(S_CBRANCH_VCCNZ, 7) X(S_CBRANCH_EXECZ, 8) X(S_CBRANCH_EXECNZ, 9)        \
    X(S_BARRIER, 10) X(S_WAITCNT, 12) X(S_SETHALT, 13) X(S_SLEEP, 14) X(S_SETPRIO, 15)             \
    X(S_SENDMSG, 16) X(S_SENDMSGHALT, 17) X(S_TRAP, 18) X(S_ICACHE_INV, 19)                        \
    X(S_INCPERFLEVEL, 20) X(S_DECPERFLEVEL, 21) X(S_TTRACEDATA, 22)

#define GCN_SMRD(X)                                                                                \
    X(S_LOAD_DWORD, 0) X(S_LOAD_DWORDX2, 1) X(S_LOAD_DWORDX4, 2) X(S_LOAD_DWORDX8, 3)              \
    X(S_LOAD_DWORDX16, 4) X(S_BUFFER_LOAD_DWORD, 8) X(S_BUFFER_LOAD_DWORDX2, 9)                    \
    X(S_BUFFER_LOAD_DWORDX4, 10) X(S_BUFFER_LOAD_DWORDX8, 11) X(S_BUFFER_LOAD_DWORDX16, 12)        \
    X(S_DCACHE_INV_VOL, 29) X(S_MEMTIME, 30) X(S_DCACHE_INV, 31)

// ---- vector ALU, all in VOP3 numbering ----
#define GCN_VOP(X)                                                                                 \
    /* VOPC compares: base + condition, see CmpCond */                                             \
    X(V_CMP_F32, 0x00) X(V_CMPX_F32, 0x10) X(V_CMP_F64, 0x20) X(V_CMPX_F64, 0x30)                  \
    X(V_CMPS_F32, 0x40) X(V_CMPSX_F32, 0x50) X(V_CMPS_F64, 0x60) X(V_CMPSX_F64, 0x70)              \
    X(V_CMP_I32, 0x80) X(V_CMP_CLASS_F32, 0x88) X(V_CMPX_I32, 0x90) X(V_CMPX_CLASS_F32, 0x98)      \
    X(V_CMP_I64, 0xA0) X(V_CMP_CLASS_F64, 0xA8) X(V_CMPX_I64, 0xB0) X(V_CMPX_CLASS_F64, 0xB8)      \
    X(V_CMP_U32, 0xC0) X(V_CMPX_U32, 0xD0) X(V_CMP_U64, 0xE0) X(V_CMPX_U64, 0xF0)                  \
    /* VOP2 (256 + op) */                                                                          \
    X(V_CNDMASK_B32, 256) X(V_READLANE_B32, 257) X(V_WRITELANE_B32, 258) X(V_ADD_F32, 259)         \
    X(V_SUB_F32, 260) X(V_SUBREV_F32, 261) X(V_MAC_LEGACY_F32, 262) X(V_MUL_LEGACY_F32, 263)       \
    X(V_MUL_F32, 264) X(V_MUL_I32_I24, 265) X(V_MUL_HI_I32_I24, 266) X(V_MUL_U32_U24, 267)         \
    X(V_MUL_HI_U32_U24, 268) X(V_MIN_LEGACY_F32, 269) X(V_MAX_LEGACY_F32, 270)                     \
    X(V_MIN_F32, 271) X(V_MAX_F32, 272) X(V_MIN_I32, 273) X(V_MAX_I32, 274) X(V_MIN_U32, 275)      \
    X(V_MAX_U32, 276) X(V_LSHR_B32, 277) X(V_LSHRREV_B32, 278) X(V_ASHR_I32, 279)                  \
    X(V_ASHRREV_I32, 280) X(V_LSHL_B32, 281) X(V_LSHLREV_B32, 282) X(V_AND_B32, 283)               \
    X(V_OR_B32, 284) X(V_XOR_B32, 285) X(V_BFM_B32, 286) X(V_MAC_F32, 287) X(V_MADMK_F32, 288)     \
    X(V_MADAK_F32, 289) X(V_BCNT_U32_B32, 290) X(V_MBCNT_LO_U32_B32, 291)                          \
    X(V_MBCNT_HI_U32_B32, 292) X(V_ADD_I32, 293) X(V_SUB_I32, 294) X(V_SUBREV_I32, 295)            \
    X(V_ADDC_U32, 296) X(V_SUBB_U32, 297) X(V_SUBBREV_U32, 298) X(V_LDEXP_F32, 299)                \
    X(V_CVT_PKACCUM_U8_F32, 300) X(V_CVT_PKNORM_I16_F32, 301) X(V_CVT_PKNORM_U16_F32, 302)         \
    X(V_CVT_PKRTZ_F16_F32, 303) X(V_CVT_PK_U16_U32, 304) X(V_CVT_PK_I16_I32, 305)                  \
    /* VOP3 only */                                                                                \
    X(V_MAD_LEGACY_F32, 320) X(V_MAD_F32, 321) X(V_MAD_I32_I24, 322) X(V_MAD_U32_U24, 323)         \
    X(V_CUBEID_F32, 324) X(V_CUBESC_F32, 325) X(V_CUBETC_F32, 326) X(V_CUBEMA_F32, 327)            \
    X(V_BFE_U32, 328) X(V_BFE_I32, 329) X(V_BFI_B32, 330) X(V_FMA_F32, 331) X(V_FMA_F64, 332)      \
    X(V_LERP_U8, 333) X(V_ALIGNBIT_B32, 334) X(V_ALIGNBYTE_B32, 335) X(V_MULLIT_F32, 336)          \
    X(V_MIN3_F32, 337) X(V_MIN3_I32, 338) X(V_MIN3_U32, 339) X(V_MAX3_F32, 340)                    \
    X(V_MAX3_I32, 341) X(V_MAX3_U32, 342) X(V_MED3_F32, 343) X(V_MED3_I32, 344)                    \
    X(V_MED3_U32, 345) X(V_SAD_U8, 346) X(V_SAD_HI_U8, 347) X(V_SAD_U16, 348) X(V_SAD_U32, 349)    \
    X(V_CVT_PK_U8_F32, 350) X(V_DIV_FIXUP_F32, 351) X(V_DIV_FIXUP_F64, 352) X(V_LSHL_B64, 353)     \
    X(V_LSHR_B64, 354) X(V_ASHR_I64, 355) X(V_ADD_F64, 356) X(V_MUL_F64, 357) X(V_MIN_F64, 358)    \
    X(V_MAX_F64, 359) X(V_LDEXP_F64, 360) X(V_MUL_LO_U32, 361) X(V_MUL_HI_U32, 362)                \
    X(V_MUL_LO_I32, 363) X(V_MUL_HI_I32, 364) X(V_DIV_SCALE_F32, 365) X(V_DIV_SCALE_F64, 366)      \
    X(V_DIV_FMAS_F32, 367) X(V_DIV_FMAS_F64, 368) X(V_MSAD_U8, 369) X(V_QSAD_PK_U16_U8, 370)       \
    X(V_MQSAD_PK_U16_U8, 371) X(V_TRIG_PREOP_F64, 372) X(V_MQSAD_U32_U8, 373)                      \
    X(V_MAD_U64_U32, 374) X(V_MAD_I64_I32, 375)                                                    \
    /* VOP1 (384 + op) */                                                                          \
    X(V_NOP, 384) X(V_MOV_B32, 385) X(V_READFIRSTLANE_B32, 386) X(V_CVT_I32_F64, 387)              \
    X(V_CVT_F64_I32, 388) X(V_CVT_F32_I32, 389) X(V_CVT_F32_U32, 390) X(V_CVT_U32_F32, 391)        \
    X(V_CVT_I32_F32, 392) X(V_MOV_FED_B32, 393) X(V_CVT_F16_F32, 394) X(V_CVT_F32_F16, 395)        \
    X(V_CVT_RPI_I32_F32, 396) X(V_CVT_FLR_I32_F32, 397) X(V_CVT_OFF_F32_I4, 398)                   \
    X(V_CVT_F32_F64, 399) X(V_CVT_F64_F32, 400) X(V_CVT_F32_UBYTE0, 401)                           \
    X(V_CVT_F32_UBYTE1, 402) X(V_CVT_F32_UBYTE2, 403) X(V_CVT_F32_UBYTE3, 404)                     \
    X(V_CVT_U32_F64, 405) X(V_CVT_F64_U32, 406) X(V_TRUNC_F64, 407) X(V_CEIL_F64, 408)             \
    X(V_RNDNE_F64, 409) X(V_FLOOR_F64, 410) X(V_FRACT_F32, 416) X(V_TRUNC_F32, 417)                \
    X(V_CEIL_F32, 418) X(V_RNDNE_F32, 419) X(V_FLOOR_F32, 420) X(V_EXP_F32, 421)                   \
    X(V_LOG_CLAMP_F32, 422) X(V_LOG_F32, 423) X(V_RCP_CLAMP_F32, 424) X(V_RCP_LEGACY_F32, 425)     \
    X(V_RCP_F32, 426) X(V_RCP_IFLAG_F32, 427) X(V_RSQ_CLAMP_F32, 428) X(V_RSQ_LEGACY_F32, 429)     \
    X(V_RSQ_F32, 430) X(V_RCP_F64, 431) X(V_RCP_CLAMP_F64, 432) X(V_RSQ_F64, 433)                  \
    X(V_RSQ_CLAMP_F64, 434) X(V_SQRT_F32, 435) X(V_SQRT_F64, 436) X(V_SIN_F32, 437)                \
    X(V_COS_F32, 438) X(V_NOT_B32, 439) X(V_BFREV_B32, 440) X(V_FFBH_U32, 441)                     \
    X(V_FFBL_B32, 442) X(V_FFBH_I32, 443) X(V_FREXP_EXP_I32_F64, 444) X(V_FREXP_MANT_F64, 445)     \
    X(V_FRACT_F64, 446) X(V_FREXP_EXP_I32_F32, 447) X(V_FREXP_MANT_F32, 448) X(V_CLREXCP, 449)     \
    X(V_MOVRELD_B32, 450) X(V_MOVRELS_B32, 451) X(V_MOVRELSD_B32, 452) X(V_LOG_LEGACY_F32, 453)    \
    X(V_EXP_LEGACY_F32, 454)

#define GCN_VINTRP(X) X(V_INTERP_P1_F32, 0) X(V_INTERP_P2_F32, 1) X(V_INTERP_MOV_F32, 2)

#define GCN_MUBUF(X)                                                                               \
    X(BUFFER_LOAD_FORMAT_X, 0) X(BUFFER_LOAD_FORMAT_XY, 1) X(BUFFER_LOAD_FORMAT_XYZ, 2)            \
    X(BUFFER_LOAD_FORMAT_XYZW, 3) X(BUFFER_STORE_FORMAT_X, 4) X(BUFFER_STORE_FORMAT_XY, 5)         \
    X(BUFFER_STORE_FORMAT_XYZ, 6) X(BUFFER_STORE_FORMAT_XYZW, 7) X(BUFFER_LOAD_UBYTE, 8)           \
    X(BUFFER_LOAD_SBYTE, 9) X(BUFFER_LOAD_USHORT, 10) X(BUFFER_LOAD_SSHORT, 11)                    \
    X(BUFFER_LOAD_DWORD, 12) X(BUFFER_LOAD_DWORDX2, 13) X(BUFFER_LOAD_DWORDX4, 14)                 \
    X(BUFFER_LOAD_DWORDX3, 15) X(BUFFER_STORE_BYTE, 24) X(BUFFER_STORE_SHORT, 26)                  \
    X(BUFFER_STORE_DWORD, 28) X(BUFFER_STORE_DWORDX2, 29) X(BUFFER_STORE_DWORDX4, 30)              \
    X(BUFFER_STORE_DWORDX3, 31) X(BUFFER_ATOMIC_SWAP, 48) X(BUFFER_ATOMIC_CMPSWAP, 49)             \
    X(BUFFER_ATOMIC_ADD, 50) X(BUFFER_ATOMIC_SUB, 51) X(BUFFER_ATOMIC_SMIN, 53)                    \
    X(BUFFER_ATOMIC_UMIN, 54) X(BUFFER_ATOMIC_SMAX, 55) X(BUFFER_ATOMIC_UMAX, 56)                  \
    X(BUFFER_ATOMIC_AND, 57) X(BUFFER_ATOMIC_OR, 58) X(BUFFER_ATOMIC_XOR, 59)                      \
    X(BUFFER_ATOMIC_INC, 60) X(BUFFER_ATOMIC_DEC, 61) X(BUFFER_WBINVL1_SC, 112)                    \
    X(BUFFER_WBINVL1, 113)

#define GCN_MTBUF(X)                                                                               \
    X(TBUFFER_LOAD_FORMAT_X, 0) X(TBUFFER_LOAD_FORMAT_XY, 1) X(TBUFFER_LOAD_FORMAT_XYZ, 2)         \
    X(TBUFFER_LOAD_FORMAT_XYZW, 3) X(TBUFFER_STORE_FORMAT_X, 4) X(TBUFFER_STORE_FORMAT_XY, 5)      \
    X(TBUFFER_STORE_FORMAT_XYZ, 6) X(TBUFFER_STORE_FORMAT_XYZW, 7)

#define GCN_MIMG(X)                                                                                \
    X(IMAGE_LOAD, 0) X(IMAGE_LOAD_MIP, 1) X(IMAGE_STORE, 8) X(IMAGE_STORE_MIP, 9)                  \
    X(IMAGE_GET_RESINFO, 14) X(IMAGE_ATOMIC_ADD, 17) X(IMAGE_SAMPLE, 32) X(IMAGE_SAMPLE_CL, 33)    \
    X(IMAGE_SAMPLE_D, 34) X(IMAGE_SAMPLE_D_CL, 35) X(IMAGE_SAMPLE_L, 36) X(IMAGE_SAMPLE_B, 37)     \
    X(IMAGE_SAMPLE_B_CL, 38) X(IMAGE_SAMPLE_LZ, 39) X(IMAGE_SAMPLE_C, 40)                          \
    X(IMAGE_SAMPLE_C_CL, 41) X(IMAGE_SAMPLE_C_D, 42) X(IMAGE_SAMPLE_C_D_CL, 43)                    \
    X(IMAGE_SAMPLE_C_L, 44) X(IMAGE_SAMPLE_C_B, 45) X(IMAGE_SAMPLE_C_B_CL, 46)                     \
    X(IMAGE_SAMPLE_C_LZ, 47) X(IMAGE_SAMPLE_O, 48) X(IMAGE_SAMPLE_CL_O, 49)                        \
    X(IMAGE_SAMPLE_D_O, 50) X(IMAGE_SAMPLE_D_CL_O, 51) X(IMAGE_SAMPLE_L_O, 52)                     \
    X(IMAGE_SAMPLE_B_O, 53) X(IMAGE_SAMPLE_B_CL_O, 54) X(IMAGE_SAMPLE_LZ_O, 55)                    \
    X(IMAGE_SAMPLE_C_O, 56) X(IMAGE_SAMPLE_C_CL_O, 57) X(IMAGE_SAMPLE_C_D_O, 58)                   \
    X(IMAGE_SAMPLE_C_D_CL_O, 59) X(IMAGE_SAMPLE_C_L_O, 60) X(IMAGE_SAMPLE_C_B_O, 61)               \
    X(IMAGE_SAMPLE_C_B_CL_O, 62) X(IMAGE_SAMPLE_C_LZ_O, 63) X(IMAGE_GATHER4, 64)                   \
    X(IMAGE_GATHER4_CL, 65) X(IMAGE_GATHER4_L, 68) X(IMAGE_GATHER4_B, 69)                          \
    X(IMAGE_GATHER4_B_CL, 70) X(IMAGE_GATHER4_LZ, 71) X(IMAGE_GATHER4_C, 72)                       \
    X(IMAGE_GATHER4_C_CL, 73) X(IMAGE_GATHER4_C_L, 76) X(IMAGE_GATHER4_C_B, 77)                    \
    X(IMAGE_GATHER4_C_B_CL, 78) X(IMAGE_GATHER4_C_LZ, 79) X(IMAGE_GATHER4_O, 80)                   \
    X(IMAGE_GATHER4_LZ_O, 87) X(IMAGE_GATHER4_C_LZ_O, 95) X(IMAGE_GET_LOD, 96)

#define GCN_DS(X)                                                                                  \
    X(DS_ADD_U32, 0) X(DS_SUB_U32, 1) X(DS_MIN_I32, 5) X(DS_MAX_I32, 6) X(DS_MIN_U32, 7)           \
    X(DS_MAX_U32, 8) X(DS_AND_B32, 9) X(DS_OR_B32, 10) X(DS_XOR_B32, 11) X(DS_WRITE_B32, 13)       \
    X(DS_WRITE2_B32, 14) X(DS_WRITE2ST64_B32, 15) X(DS_WRITE_B8, 30) X(DS_WRITE_B16, 31)           \
    X(DS_ADD_RTN_U32, 32) X(DS_SWIZZLE_B32, 53) X(DS_READ_B32, 54) X(DS_READ2_B32, 55)             \
    X(DS_READ2ST64_B32, 56) X(DS_READ_I8, 57) X(DS_READ_U8, 58) X(DS_READ_I16, 59)                 \
    X(DS_READ_U16, 60) X(DS_CONSUME, 61) X(DS_APPEND, 62) X(DS_WRITE_B64, 77)                      \
    X(DS_WRITE2_B64, 78) X(DS_READ_B64, 118) X(DS_READ2_B64, 119) X(DS_WRITE_B96, 222)             \
    X(DS_WRITE_B128, 223) X(DS_READ_B96, 254) X(DS_READ_B128, 255)

#define GCN_ENUM_ENTRY(name, value) name = value,
enum class Sop2 : u16 { GCN_SOP2(GCN_ENUM_ENTRY) };
enum class Sopk : u16 { GCN_SOPK(GCN_ENUM_ENTRY) };
enum class Sop1 : u16 { GCN_SOP1(GCN_ENUM_ENTRY) };
enum class Sopc : u16 { GCN_SOPC(GCN_ENUM_ENTRY) };
enum class Sopp : u16 { GCN_SOPP(GCN_ENUM_ENTRY) };
enum class Smrd : u16 { GCN_SMRD(GCN_ENUM_ENTRY) };
enum class Vop : u16 { GCN_VOP(GCN_ENUM_ENTRY) };
enum class Vintrp : u16 { GCN_VINTRP(GCN_ENUM_ENTRY) };
enum class Mubuf : u16 { GCN_MUBUF(GCN_ENUM_ENTRY) };
enum class Mtbuf : u16 { GCN_MTBUF(GCN_ENUM_ENTRY) };
enum class Mimg : u16 { GCN_MIMG(GCN_ENUM_ENTRY) };
enum class Ds : u16 { GCN_DS(GCN_ENUM_ENTRY) };
#undef GCN_ENUM_ENTRY

// Comparison condition in the low 3/4 bits of a VOPC opcode.
// Float: F LT EQ LE GT LG GE O U NGE NLG NGT NLE NEQ NLT TRU (16 conditions).
// Integer: F LT EQ LE GT NE GE T (8 conditions).

// EXP targets.
namespace ExpTarget {
constexpr u8 Mrt0 = 0;
constexpr u8 MrtZ = 8;
constexpr u8 Null = 9;
constexpr u8 Pos0 = 12;
constexpr u8 Param0 = 32;
} // namespace ExpTarget

struct Inst {
    u32 pc = 0; // byte offset inside the program
    Enc enc = Enc::Unknown;
    u8 size = 1; // dwords, including a literal
    u16 op = 0;  // per-encoding opcode; VALU encodings use VOP3 numbering

    // ALU operands (9-bit codes); `dst` is an SGPR code for SALU, a VGPR
    // index for VALU. `sdst` is the scalar mask destination of VOP3b /
    // VOPC-in-VOP3 / carry-out ops (VCC for the 32-bit forms).
    u16 dst = 0;
    u16 sdst = 0;
    u16 src[3] = {0, 0, 0};
    u32 literal = 0;
    s16 simm16 = 0;

    // VOP3 modifiers.
    u8 abs = 0; // bit per source
    u8 neg = 0; // bit per source
    u8 omod = 0;
    bool clamp = false;
    bool vop3 = false; // came from the 64-bit encoding

    // SMRD.
    u8 sbase = 0; // SGPR index (already doubled)
    bool imm = false;
    u32 smrd_offset = 0; // dwords if imm, else SGPR code

    // VINTRP.
    u8 attr = 0;
    u8 chan = 0;

    // Memory (MUBUF / MTBUF / MIMG / DS).
    u16 offset = 0;
    bool offen = false;
    bool idxen = false;
    bool glc = false;
    bool slc = false;
    bool addr64 = false;
    bool lds = false;
    u8 dfmt = 0;
    u8 nfmt = 0;
    u8 vaddr = 0;
    u8 vdata = 0;
    u8 srsrc = 0; // SGPR index (already * 4)
    u8 ssamp = 0; // SGPR index (already * 4)
    u16 soffset = 0;
    u8 dmask = 0;
    bool unorm = false;
    bool da = false;
    bool r128 = false;
    bool tfe = false;
    bool lwe = false;
    u8 offset0 = 0, offset1 = 0; // DS
    bool gds = false;
    u8 data0 = 0, data1 = 0;     // DS

    // EXP.
    u8 en = 0;
    u8 target = 0;
    bool compr = false;
    bool done = false;
    bool vm = false;
    u8 vsrc[4] = {0, 0, 0, 0};
};

// Decodes one instruction at `code`; returns false on an unknown encoding.
bool Decode(const u32* code, u32 pc, Inst& out);

// Decodes a whole program starting at `code`: runs until an S_ENDPGM that no
// branch jumps past (or `max_dwords`). With `stop_at_setpc`, an S_SETPC_B64
// also ends it (fetch subroutines return that way).
std::vector<Inst> DecodeProgram(const u32* code, u32 max_dwords = 1u << 16,
                                bool stop_at_setpc = false);

// Byte length of the program at `code` (as DecodeProgram sees it).
u32 ProgramSize(const u32* code);

const char* OpName(Enc enc, u16 op);
std::string Disassemble(const Inst& inst);

// Branch target (byte offset) of an SOPP branch.
inline u32 BranchTarget(const Inst& i) {
    return static_cast<u32>(static_cast<s32>(i.pc) + 4 + i.simm16 * 4);
}

inline bool IsBranch(const Inst& i) {
    if (i.enc != Enc::SOPP) {
        return false;
    }
    const auto op = static_cast<Sopp>(i.op);
    return op == Sopp::S_BRANCH || (op >= Sopp::S_CBRANCH_SCC0 && op <= Sopp::S_CBRANCH_EXECNZ);
}

} // namespace Gpu::Shader::Gcn
