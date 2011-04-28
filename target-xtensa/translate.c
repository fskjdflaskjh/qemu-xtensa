/*
 * Xtensa ISA:
 * http://www.tensilica.com/products/literature-docs/documentation/xtensa-isa-databook.htm
 *
 * Copyright (c) 2011, Max Filippov, Motorola Solutions, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Motorola Solutions nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>

#include "cpu.h"
#include "exec-all.h"
#include "disas.h"
#include "tcg-op.h"
#include "qemu-log.h"

#include "helpers.h"
#define GEN_HELPER 1
#include "helpers.h"

typedef struct DisasContext {
    const XtensaConfig *config;
    TranslationBlock *tb;
    uint32_t pc;
    uint32_t lend;
    int is_jmp;
    int singlestep_enabled;
    unsigned used_window;
} DisasContext;

static TCGv_ptr cpu_env;
static TCGv_i32 cpu_pc;
static TCGv_i32 cpu_R[16];
static TCGv_i32 cpu_SR[256];
static TCGv_i32 cpu_UR[256];

#include "gen-icount.h"

static const char * const sregnames[256] = {
    [LBEG] = "LBEG",
    [LEND] = "LEND",
    [LCOUNT] = "LCOUNT",
    [SAR] = "SAR",
    [LITBASE] = "LITBASE",
    [SCOMPARE1] = "SCOMPARE1",
    [WINDOW_BASE] = "WINDOW_BASE",
    [WINDOW_START] = "WINDOW_START",
    [EPC1] = "EPC1",
    [EPC1 + 1] = "EPC2",
    [EPC1 + 2] = "EPC3",
    [EPC1 + 3] = "EPC4",
    [EPC1 + 4] = "EPC5",
    [EPC1 + 5] = "EPC6",
    [EPC1 + 6] = "EPC7",
    [DEPC] = "DEPC",
    [EPS2] = "EPS2",
    [EPS2 + 1] = "EPS3",
    [EPS2 + 2] = "EPS4",
    [EPS2 + 3] = "EPS5",
    [EPS2 + 4] = "EPS6",
    [EPS2 + 5] = "EPS7",
    [EXCSAVE1] = "EXCSAVE1",
    [EXCSAVE1 + 1] = "EXCSAVE2",
    [EXCSAVE1 + 2] = "EXCSAVE3",
    [EXCSAVE1 + 3] = "EXCSAVE4",
    [EXCSAVE1 + 4] = "EXCSAVE5",
    [EXCSAVE1 + 5] = "EXCSAVE6",
    [EXCSAVE1 + 6] = "EXCSAVE7",
    [CPENABLE] = "CPENABLE",
    [INTSET] = "INTSET",
    [INTCLEAR] = "INTCLEAR",
    [INTENABLE] = "INTENABLE",
    [PS] = "PS",
    [EXCCAUSE] = "EXCCAUSE",
    [CCOUNT] = "CCOUNT",
    [PRID] = "PRID",
    [EXCVADDR] = "EXCVADDR",
    [CCOMPARE] = "CCOMPARE0",
    [CCOMPARE + 1] = "CCOMPARE1",
    [CCOMPARE + 2] = "CCOMPARE2",
};

static const char * const uregnames[256] = {
    [THREADPTR] = "THREADPTR",
    [FCR] = "FCR",
    [FSR] = "FSR",
};

void xtensa_translate_init(void)
{
    static const char * const regnames[] = {
        "ar0", "ar1", "ar2", "ar3",
        "ar4", "ar5", "ar6", "ar7",
        "ar8", "ar9", "ar10", "ar11",
        "ar12", "ar13", "ar14", "ar15",
    };
    int i;

    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");
    cpu_pc = tcg_global_mem_new_i32(TCG_AREG0,
            offsetof(CPUState, pc), "pc");

    for (i = 0; i < 16; i++) {
        cpu_R[i] = tcg_global_mem_new_i32(TCG_AREG0,
                offsetof(CPUState, regs[i]),
                regnames[i]);
    }

    for (i = 0; i < 256; ++i) {
        if (sregnames[i]) {
            cpu_SR[i] = tcg_global_mem_new_i32(TCG_AREG0,
                    offsetof(CPUState, sregs[i]),
                    sregnames[i]);
        }
    }

    for (i = 0; i < 256; ++i) {
        if (uregnames[i]) {
            cpu_UR[i] = tcg_global_mem_new_i32(TCG_AREG0,
                    offsetof(CPUState, uregs[i]),
                    uregnames[i]);
        }
    }
}

static inline int option_enabled(DisasContext *dc, int opt)
{
    return xtensa_option_enabled(dc->config, opt);
}

static void gen_check_interrupts(DisasContext *dc)
{
    tcg_gen_movi_i32(cpu_pc, dc->pc);
    gen_helper_check_interrupts();
}

static void reset_used_window(DisasContext *dc)
{
    dc->used_window = 0;
}

static void gen_rsr(TCGv_i32 d, int sr)
{
    if (sregnames[sr]) {
        tcg_gen_mov_i32(d, cpu_SR[sr]);
    } else {
        printf("SR %d not implemented, ", sr);
    }
}

static void gen_wsr_lend(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
    gen_helper_wsr_lend(v);
}

static void gen_wsr_windowbase(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
    gen_helper_wsr_windowbase(v);
    reset_used_window(dc);
}

static void gen_wsr_windowstart(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
    tcg_gen_mov_i32(cpu_SR[sr], v);
    reset_used_window(dc);
}

static void gen_wsr_ps(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
    tcg_gen_mov_i32(cpu_SR[sr], v);
    reset_used_window(dc);
    gen_check_interrupts(dc);
}

static void gen_wsr_prid(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
}

static void gen_wsr_ccompare(DisasContext *dc, uint32_t sr, TCGv_i32 v)
{
    TCGv_i32 id = tcg_const_i32(sr - CCOMPARE);
    TCGv_i32 active = tcg_const_i32(0);
    tcg_gen_mov_i32(cpu_SR[sr], v);
    gen_helper_timer_irq(id, active);
    tcg_temp_free(id);
    tcg_temp_free(active);
}

static void gen_wsr(DisasContext *dc, uint32_t sr, TCGv_i32 s)
{
    static void (* const wsr_handler[256])(DisasContext *dc,
            uint32_t sr, TCGv_i32 v) = {
        [LEND] = gen_wsr_lend,
        [WINDOW_BASE] = gen_wsr_windowbase,
        [WINDOW_START] = gen_wsr_windowstart,
        [PS] = gen_wsr_ps,
        [PRID] = gen_wsr_prid,
        [CCOMPARE] = gen_wsr_ccompare,
        [CCOMPARE + 1] = gen_wsr_ccompare,
        [CCOMPARE + 2] = gen_wsr_ccompare,
    };

    if (sregnames[sr]) {
        if (wsr_handler[sr]) {
            wsr_handler[sr](dc, sr, s);
        } else {
            tcg_gen_mov_i32(cpu_SR[sr], s);
        }
    } else {
        printf("SR %d not implemented, ", sr);
    }
}

static void gen_exception(int excp)
{
    TCGv_i32 tmp = tcg_const_i32(excp);
    gen_helper_exception(tmp);
    tcg_temp_free(tmp);
}

static void gen_exception_cause(DisasContext *dc, uint32_t cause)
{
    TCGv_i32 _pc = tcg_const_i32(dc->pc);
    TCGv_i32 _cause = tcg_const_i32(cause);
    gen_helper_exception_cause(_pc, _cause);
    tcg_temp_free(_pc);
    tcg_temp_free(_cause);
}

static void gen_exception_cause_vaddr(DisasContext *dc, uint32_t cause,
        TCGv_i32 vaddr)
{
    TCGv_i32 _pc = tcg_const_i32(dc->pc);
    TCGv_i32 _cause = tcg_const_i32(cause);
    gen_helper_exception_cause_vaddr(_pc, _cause, vaddr);
    tcg_temp_free(_pc);
    tcg_temp_free(_cause);
}

static void gen_check_privilege(DisasContext *dc)
{
    if (option_enabled(dc, XTENSA_OPTION_MMU)) {
        TCGv_i32 tmp = tcg_temp_new_i32();
        int label = gen_new_label();

        tcg_gen_andi_i32(tmp, cpu_SR[PS], PS_EXCM);
        tcg_gen_brcondi_i32(TCG_COND_EQ, tmp, PS_EXCM, label);
        tcg_gen_andi_i32(tmp, cpu_SR[PS], PS_RING);
        tcg_gen_brcondi_i32(TCG_COND_GEU, tmp, 0, label);

        gen_exception_cause(dc, PRIVILEGED_CAUSE);

        gen_set_label(label);
        tcg_temp_free(tmp);
    }
}

static void gen_jump(DisasContext *dc, TCGv dest)
{
    tcg_gen_mov_i32(cpu_pc, dest);
    if (dc->singlestep_enabled) {
        gen_exception(EXCP_DEBUG);
    }
    tcg_gen_exit_tb(0);
    dc->is_jmp = DISAS_UPDATE;
}

static void gen_jumpi(DisasContext *dc, uint32_t dest)
{
    TCGv_i32 tmp = tcg_const_i32(dest);
    gen_jump(dc, tmp);
    tcg_temp_free(tmp);
}

static void gen_callw(DisasContext *dc, int _callinc, TCGv_i32 target)
{
    TCGv_i32 callinc = tcg_const_i32(_callinc);

    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS],
            callinc, PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_temp_free(callinc);
    tcg_gen_movi_i32(cpu_R[_callinc << 2],
            (_callinc << 30) | ((dc->pc + 3) & 0x3fffffff));
    gen_jump(dc, target);
}

static void gen_check_loop_end(DisasContext *dc)
{
    if (option_enabled(dc, XTENSA_OPTION_LOOP) &&
            dc->pc == dc->lend) {
        TCGv_i32 tmp = tcg_temp_new_i32();
        int label = gen_new_label();

        tcg_gen_andi_i32(tmp, cpu_SR[PS], PS_EXCM);
        tcg_gen_brcondi_i32(TCG_COND_EQ, tmp, PS_EXCM, label);
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_SR[LEND], dc->pc, label);
        tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_SR[LCOUNT], 0, label);
        tcg_gen_subi_i32(cpu_SR[LCOUNT], cpu_SR[LCOUNT], 1);
        gen_jump(dc, cpu_SR[LBEG]);
        gen_set_label(label);
        gen_jumpi(dc, dc->pc);
        tcg_temp_free(tmp);
    }
}

static void gen_jumpi_check_loop_end(DisasContext *dc, uint32_t dest)
{
    dc->pc = dest;
    gen_check_loop_end(dc);
    gen_jumpi(dc, dest);
}

static void gen_load_store_alignment(DisasContext *dc, int shift, TCGv_i32 addr)
{
    TCGv_i32 tmp = tcg_temp_local_new_i32();
    tcg_gen_mov_i32(tmp, addr);
    tcg_gen_andi_i32(addr, addr, ~0 << shift);
    if (option_enabled(dc, XTENSA_OPTION_UNALIGNED_EXCEPTION)) {
        int label = gen_new_label();
        tcg_gen_brcond_i32(TCG_COND_EQ, addr, tmp, label);
        gen_exception_cause_vaddr(dc, LOAD_STORE_ALIGNMENT_CAUSE, tmp);
        gen_set_label(label);
    }
    tcg_temp_free(tmp);
}

static void gen_waiti(DisasContext *dc, uint32_t imm4)
{
    TCGv_i32 pc = tcg_const_i32(dc->pc);
    TCGv_i32 intlevel = tcg_const_i32(imm4);
    gen_helper_waiti(pc, intlevel);
    tcg_temp_free(pc);
    tcg_temp_free(intlevel);
}

static void gen_window_check1(DisasContext *dc, unsigned r1)
{
    if (option_enabled(dc, XTENSA_OPTION_WINDOWED_REGISTER) &&
            r1 / 4 > dc->used_window) {
        TCGv_i32 pc = tcg_const_i32(dc->pc);
        TCGv_i32 w = tcg_const_i32(r1 / 4);

        gen_helper_window_check(pc, w);

        tcg_temp_free(w);
        tcg_temp_free(pc);
    }
}

static void gen_window_check2(DisasContext *dc, unsigned r1, unsigned r2)
{
    gen_window_check1(dc, r1 > r2 ? r1 : r2);
}

static void gen_window_check3(DisasContext *dc, unsigned r1, unsigned r2,
        unsigned r3)
{
    gen_window_check2(dc, r1, r2 > r3 ? r2 : r3);
}

static void disas_xtensa_insn(DisasContext *dc)
{
#define HAS_OPTION(opt) do { \
        if (!option_enabled(dc, opt)) { \
            goto invalid_opcode; \
        } \
    } while (0)

#define TBD() printf("TBD(pc = %08x): %s:%d\n", dc->pc, __FILE__, __LINE__)
#define RESERVED() do { \
        printf("RESERVED(pc = %08x, %02x%02x%02x): %s:%d\n", \
                dc->pc, _b0, _b1, _b2, __FILE__, __LINE__); \
        goto invalid_opcode; \
    } while (0)


#ifdef TARGET_WORDS_BIGENDIAN
#define _OP0 (((_b0) & 0xf0) >> 4)
#define _OP1 (((_b2) & 0xf0) >> 4)
#define _OP2 ((_b2) & 0xf)
#define RRR_R ((_b1) & 0xf)
#define RRR_S (((_b1) & 0xf0) >> 4)
#define RRR_T ((_b0) & 0xf)
#else
#define _OP0 (((_b0) & 0xf))
#define _OP1 (((_b2) & 0xf))
#define _OP2 (((_b2) & 0xf0) >> 4)
#define RRR_R (((_b1) & 0xf0) >> 4)
#define RRR_S (((_b1) & 0xf))
#define RRR_T (((_b0) & 0xf0) >> 4)
#endif

#define RRRN_R RRR_R
#define RRRN_S RRR_S
#define RRRN_T RRR_T

#define RRI8_R RRR_R
#define RRI8_S RRR_S
#define RRI8_T RRR_T
#define RRI8_IMM8 (_b2)
#define RRI8_IMM8_SE ((((_b2) & 0x80) ? 0xffffff00 : 0) | RRI8_IMM8)

#ifdef TARGET_WORDS_BIGENDIAN
#define RI16_IMM16 (((_b1) << 8) | (_b2))
#else
#define RI16_IMM16 (((_b2) << 8) | (_b1))
#endif

#ifdef TARGET_WORDS_BIGENDIAN
#define CALL_N (((_b0) & 0xc) >> 2)
#define CALL_OFFSET ((((_b0) & 0x3) << 16) | ((_b1) << 8) | (_b2))
#else
#define CALL_N (((_b0) & 0x30) >> 4)
#define CALL_OFFSET ((((_b0) & 0xc0) >> 6) | ((_b1) << 2) | ((_b2) << 10))
#endif
#define CALL_OFFSET_SE \
    (((CALL_OFFSET & 0x20000) ? 0xfffc0000 : 0) | CALL_OFFSET)

#define CALLX_N CALL_N
#ifdef TARGET_WORDS_BIGENDIAN
#define CALLX_M ((_b0) & 0x3)
#else
#define CALLX_M (((_b0) & 0xc0) >> 6)
#endif
#define CALLX_S RRR_S

#define BRI12_M CALLX_M
#define BRI12_S RRR_S
#ifdef TARGET_WORDS_BIGENDIAN
#define BRI12_IMM12 ((((_b1) & 0xf) << 8) | (_b2))
#else
#define BRI12_IMM12 ((((_b1) & 0xf0) >> 4) | ((_b2) << 4))
#endif
#define BRI12_IMM12_SE (((BRI12_IMM12 & 0x800) ? 0xfffff000 : 0) | BRI12_IMM12)

#define BRI8_M BRI12_M
#define BRI8_R RRI8_R
#define BRI8_S RRI8_S
#define BRI8_IMM8 RRI8_IMM8
#define BRI8_IMM8_SE RRI8_IMM8_SE

#define RSR_SR (_b1)

    uint8_t _b0 = ldub_code(dc->pc);
    uint8_t _b1 = ldub_code(dc->pc + 1);
    uint8_t _b2 = ldub_code(dc->pc + 2);

    static const uint32_t B4CONST[] = {
        0xffffffff, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256
    };

    static const uint32_t B4CONSTU[] = {
        32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256
    };

    switch (_OP0) {
    case 0: /*QRST*/
        switch (_OP1) {
        case 0: /*RST0*/
            switch (_OP2) {
            case 0: /*ST0*/
                if ((RRR_R & 0xc) == 0x8) {
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                }

                switch (RRR_R) {
                case 0: /*SNM0*/
                    switch (CALLX_M) {
                    case 0: /*ILL*/
                        gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
                        break;

                    case 1: /*reserved*/
                        RESERVED();
                        break;

                    case 2: /*JR*/
                        switch (CALLX_N) {
                        case 0: /*RET*/
                        case 2: /*JX*/
                            gen_window_check1(dc, CALLX_S);
                            gen_jump(dc, cpu_R[CALLX_S]);
                            break;

                        case 1: /*RETWw*/
                            HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                            {
                                TCGv_i32 tmp = tcg_const_i32(dc->pc);
                                gen_helper_retw(tmp, tmp);
                                gen_jump(dc, tmp);
                                tcg_temp_free(tmp);
                            }
                            break;

                        case 3: /*reserved*/
                            RESERVED();
                            break;
                        }
                        break;

                    case 3: /*CALLX*/
                        gen_window_check2(dc, CALLX_S, CALLX_N << 2);
                        switch (CALLX_N) {
                        case 0: /*CALLX0*/
                            {
                                TCGv_i32 tmp = tcg_temp_new_i32();
                                tcg_gen_mov_i32(tmp, cpu_R[CALLX_S]);
                                tcg_gen_movi_i32(cpu_R[0], dc->pc + 3);
                                gen_jump(dc, tmp);
                                tcg_temp_free(tmp);
                            }
                            break;

                        case 1: /*CALLX4w*/
                        case 2: /*CALLX8w*/
                        case 3: /*CALLX12w*/
                            HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                            {
                                TCGv_i32 tmp = tcg_temp_new_i32();

                                tcg_gen_mov_i32(tmp, cpu_R[CALLX_S]);
                                gen_callw(dc, CALLX_N, tmp);
                                tcg_temp_free(tmp);
                            }
                            break;
                        }
                        break;
                    }
                    break;

                case 1: /*MOVSPw*/
                    HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                    gen_window_check2(dc, RRR_T, RRR_S);
                    {
                        TCGv_i32 pc = tcg_const_i32(dc->pc);
                        gen_helper_movsp(pc);
                        tcg_gen_mov_i32(cpu_R[RRR_T], cpu_R[RRR_S]);
                        tcg_temp_free(pc);
                    }
                    break;

                case 2: /*SYNC*/
                    switch (RRR_T) {
                    case 0: /*ISYNC*/
                        break;

                    case 1: /*RSYNC*/
                        break;

                    case 2: /*ESYNC*/
                        break;

                    case 3: /*DSYNC*/
                        break;

                    case 8: /*EXCW*/
                        HAS_OPTION(XTENSA_OPTION_EXCEPTION);
                        break;

                    case 12: /*MEMW*/
                        break;

                    case 13: /*EXTW*/
                        break;

                    case 15: /*NOP*/
                        break;

                    default: /*reserved*/
                        RESERVED();
                        break;
                    }
                    break;

                case 3: /*RFEIx*/
                    switch (RRR_T) {
                    case 0: /*RFETx*/
                        HAS_OPTION(XTENSA_OPTION_EXCEPTION);
                        switch (RRR_S) {
                        case 0: /*RFEx*/
                            gen_check_privilege(dc);
                            tcg_gen_andi_i32(cpu_SR[PS], cpu_SR[PS], ~PS_EXCM);
                            gen_jump(dc, cpu_SR[EPC1]);
                            break;

                        case 1: /*RFUEx*/
                            RESERVED();
                            break;

                        case 2: /*RFDEx*/
                            gen_check_privilege(dc);
                            gen_jump(dc, cpu_SR[
                                    dc->config->ndepc ? DEPC : EPC1]);
                            break;

                        case 4: /*RFWOw*/
                        case 5: /*RFWUw*/
                            HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                            gen_check_privilege(dc);
                            {
                                TCGv_i32 tmp = tcg_const_i32(1);

                                tcg_gen_andi_i32(
                                        cpu_SR[PS], cpu_SR[PS], ~PS_EXCM);
                                tcg_gen_shl_i32(tmp, tmp, cpu_SR[WINDOW_BASE]);

                                if (RRR_S == 4) {
                                    tcg_gen_andc_i32(cpu_SR[WINDOW_START],
                                            cpu_SR[WINDOW_START], tmp);
                                } else {
                                    tcg_gen_or_i32(cpu_SR[WINDOW_START],
                                            cpu_SR[WINDOW_START], tmp);
                                }

                                gen_helper_restore_owb();
                                gen_jump(dc, cpu_SR[EPC1]);

                                tcg_temp_free(tmp);
                            }
                            break;

                        default: /*reserved*/
                            RESERVED();
                            break;
                        }
                        break;

                    case 1: /*RFIx*/
                        HAS_OPTION(XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT);
                        gen_check_privilege(dc);
                        tcg_gen_mov_i32(cpu_SR[PS], cpu_SR[EPS2 + RRR_S - 2]);
                        gen_jump(dc, cpu_SR[EPC1 + RRR_S - 1]);
                        break;

                    case 2: /*RFME*/
                        TBD();
                        break;

                    default: /*reserved*/
                        RESERVED();
                        break;

                    }
                    break;

                case 4: /*BREAKx*/
                    HAS_OPTION(XTENSA_OPTION_EXCEPTION);
                    TBD();
                    break;

                case 5: /*SYSCALLx*/
                    HAS_OPTION(XTENSA_OPTION_EXCEPTION);
                    switch (RRR_S) {
                    case 0: /*SYSCALLx*/
                        gen_exception_cause(dc, SYSCALL_CAUSE);
                        break;

                    case 1: /*SIMCALL*/
                        gen_helper_simcall();
                        break;

                    default:
                        RESERVED();
                        break;
                    }
                    break;

                case 6: /*RSILx*/
                    HAS_OPTION(XTENSA_OPTION_INTERRUPT);
                    gen_check_privilege(dc);
                    gen_window_check1(dc, RRR_T);
                    tcg_gen_mov_i32(cpu_R[RRR_T], cpu_SR[PS]);
                    tcg_gen_ori_i32(cpu_SR[PS], cpu_SR[PS], RRR_S);
                    tcg_gen_andi_i32(cpu_SR[PS], cpu_SR[PS],
                            RRR_S | ~PS_INTLEVEL);
                    gen_check_interrupts(dc);
                    break;

                case 7: /*WAITIx*/
                    HAS_OPTION(XTENSA_OPTION_INTERRUPT);
                    gen_check_privilege(dc);
                    gen_waiti(dc, RRR_S);
                    break;

                case 8: /*ANY4p*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                case 9: /*ALL4p*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                case 10: /*ANY8p*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                case 11: /*ALL8p*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;

                }
                break;

            case 1: /*AND*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                tcg_gen_and_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 2: /*OR*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                tcg_gen_or_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 3: /*XOR*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                tcg_gen_xor_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 4: /*ST1*/
                switch (RRR_R) {
                case 0: /*SSR*/
                    gen_window_check1(dc, RRR_S);
                    tcg_gen_andi_i32(cpu_SR[SAR], cpu_R[RRR_S], 0x1f);
                    break;

                case 1: /*SSL*/
                    gen_window_check1(dc, RRR_S);
                    {
                        TCGv_i32 base = tcg_const_i32(32);
                        TCGv_i32 tmp = tcg_temp_new_i32();
                        tcg_gen_andi_i32(tmp, cpu_R[RRR_S], 0x1f);
                        tcg_gen_sub_i32(cpu_SR[SAR], base, tmp);
                        tcg_temp_free(tmp);
                        tcg_temp_free(base);
                    }
                    break;

                case 2: /*SSA8L*/
                    gen_window_check1(dc, RRR_S);
                    {
                        TCGv_i32 tmp = tcg_temp_new_i32();
                        tcg_gen_andi_i32(tmp, cpu_R[RRR_S], 0x3);
                        tcg_gen_shli_i32(cpu_SR[SAR], tmp, 3);
                        tcg_temp_free(tmp);
                    }
                    break;

                case 3: /*SSA8B*/
                    gen_window_check1(dc, RRR_S);
                    {
                        TCGv_i32 base = tcg_const_i32(32);
                        TCGv_i32 tmp = tcg_temp_new_i32();
                        tcg_gen_andi_i32(tmp, cpu_R[RRR_S], 0x3);
                        tcg_gen_shli_i32(tmp, tmp, 3);
                        tcg_gen_sub_i32(cpu_SR[SAR], base, tmp);
                        tcg_temp_free(tmp);
                        tcg_temp_free(base);
                    }
                    break;

                case 4: /*SSAI*/
                    tcg_gen_movi_i32(cpu_SR[SAR], RRR_S | ((RRR_T & 1) << 4));
                    break;

                case 6: /*RER*/
                    TBD();
                    break;

                case 7: /*WER*/
                    TBD();
                    break;

                case 8: /*ROTWw*/
                    HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                    gen_check_privilege(dc);
                    {
                        TCGv_i32 tmp = tcg_const_i32(
                                RRR_T | ((RRR_T & 8) ? 0xfffffff0 : 0));
                        gen_helper_rotw(tmp);
                        tcg_temp_free(tmp);
                        reset_used_window(dc);
                    }
                    break;

                case 14: /*NSAu*/
                    HAS_OPTION(XTENSA_OPTION_MISC_OP);
                    TBD();
                    break;

                case 15: /*NSAUu*/
                    HAS_OPTION(XTENSA_OPTION_MISC_OP);
                    gen_window_check2(dc, RRR_S, RRR_T);
                    {
#define gen_bit_bisect(w) do { \
        int label = gen_new_label(); \
        tcg_gen_brcondi_i32(TCG_COND_LTU, tmp, 1 << (w), label); \
        tcg_gen_shri_i32(tmp, tmp, (w)); \
        tcg_gen_subi_i32(res, res, (w)); \
        gen_set_label(label); \
    } while (0)

                        int label = gen_new_label();
                        TCGv_i32 res = tcg_temp_local_new_i32();

                        tcg_gen_movi_i32(res, 32);
                        tcg_gen_brcondi_i32(
                                TCG_COND_EQ, cpu_R[RRR_S], 0, label);
                        {
                            TCGv_i32 tmp = tcg_temp_local_new_i32();
                            tcg_gen_mov_i32(tmp, cpu_R[RRR_S]);
                            tcg_gen_movi_i32(res, 31);

                            gen_bit_bisect(16);
                            gen_bit_bisect(8);
                            gen_bit_bisect(4);
                            gen_bit_bisect(2);
                            gen_bit_bisect(1);

                            tcg_temp_free(tmp);
                        }
                        gen_set_label(label);
                        tcg_gen_mov_i32(cpu_R[RRR_T], res);
                        tcg_temp_free(res);
#undef gen_bit_bisect
                    }
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;
                }
                break;

            case 5: /*TLB*/
                TBD();
                break;

            case 6: /*RT0*/
                gen_window_check2(dc, RRR_R, RRR_T);
                switch (RRR_S) {
                case 0: /*NEG*/
                    tcg_gen_neg_i32(cpu_R[RRR_R], cpu_R[RRR_T]);
                    break;

                case 1: /*ABS*/
                    {
                        int label = gen_new_label();
                        tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_T]);
                        tcg_gen_brcondi_i32(
                                TCG_COND_GE, cpu_R[RRR_R], 0, label);
                        tcg_gen_neg_i32(cpu_R[RRR_R], cpu_R[RRR_T]);
                        gen_set_label(label);
                    }
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;
                }
                break;

            case 7: /*reserved*/
                RESERVED();
                break;

            case 8: /*ADD*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                tcg_gen_add_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 9: /*ADD**/
            case 10:
            case 11:
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_shli_i32(tmp, cpu_R[RRR_S], _OP2 - 8);
                    tcg_gen_add_i32(cpu_R[RRR_R], tmp, cpu_R[RRR_T]);
                    tcg_temp_free(tmp);
                }
                break;

            case 12: /*SUB*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                tcg_gen_sub_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 13: /*SUB**/
            case 14:
            case 15:
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_shli_i32(tmp, cpu_R[RRR_S], _OP2 - 12);
                    tcg_gen_sub_i32(cpu_R[RRR_R], tmp, cpu_R[RRR_T]);
                    tcg_temp_free(tmp);
                }
                break;
            }
            break;

        case 1: /*RST1*/
            switch (_OP2) {
            case 0: /*SLLI*/
            case 1:
                gen_window_check2(dc, RRR_R, RRR_S);
                tcg_gen_shli_i32(cpu_R[RRR_R], cpu_R[RRR_S],
                        32 - (RRR_T | ((_OP2 & 1) << 4)));
                break;

            case 2: /*SRAI*/
            case 3:
                gen_window_check2(dc, RRR_R, RRR_T);
                tcg_gen_sari_i32(cpu_R[RRR_R], cpu_R[RRR_T],
                        RRR_S | ((_OP2 & 1) << 4));
                break;

            case 4: /*SRLI*/
                gen_window_check2(dc, RRR_R, RRR_T);
                tcg_gen_shri_i32(cpu_R[RRR_R], cpu_R[RRR_T], RRR_S);
                break;

            case 6: /*XSR*/
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    if (RSR_SR >= 64) {
                        gen_check_privilege(dc);
                    }
                    gen_window_check1(dc, RRR_T);
                    tcg_gen_mov_i32(tmp, cpu_R[RRR_T]);
                    gen_rsr(cpu_R[RRR_T], RSR_SR);
                    gen_wsr(dc, RSR_SR, tmp);
                    tcg_temp_free(tmp);
                    if (!sregnames[RSR_SR]) {
                        TBD();
                    }
                }
                break;

#define gen_shift_reg(cmd, reg) do { \
                    TCGv_i64 tmp = tcg_temp_new_i64(); \
                    tcg_gen_extu_i32_i64(tmp, reg); \
                    tcg_gen_andi_i64(tmp, tmp, 63); \
                    tcg_gen_##cmd##_i64(v, v, tmp); \
                    tcg_gen_trunc_i64_i32(cpu_R[RRR_R], v); \
                    tcg_temp_free_i64(v); \
                    tcg_temp_free_i64(tmp); \
                } while (0)

#define gen_shift(cmd) gen_shift_reg(cmd, cpu_SR[SAR])

            case 8: /*SRC*/
                {
                    TCGv_i64 v = tcg_temp_new_i64();
                    gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                    tcg_gen_concat_i32_i64(v, cpu_R[RRR_T], cpu_R[RRR_S]);
                    gen_shift(shr);
                }
                break;

            case 9: /*SRL*/
                {
                    TCGv_i64 v = tcg_temp_new_i64();
                    gen_window_check2(dc, RRR_R, RRR_T);
                    tcg_gen_extu_i32_i64(v, cpu_R[RRR_T]);
                    gen_shift(shr);
                }
                break;

            case 10: /*SLL*/
                {
                    TCGv_i64 v = tcg_temp_new_i64();
                    TCGv_i32 s = tcg_const_i32(32);
                    gen_window_check2(dc, RRR_R, RRR_S);
                    tcg_gen_sub_i32(s, s, cpu_SR[SAR]);
                    tcg_gen_extu_i32_i64(v, cpu_R[RRR_S]);
                    gen_shift_reg(shl, s);
                    tcg_temp_free(s);
                }
                break;

            case 11: /*SRA*/
                {
                    TCGv_i64 v = tcg_temp_new_i64();
                    gen_window_check2(dc, RRR_R, RRR_T);
                    tcg_gen_ext_i32_i64(v, cpu_R[RRR_T]);
                    gen_shift(sar);
                }
                break;
#undef gen_shift
#undef gen_shift_reg

            case 12: /*MUL16U*/
                HAS_OPTION(XTENSA_OPTION_16_BIT_IMUL);
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    TCGv_i32 v1 = tcg_temp_new_i32();
                    TCGv_i32 v2 = tcg_temp_new_i32();
                    tcg_gen_ext16u_i32(v1, cpu_R[RRR_S]);
                    tcg_gen_ext16u_i32(v2, cpu_R[RRR_T]);
                    tcg_gen_mul_i32(cpu_R[RRR_R], v1, v2);
                    tcg_temp_free(v2);
                    tcg_temp_free(v1);
                }
                break;

            case 13: /*MUL16S*/
                HAS_OPTION(XTENSA_OPTION_16_BIT_IMUL);
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    TCGv_i32 v1 = tcg_temp_new_i32();
                    TCGv_i32 v2 = tcg_temp_new_i32();
                    tcg_gen_ext16s_i32(v1, cpu_R[RRR_S]);
                    tcg_gen_ext16s_i32(v2, cpu_R[RRR_T]);
                    tcg_gen_mul_i32(cpu_R[RRR_R], v1, v2);
                    tcg_temp_free(v2);
                    tcg_temp_free(v1);
                }
                break;

            default: /*reserved*/
                RESERVED();
                break;
            }
            break;

        case 2: /*RST2*/
            gen_window_check3(dc, RRR_R, RRR_S, RRR_T);

            if (_OP2 >= 12) {
                HAS_OPTION(XTENSA_OPTION_32_BIT_IDIV);
                int label = gen_new_label();
                tcg_gen_brcondi_i32(TCG_COND_NE, cpu_R[RRR_T], 0, label);
                gen_exception_cause(dc, INTEGER_DIVIE_BY_ZERO_CAUSE);
                gen_set_label(label);
            }

            switch (_OP2) {
            case 8: /*MULLi*/
                HAS_OPTION(XTENSA_OPTION_32_BIT_IMUL);
                tcg_gen_mul_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 10: /*MULUHi*/
            case 11: /*MULSHi*/
                HAS_OPTION(XTENSA_OPTION_32_BIT_IMUL);
                {
                    TCGv_i64 r = tcg_temp_new_i64();
                    TCGv_i64 s = tcg_temp_new_i64();
                    TCGv_i64 t = tcg_temp_new_i64();

                    if (_OP2 == 10) {
                        tcg_gen_extu_i32_i64(s, cpu_R[RRR_S]);
                        tcg_gen_extu_i32_i64(t, cpu_R[RRR_T]);
                    } else {
                        tcg_gen_ext_i32_i64(s, cpu_R[RRR_S]);
                        tcg_gen_ext_i32_i64(t, cpu_R[RRR_T]);
                    }
                    tcg_gen_mul_i64(r, s, t);
                    tcg_gen_shri_i64(r, r, 32);
                    tcg_gen_trunc_i64_i32(cpu_R[RRR_R], r);

                    tcg_temp_free_i64(r);
                    tcg_temp_free_i64(s);
                    tcg_temp_free_i64(t);
                }
                break;

            case 12: /*QUOUi*/
                tcg_gen_divu_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 13: /*QUOSi*/
                tcg_gen_div_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 14: /*REMUi*/
                tcg_gen_remu_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            case 15: /*REMSi*/
                tcg_gen_rem_i32(cpu_R[RRR_R], cpu_R[RRR_S], cpu_R[RRR_T]);
                break;

            default: /*reserved*/
                RESERVED();
                break;
            }
            break;

        case 3: /*RST3*/
            switch (_OP2) {
            case 0: /*RSR*/
                if (RSR_SR >= 64) {
                    gen_check_privilege(dc);
                }
                gen_window_check1(dc, RRR_T);
                gen_rsr(cpu_R[RRR_T], RSR_SR);
                if (!sregnames[RSR_SR]) {
                    TBD();
                }
                break;

            case 1: /*WSR*/
                if (RSR_SR >= 64) {
                    gen_check_privilege(dc);
                }
                gen_window_check1(dc, RRR_T);
                gen_wsr(dc, RSR_SR, cpu_R[RRR_T]);
                if (!sregnames[RSR_SR]) {
                    TBD();
                }
                break;

            case 2: /*SEXTu*/
                HAS_OPTION(XTENSA_OPTION_MISC_OP);
                gen_window_check2(dc, RRR_R, RRR_S);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_shli_i32(tmp, cpu_R[RRR_S], 24 - RRR_T);
                    tcg_gen_sari_i32(cpu_R[RRR_R], tmp, 24 - RRR_T);
                    tcg_temp_free(tmp);
                }
                break;

            case 3: /*CLAMPSu*/
                HAS_OPTION(XTENSA_OPTION_MISC_OP);
                gen_window_check2(dc, RRR_R, RRR_S);
                {
                    TCGv_i32 tmp1 = tcg_temp_new_i32();
                    TCGv_i32 tmp2 = tcg_temp_new_i32();
                    int label = gen_new_label();

                    tcg_gen_sari_i32(tmp1, cpu_R[RRR_S], 24 - RRR_T);
                    tcg_gen_xor_i32(tmp2, tmp1, cpu_R[RRR_S]);
                    tcg_gen_andi_i32(tmp2, tmp2, 0xffffffff << (RRR_T + 7));
                    tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_S]);
                    tcg_gen_brcondi_i32(TCG_COND_EQ, tmp2, 0, label);

                    tcg_gen_sari_i32(tmp1, cpu_R[RRR_S], 31);
                    tcg_gen_xori_i32(cpu_R[RRR_R], tmp1,
                            0xffffffff >> (25 - RRR_T));

                    gen_set_label(label);

                    tcg_temp_free(tmp1);
                    tcg_temp_free(tmp2);
                }
                break;

            case 4: /*MINu*/
            case 5: /*MAXu*/
            case 6: /*MINUu*/
            case 7: /*MAXUu*/
                HAS_OPTION(XTENSA_OPTION_MISC_OP);
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    static const TCGCond cond[] = {
                        TCG_COND_LE,
                        TCG_COND_GE,
                        TCG_COND_LEU,
                        TCG_COND_GEU
                    };
                    int label = gen_new_label();

                    if (RRR_R != RRR_T) {
                        tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_S]);
                        tcg_gen_brcond_i32(cond[_OP2 - 4],
                                cpu_R[RRR_S], cpu_R[RRR_T], label);
                        tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_T]);
                    } else {
                        tcg_gen_brcond_i32(cond[_OP2 - 4],
                                cpu_R[RRR_T], cpu_R[RRR_S], label);
                        tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_S]);
                    }
                    gen_set_label(label);
                }
                break;

            case 8: /*MOVEQZ*/
            case 9: /*MOVNEZ*/
            case 10: /*MOVLTZ*/
            case 11: /*MOVGEZ*/
                gen_window_check3(dc, RRR_R, RRR_S, RRR_T);
                {
                    static const TCGCond cond[] = {
                        TCG_COND_NE,
                        TCG_COND_EQ,
                        TCG_COND_GE,
                        TCG_COND_LT
                    };
                    int label = gen_new_label();
                    tcg_gen_brcondi_i32(cond[_OP2 - 8], cpu_R[RRR_T], 0, label);
                    tcg_gen_mov_i32(cpu_R[RRR_R], cpu_R[RRR_S]);
                    gen_set_label(label);
                }
                break;

            case 12: /*MOVFp*/
                HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                TBD();
                break;

            case 13: /*MOVTp*/
                HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                TBD();
                break;

            case 14: /*RUR*/
                gen_window_check1(dc, RRR_R);
                {
                    int st = (RRR_S << 4) + RRR_T;
                    if (uregnames[st]) {
                        tcg_gen_mov_i32(cpu_R[RRR_R], cpu_UR[st]);
                    } else {
                        printf("rur %d not implemented, ", st);
                        TBD();
                    }
                }
                break;

            case 15: /*WUR*/
                gen_window_check1(dc, RRR_T);
                {
                    if (uregnames[RSR_SR]) {
                        tcg_gen_mov_i32(cpu_UR[RSR_SR], cpu_R[RRR_T]);
                    } else {
                        printf("wur %d not implemented, ", RSR_SR);
                        TBD();
                    }
                }
                break;

            }
            break;

        case 4: /*EXTUI*/
        case 5:
            gen_window_check2(dc, RRR_R, RRR_T);
            {
                int shiftimm = RRR_S | (_OP1 << 4);
                int maskimm = (1 << (_OP2 + 1)) - 1;

                TCGv_i32 tmp = tcg_temp_new_i32();
                tcg_gen_shri_i32(tmp, cpu_R[RRR_T], shiftimm);
                tcg_gen_andi_i32(cpu_R[RRR_R], tmp, maskimm);
                tcg_temp_free(tmp);
            }
            break;

        case 6: /*CUST0*/
            RESERVED();
            break;

        case 7: /*CUST1*/
            RESERVED();
            break;

        case 8: /*LSCXp*/
            HAS_OPTION(XTENSA_OPTION_COPROCESSOR);
            TBD();
            break;

        case 9: /*LSC4*/
            gen_window_check2(dc, RRR_S, RRR_T);
            switch (_OP2) {
            case 0: /*L32E*/
                HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                gen_check_privilege(dc);
                {
                    TCGv_i32 addr = tcg_temp_new_i32();
                    tcg_gen_addi_i32(addr, cpu_R[RRR_S],
                            (0xffffffc0 | (RRR_R << 2)));
                    /*TODO protection control*/
                    tcg_gen_qemu_ld32u(cpu_R[RRR_T], addr, 0);
                    tcg_temp_free(addr);
                }
                break;

            case 4: /*S32E*/
                HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                gen_check_privilege(dc);
                {
                    TCGv_i32 addr = tcg_temp_new_i32();
                    tcg_gen_addi_i32(addr, cpu_R[RRR_S],
                            (0xffffffc0 | (RRR_R << 2)));
                    /*TODO protection control*/
                    tcg_gen_qemu_st32(cpu_R[RRR_T], addr, 0);
                    tcg_temp_free(addr);
                }
                break;

            default:
                RESERVED();
                break;
            }
            break;

        case 10: /*FP0*/
            HAS_OPTION(XTENSA_OPTION_FP_COPROCESSOR);
            TBD();
            break;

        case 11: /*FP1*/
            HAS_OPTION(XTENSA_OPTION_FP_COPROCESSOR);
            TBD();
            break;

        default: /*reserved*/
            RESERVED();
            break;
        }
        break;

    case 1: /*L32R*/
        gen_window_check1(dc, RRR_T);
        {
            TCGv_i32 tmp = tcg_temp_local_new_i32();

            tcg_gen_movi_i32(tmp,
                    (0xfffc0000 | (RI16_IMM16 << 2)) +
                    ((dc->pc + 3) & ~3));

            if (option_enabled(dc, XTENSA_OPTION_EXTENDED_L32R)) {
                TCGv_i32 tmp1 = tcg_temp_new_i32();
                int label = gen_new_label();

                tcg_gen_andi_i32(tmp1, cpu_SR[LITBASE], 1);
                tcg_gen_brcondi_i32(TCG_COND_EQ, tmp1, 0, label);
                tcg_gen_andi_i32(tmp1, cpu_SR[LITBASE], 0xfffff000);
                tcg_gen_addi_i32(tmp, tmp1, (0xfffc0000 | (RI16_IMM16 << 2)));
                gen_set_label(label);
                tcg_temp_free(tmp1);
            }

            /* much simplified, no MMU */
            tcg_gen_qemu_ld32u(cpu_R[RRR_T], tmp, 0);
            tcg_temp_free(tmp);
        }
        break;

    case 2: /*LSAI*/
#define gen_load_store(type, shift) do { \
            TCGv_i32 addr = tcg_temp_local_new_i32(); \
            gen_window_check2(dc, RRI8_S, RRI8_T); \
            tcg_gen_addi_i32(addr, cpu_R[RRI8_S], RRI8_IMM8 << shift); \
            if (shift) { \
                gen_load_store_alignment(dc, shift, addr); \
            } \
            tcg_gen_qemu_##type(cpu_R[RRI8_T], addr, 0); \
            tcg_temp_free(addr); \
        } while (0)

        switch (RRI8_R) {
        case 0: /*L8UI*/
            gen_load_store(ld8u, 0);
            break;

        case 1: /*L16UI*/
            gen_load_store(ld16u, 1);
            break;

        case 2: /*L32I*/
            gen_load_store(ld32u, 2);
            break;

        case 4: /*S8I*/
            gen_load_store(st8, 0);
            break;

        case 5: /*S16I*/
            gen_load_store(st16, 1);
            break;

        case 6: /*S32I*/
            gen_load_store(st32, 2);
            break;

        case 7: /*CACHEc*/
            if (RRI8_T < 8) {
                HAS_OPTION(XTENSA_OPTION_DCACHE);
            }

            switch (RRI8_T) {
            case 0: /*DPFRc*/
                break;

            case 1: /*DPFWc*/
                break;

            case 2: /*DPFROc*/
                break;

            case 3: /*DPFWOc*/
                break;

            case 4: /*DHWBc*/
                break;

            case 5: /*DHWBIc*/
                break;

            case 6: /*DHIc*/
                break;

            case 7: /*DIIc*/
                break;

            case 8: /*DCEc*/
                switch (_OP1) {
                case 0: /*DPFLl*/
                    HAS_OPTION(XTENSA_OPTION_DCACHE_INDEX_LOCK);
                    break;

                case 2: /*DHUl*/
                    HAS_OPTION(XTENSA_OPTION_DCACHE_INDEX_LOCK);
                    break;

                case 3: /*DIUl*/
                    HAS_OPTION(XTENSA_OPTION_DCACHE_INDEX_LOCK);
                    break;

                case 4: /*DIWBc*/
                    HAS_OPTION(XTENSA_OPTION_DCACHE);
                    break;

                case 5: /*DIWBIc*/
                    HAS_OPTION(XTENSA_OPTION_DCACHE);
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;

                }
                break;

            case 12: /*IPFc*/
                HAS_OPTION(XTENSA_OPTION_ICACHE);
                break;

            case 13: /*ICEc*/
                switch (_OP1) {
                case 0: /*IPFLl*/
                    HAS_OPTION(XTENSA_OPTION_ICACHE_INDEX_LOCK);
                    break;

                case 2: /*IHUl*/
                    HAS_OPTION(XTENSA_OPTION_ICACHE_INDEX_LOCK);
                    break;

                case 3: /*IIUl*/
                    HAS_OPTION(XTENSA_OPTION_ICACHE_INDEX_LOCK);
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;
                }
                break;

            case 14: /*IHIc*/
                HAS_OPTION(XTENSA_OPTION_ICACHE);
                break;

            case 15: /*IIIc*/
                HAS_OPTION(XTENSA_OPTION_ICACHE);
                break;

            default: /*reserved*/
                RESERVED();
                break;
            }
            break;

        case 9: /*L16SI*/
            gen_load_store(ld16s, 1);
            break;

        case 10: /*MOVI*/
            gen_window_check1(dc, RRI8_T);
            tcg_gen_movi_i32(cpu_R[RRI8_T],
                    RRI8_IMM8 | (RRI8_S << 8) |
                    ((RRI8_S & 0x8) ? 0xfffff000 : 0));
            break;

        case 11: /*L32AIy*/
            HAS_OPTION(XTENSA_OPTION_MP_SYNCHRO);
            gen_load_store(ld32u, 2); /*TODO acquire?*/
            break;

        case 12: /*ADDI*/
            gen_window_check2(dc, RRI8_S, RRI8_T);
            tcg_gen_addi_i32(cpu_R[RRI8_T], cpu_R[RRI8_S], RRI8_IMM8_SE);
            break;

        case 13: /*ADDMI*/
            gen_window_check2(dc, RRI8_S, RRI8_T);
            tcg_gen_addi_i32(cpu_R[RRI8_T], cpu_R[RRI8_S], RRI8_IMM8_SE << 8);
            break;

        case 14: /*S32C1Iy*/
            HAS_OPTION(XTENSA_OPTION_MP_SYNCHRO);
            gen_window_check2(dc, RRI8_S, RRI8_T);
            {
                int label = gen_new_label();
                TCGv_i32 tmp = tcg_temp_local_new_i32();
                TCGv_i32 addr = tcg_temp_local_new_i32();

                tcg_gen_mov_i32(tmp, cpu_R[RRI8_T]);
                tcg_gen_addi_i32(addr, cpu_R[RRI8_S], RRI8_IMM8 << 2);
                gen_load_store_alignment(dc, 2, addr);
                tcg_gen_qemu_ld32u(cpu_R[RRI8_T], addr, 0);
                tcg_gen_brcond_i32(TCG_COND_NE, tmp, cpu_SR[SCOMPARE1], label);

                tcg_gen_qemu_st32(tmp, addr, 0);

                gen_set_label(label);
                tcg_temp_free(addr);
                tcg_temp_free(tmp);
            }
            break;

        case 15: /*S32RIy*/
            HAS_OPTION(XTENSA_OPTION_MP_SYNCHRO);
            gen_load_store(st32, 2); /*TODO release?*/
            break;

        default: /*reserved*/
            RESERVED();
            break;
        }
        break;
#undef gen_load_store

    case 3: /*LSCIp*/
        HAS_OPTION(XTENSA_OPTION_COPROCESSOR);
        TBD();
        break;

    case 4: /*MAC16d*/
        HAS_OPTION(XTENSA_OPTION_MAC16);
        TBD();
        break;

    case 5: /*CALLN*/
        switch (CALL_N) {
        case 0: /*CALL0*/
            tcg_gen_movi_i32(cpu_R[0], dc->pc + 3);
            gen_jumpi(dc, (dc->pc & ~3) + (CALL_OFFSET_SE << 2) + 4);
            break;

        case 1: /*CALL4w*/
        case 2: /*CALL8w*/
        case 3: /*CALL12w*/
            HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
            gen_window_check1(dc, CALL_N << 2);
            {
                TCGv_i32 tmp = tcg_const_i32(
                        (dc->pc & ~3) + (CALL_OFFSET_SE << 2) + 4);
                gen_callw(dc, CALL_N, tmp);
                tcg_temp_free(tmp);
            }
            break;
        }
        break;

    case 6: /*SI*/
        switch (CALL_N) {
        case 0: /*J*/
            gen_jumpi(dc, dc->pc + 4 + CALL_OFFSET_SE);
            break;

        case 1: /*BZ*/
            gen_window_check1(dc, BRI12_S);
            {
                int label = gen_new_label();
                int inv = BRI12_M & 1;

                switch (BRI12_M & 2) {
                case 0: /*BEQZ*/
                    tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            cpu_R[BRI12_S], 0, label);
                    break;

                case 2: /*BLTZ*/
                    tcg_gen_brcondi_i32(inv ? TCG_COND_LT : TCG_COND_GE,
                            cpu_R[BRI12_S], 0, label);
                    break;
                }
                gen_jumpi(dc, dc->pc + 4 + BRI12_IMM12_SE);
                gen_set_label(label);
                gen_jumpi_check_loop_end(dc, dc->pc + 3);
            }
            break;

        case 2: /*BI0*/
            gen_window_check1(dc, BRI8_S);
            {
                int label = gen_new_label();
                int inv = BRI8_M & 1;

                switch (BRI8_M & 2) {
                case 0: /*BEQI*/
                    tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            cpu_R[BRI8_S], B4CONST[BRI8_R], label);
                    break;

                case 2: /*BLTI*/
                    tcg_gen_brcondi_i32(inv ? TCG_COND_LT : TCG_COND_GE,
                            cpu_R[BRI8_S], B4CONST[BRI8_R], label);
                    break;
                }
                gen_jumpi(dc, dc->pc + 4 + BRI8_IMM8_SE);
                gen_set_label(label);
                gen_jumpi_check_loop_end(dc, dc->pc + 3);
            }
            break;

        case 3: /*BI1*/
            switch (BRI8_M) {
            case 0: /*ENTRYw*/
                HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                {
                    TCGv_i32 pc = tcg_const_i32(dc->pc);
                    TCGv_i32 s = tcg_const_i32(BRI12_S);
                    TCGv_i32 imm = tcg_const_i32(BRI12_IMM12);
                    gen_helper_entry(pc, s, imm);
                    tcg_temp_free(imm);
                    tcg_temp_free(s);
                    tcg_temp_free(pc);
                    reset_used_window(dc);
                }
                break;

            case 1: /*B1*/
                switch (BRI8_R) {
                case 0: /*BFp*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                case 1: /*BTp*/
                    HAS_OPTION(XTENSA_OPTION_BOOLEAN);
                    TBD();
                    break;

                case 8: /*LOOP*/
                case 9: /*LOOPNEZ*/
                case 10: /*LOOPGTZ*/
                    HAS_OPTION(XTENSA_OPTION_LOOP);
                    gen_window_check1(dc, RRI8_S);
                    {
                        uint32_t lend = dc->pc + RRI8_IMM8 + 4;
                        TCGv_i32 tmp = tcg_const_i32(lend);

                        tcg_gen_subi_i32(cpu_SR[LCOUNT], cpu_R[RRI8_S], 1);
                        tcg_gen_movi_i32(cpu_SR[LBEG], dc->pc + 3);
                        gen_wsr_lend(dc, LEND, tmp);
                        tcg_temp_free(tmp);

                        if (BRI8_R > 8) {
                            int label = gen_new_label();
                            tcg_gen_brcondi_i32(
                                    BRI8_R == 9 ? TCG_COND_NE : TCG_COND_GT,
                                    cpu_R[RRI8_S], 0, label);
                            gen_jumpi(dc, lend);
                            gen_set_label(label);
                        }

                        gen_jumpi(dc, dc->pc + 3);
                    }
                    break;

                default: /*reserved*/
                    RESERVED();
                    break;

                }
                break;

            case 2: /*BLTUI*/
            case 3: /*BGEUI*/
                gen_window_check1(dc, BRI8_S);
                {
                    int label = gen_new_label();
                    int inv = BRI8_M & 1;

                    tcg_gen_brcondi_i32(inv ? TCG_COND_LTU : TCG_COND_GEU,
                            cpu_R[BRI8_S], B4CONSTU[BRI8_R], label);

                    gen_jumpi(dc, dc->pc + 4 + BRI8_IMM8_SE);
                    gen_set_label(label);
                    gen_jumpi_check_loop_end(dc, dc->pc + 3);
                }
                break;
            }
            break;

        }
        break;

    case 7: /*B*/
        {
            int label = gen_new_label();
            int inv = RRI8_R & 8;

            switch (RRI8_R & 7) {
            case 0: /*BNONE*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_and_i32(tmp, cpu_R[RRI8_S], cpu_R[RRI8_T]);
                    tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            tmp, 0, label);
                    tcg_temp_free(tmp);
                }
                break;

            case 1: /*BEQ*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                tcg_gen_brcond_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                        cpu_R[RRI8_S], cpu_R[RRI8_T], label);
                break;

            case 2: /*BLT*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                tcg_gen_brcond_i32(inv ? TCG_COND_LT : TCG_COND_GE,
                        cpu_R[RRI8_S], cpu_R[RRI8_T], label);
                break;

            case 3: /*BLTU*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                tcg_gen_brcond_i32(inv ? TCG_COND_LTU : TCG_COND_GEU,
                        cpu_R[RRI8_S], cpu_R[RRI8_T], label);
                break;

            case 4: /*BALL*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_and_i32(tmp, cpu_R[RRI8_S], cpu_R[RRI8_T]);
                    tcg_gen_brcond_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            tmp, cpu_R[RRI8_T], label);
                    tcg_temp_free(tmp);
                }
                break;

            case 5: /*BBC*/
                gen_window_check2(dc, RRI8_S, RRI8_T);
                {
                    TCGv_i32 bit = tcg_const_i32(1);
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_andi_i32(tmp, cpu_R[RRI8_T], 0x1f);
                    tcg_gen_shl_i32(bit, bit, tmp);
                    tcg_gen_and_i32(tmp, cpu_R[RRI8_S], bit);
                    tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            tmp, 0, label);
                    tcg_temp_free(tmp);
                    tcg_temp_free(bit);
                }
                break;

            case 6: /*BBCI*/
            case 7:
                gen_window_check1(dc, RRI8_S);
                {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_andi_i32(tmp, cpu_R[RRI8_S],
                            1 << (((RRI8_R & 1) << 4) | RRI8_T));
                    tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                            tmp, 0, label);
                    tcg_temp_free(tmp);
                }
                break;

            }
            gen_jumpi(dc, dc->pc + 4 + RRI8_IMM8_SE);
            gen_set_label(label);
            gen_jumpi_check_loop_end(dc, dc->pc + 3);
        }
        break;

#define gen_narrow_load_store(type) do { \
            TCGv_i32 addr = tcg_temp_local_new_i32(); \
            gen_window_check2(dc, RRRN_S, RRRN_T); \
            tcg_gen_addi_i32(addr, cpu_R[RRRN_S], RRRN_R << 2); \
            gen_load_store_alignment(dc, 2, addr); \
            tcg_gen_qemu_##type(cpu_R[RRRN_T], addr, 0); \
            tcg_temp_free(addr); \
        } while (0)

    case 8: /*L32I.Nn*/
        gen_narrow_load_store(ld32u);
        break;

    case 9: /*S32I.Nn*/
        gen_narrow_load_store(st32);
        break;
#undef gen_narrow_load_store

    case 10: /*ADD.Nn*/
        gen_window_check3(dc, RRRN_R, RRRN_S, RRRN_T);
        tcg_gen_add_i32(cpu_R[RRRN_R], cpu_R[RRRN_S], cpu_R[RRRN_T]);
        break;

    case 11: /*ADDI.Nn*/
        gen_window_check2(dc, RRRN_R, RRRN_S);
        tcg_gen_addi_i32(cpu_R[RRRN_R], cpu_R[RRRN_S], RRRN_T ? RRRN_T : -1);
        break;

    case 12: /*ST2n*/
        gen_window_check1(dc, RRRN_S);
        if (RRRN_T < 8) { /*MOVI.Nn*/
            tcg_gen_movi_i32(cpu_R[RRRN_S],
                    RRRN_R | (RRRN_T << 4) |
                    ((RRRN_T & 6) == 6 ? 0xffffff80 : 0));
        } else { /*BEQZ.Nn*/ /*BNEZ.Nn*/
            int label = gen_new_label();
            int inv = RRRN_T & 4;

            tcg_gen_brcondi_i32(inv ? TCG_COND_EQ : TCG_COND_NE,
                    cpu_R[RRRN_S], 0, label);
            gen_jumpi(dc, dc->pc + 4 + (RRRN_R | ((RRRN_T & 3) << 4)));
            gen_set_label(label);
            gen_jumpi_check_loop_end(dc, dc->pc + 2);
        }
        break;

    case 13: /*ST3n*/
        switch (RRRN_R) {
        case 0: /*MOV.Nn*/
            gen_window_check2(dc, RRRN_S, RRRN_T);
            tcg_gen_mov_i32(cpu_R[RRRN_T], cpu_R[RRRN_S]);
            break;

        case 15: /*S3*/
            switch (RRRN_T) {
            case 0: /*RET.Nn*/
                gen_jump(dc, cpu_R[0]);
                break;

            case 1: /*RETW.Nn*/
                HAS_OPTION(XTENSA_OPTION_WINDOWED_REGISTER);
                {
                    TCGv_i32 tmp = tcg_const_i32(dc->pc);
                    gen_helper_retw(tmp, tmp);
                    gen_jump(dc, tmp);
                    tcg_temp_free(tmp);
                }
                break;

            case 2: /*BREAK.Nn*/
                TBD();
                break;

            case 3: /*NOP.Nn*/
                break;

            case 6: /*ILL.Nn*/
                gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
                break;

            default: /*reserved*/
                RESERVED();
                break;
            }
            break;

        default: /*reserved*/
            RESERVED();
            break;
        }
        break;

    default: /*reserved*/
        RESERVED();
        break;
    }

    if (_OP0 >= 8) {
        dc->pc += 2;
        HAS_OPTION(XTENSA_OPTION_CODE_DENSITY);
    } else {
        dc->pc += 3;
    }

    gen_check_loop_end(dc);

    return;

invalid_opcode:
    printf("INVALID(pc = %08x): %s:%d\n", dc->pc, __FILE__, __LINE__);
    dc->pc += (_OP0 >= 8) ? 2 : 3;
#undef HAS_OPTION
}

static void check_breakpoint(CPUState *env, DisasContext *dc)
{
    CPUBreakpoint *bp;

    if (unlikely(!QTAILQ_EMPTY(&env->breakpoints))) {
        QTAILQ_FOREACH(bp, &env->breakpoints, entry) {
            if (bp->pc == dc->pc) {
                tcg_gen_movi_i32(cpu_pc, dc->pc);
                gen_exception(EXCP_DEBUG);
                dc->is_jmp = DISAS_UPDATE;
             }
        }
    }
}

static void gen_timer(CPUState *env, DisasContext *dc)
{
    int id;
    tcg_gen_addi_i32(cpu_SR[CCOUNT], cpu_SR[CCOUNT], 1);
    for (id = 0; id < env->config->nccompare; ++id) {
        int label = gen_new_label();
        tcg_gen_brcond_i32(
                TCG_COND_NE, cpu_SR[CCOUNT], cpu_SR[CCOMPARE + id], label);
        {
            TCGv_i32 tid = tcg_const_i32(id);
            TCGv_i32 active = tcg_const_i32(1);
            tcg_gen_movi_i32(cpu_pc, dc->pc);
            gen_helper_timer_irq(tid, active);
            tcg_temp_free(tid);
            tcg_temp_free(active);
        }
        gen_set_label(label);
    }
}

static void gen_intermediate_code_internal(
        CPUState *env, TranslationBlock *tb, int search_pc)
{
    DisasContext dc;
    int insn_count = 0;
    int j, lj = -1;
    uint16_t *gen_opc_end = gen_opc_buf + OPC_MAX_SIZE;
    int max_insns = tb->cflags & CF_COUNT_MASK;

    if (max_insns == 0) {
        max_insns = CF_COUNT_MASK;
    }

    dc.config = env->config;
    dc.singlestep_enabled = env->singlestep_enabled;
    dc.tb = tb;
    dc.pc = env->pc;
    dc.lend = env->sregs[LEND];
    dc.is_jmp = DISAS_NEXT;
    reset_used_window(&dc);

    gen_icount_start();

    if (env->singlestep_enabled && env->exception_taken) {
        env->exception_taken = 0;
        tcg_gen_movi_i32(cpu_pc, dc.pc);
        gen_exception(EXCP_DEBUG);
    }

    do {
        check_breakpoint(env, &dc);

        if (search_pc) {
            j = gen_opc_ptr - gen_opc_buf;
            if (lj < j) {
                lj++;
                while (lj < j) {
                    gen_opc_instr_start[lj++] = 0;
                }
            }
            gen_opc_pc[lj] = dc.pc;
            gen_opc_instr_start[lj] = 1;
            gen_opc_icount[lj] = insn_count;
        }

        if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP))) {
            tcg_gen_debug_insn_start(dc.pc);
        }

        gen_timer(env, &dc);

        if (insn_count + 1 == max_insns && (tb->cflags & CF_LAST_IO)) {
            gen_io_start();
        }

        disas_xtensa_insn(&dc);
        ++insn_count;
        if (env->singlestep_enabled) {
            tcg_gen_movi_i32(cpu_pc, dc.pc);
            gen_exception(EXCP_DEBUG);
            break;
        }
    } while (dc.is_jmp == DISAS_NEXT &&
            insn_count < max_insns &&
            gen_opc_ptr < gen_opc_end);

    if (tb->cflags & CF_LAST_IO) {
        gen_io_end();
    }

    if (dc.is_jmp == DISAS_NEXT) {
        tcg_gen_movi_i32(cpu_pc, dc.pc);
        tcg_gen_exit_tb(0);
    }
    gen_icount_end(tb, insn_count);
    *gen_opc_ptr = INDEX_op_end;

}

void gen_intermediate_code(CPUState *env, TranslationBlock *tb)
{
    gen_intermediate_code_internal(env, tb, 0);
}

void gen_intermediate_code_pc(CPUState *env, TranslationBlock *tb)
{
    gen_intermediate_code_internal(env, tb, 1);
}

void cpu_dump_state(CPUState *env, FILE *f, fprintf_function cpu_fprintf,
        int flags)
{
    int i, j;

    cpu_fprintf(f, "PC=%08x\n\n", env->pc);

    for (i = j = 0; i < 256; ++i)
        if (sregnames[i]) {
            cpu_fprintf(f, "%s=%08x%c", sregnames[i], env->sregs[i],
                    (j++ % 4) == 3 ? '\n' : ' ');
        }

    cpu_fprintf(f, (j % 4) == 0 ? "\n" : "\n\n");

    for (i = j = 0; i < 256; ++i)
        if (uregnames[i]) {
            cpu_fprintf(f, "%s=%08x%c", uregnames[i], env->uregs[i],
                    (j++ % 4) == 3 ? '\n' : ' ');
        }

    cpu_fprintf(f, (j % 4) == 0 ? "\n" : "\n\n");

    for (i = 0; i < 16; ++i)
        cpu_fprintf(f, "A%02d=%08x%c", i, env->regs[i],
                (i % 4) == 3 ? '\n' : ' ');

    cpu_fprintf(f, "\n");

    for (i = 0; i < env->config->nareg; ++i)
        cpu_fprintf(f, "AR%02d=%08x%c", i, env->phys_regs[i],
                (i % 4) == 3 ? '\n' : ' ');
}

void restore_state_to_opc(CPUState *env, TranslationBlock *tb, int pc_pos)
{
    env->pc = gen_opc_pc[pc_pos];
}
