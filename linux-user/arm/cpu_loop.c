/*
 *  qemu user cpu loop
 *
 *  Copyright (c) 2003-2008 Fabrice Bellard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu.h"
#include "elf.h"
#include "cpu_loop-common.h"

#define get_user_code_u32(x, gaddr, env)                \
    ({ abi_long __r = get_user_u32((x), (gaddr));       \
        if (!__r && bswap_code(arm_sctlr_b(env))) {     \
            (x) = bswap32(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_code_u16(x, gaddr, env)                \
    ({ abi_long __r = get_user_u16((x), (gaddr));       \
        if (!__r && bswap_code(arm_sctlr_b(env))) {     \
            (x) = bswap16(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_data_u32(x, gaddr, env)                \
    ({ abi_long __r = get_user_u32((x), (gaddr));       \
        if (!__r && arm_cpu_bswap_data(env)) {          \
            (x) = bswap32(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_data_u16(x, gaddr, env)                \
    ({ abi_long __r = get_user_u16((x), (gaddr));       \
        if (!__r && arm_cpu_bswap_data(env)) {          \
            (x) = bswap16(x);                           \
        }                                               \
        __r;                                            \
    })

#define put_user_data_u32(x, gaddr, env)                \
    ({ typeof(x) __x = (x);                             \
        if (arm_cpu_bswap_data(env)) {                  \
            __x = bswap32(__x);                         \
        }                                               \
        put_user_u32(__x, (gaddr));                     \
    })

#define put_user_data_u16(x, gaddr, env)                \
    ({ typeof(x) __x = (x);                             \
        if (arm_cpu_bswap_data(env)) {                  \
            __x = bswap16(__x);                         \
        }                                               \
        put_user_u16(__x, (gaddr));                     \
    })

//#define HASH_LLSC
//#define LLSC_LOG
//#define PICO_ST_LLSC
#define PF_LLSC
/* Commpage handling -- there is no commpage for AArch64 */

/*
 * See the Linux kernel's Documentation/arm/kernel_user_helpers.txt
 * Input:
 * r0 = pointer to oldval
 * r1 = pointer to newval
 * r2 = pointer to target value
 *
 * Output:
 * r0 = 0 if *ptr was changed, non-0 if no exchange happened
 * C set if *ptr was changed, clear if no exchange happened
 *
 * Note segv's in kernel helpers are a bit tricky, we can set the
 * data address sensibly but the PC address is just the entry point.
 */
static void arm_kernel_cmpxchg64_helper(CPUARMState *env)
{
    uint64_t oldval, newval, val;
    uint32_t addr, cpsr;
    target_siginfo_t info;

    /* Based on the 32 bit code in do_kernel_trap */

    /* XXX: This only works between threads, not between processes.
       It's probably possible to implement this with native host
       operations. However things like ldrex/strex are much harder so
       there's not much point trying.  */
    start_exclusive();
    cpsr = cpsr_read(env);
    addr = env->regs[2];

    if (get_user_u64(oldval, env->regs[0])) {
        env->exception.vaddress = env->regs[0];
        goto segv;
    };

    if (get_user_u64(newval, env->regs[1])) {
        env->exception.vaddress = env->regs[1];
        goto segv;
    };

    if (get_user_u64(val, addr)) {
        env->exception.vaddress = addr;
        goto segv;
    }

    if (val == oldval) {
        val = newval;

        if (put_user_u64(val, addr)) {
            env->exception.vaddress = addr;
            goto segv;
        };

        env->regs[0] = 0;
        cpsr |= CPSR_C;
    } else {
        env->regs[0] = -1;
        cpsr &= ~CPSR_C;
    }
    cpsr_write(env, cpsr, CPSR_C, CPSRWriteByInstr);
    end_exclusive();
    return;

segv:
    end_exclusive();
    /* We get the PC of the entry address - which is as good as anything,
       on a real kernel what you get depends on which mode it uses. */
    info.si_signo = TARGET_SIGSEGV;
    info.si_errno = 0;
    /* XXX: check env->error_code */
    info.si_code = TARGET_SEGV_MAPERR;
    info._sifields._sigfault._addr = env->exception.vaddress;
    queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
}

/* Handle a jump to the kernel code page.  */
static int
do_kernel_trap(CPUARMState *env)
{
    uint32_t addr;
    uint32_t cpsr;
    uint32_t val;

    switch (env->regs[15]) {
    case 0xffff0fa0: /* __kernel_memory_barrier */
        /* ??? No-op. Will need to do better for SMP.  */
        break;
    case 0xffff0fc0: /* __kernel_cmpxchg */
         /* XXX: This only works between threads, not between processes.
            It's probably possible to implement this with native host
            operations. However things like ldrex/strex are much harder so
            there's not much point trying.  */
        start_exclusive();
        cpsr = cpsr_read(env);
        addr = env->regs[2];
        /* FIXME: This should SEGV if the access fails.  */
        if (get_user_u32(val, addr))
            val = ~env->regs[0];
        if (val == env->regs[0]) {
            val = env->regs[1];
            /* FIXME: Check for segfaults.  */
            put_user_u32(val, addr);
            env->regs[0] = 0;
            cpsr |= CPSR_C;
        } else {
            env->regs[0] = -1;
            cpsr &= ~CPSR_C;
        }
        cpsr_write(env, cpsr, CPSR_C, CPSRWriteByInstr);
        end_exclusive();
        break;
    case 0xffff0fe0: /* __kernel_get_tls */
        env->regs[0] = cpu_get_tls(env);
        break;
    case 0xffff0f60: /* __kernel_cmpxchg64 */
        arm_kernel_cmpxchg64_helper(env);
        break;

    default:
        return 1;
    }
    /* Jump back to the caller.  */
    addr = env->regs[14];
    if (addr & 1) {
        env->thumb = 1;
        addr &= ~1;
    }
    env->regs[15] = addr;

    return 0;
}

/* Load exclusive handling for AArch32 */
static int do_ldrex(CPUARMState *env)
{
    uint64_t val;
    int segv = 0;
    uint32_t addr;
#ifdef HASH_LLSC
	uint32_t hash_addr;
#endif
    //fprintf(stderr, "do_ldrex\n");
    start_exclusive();

    addr = env->exclusive_addr;

    segv = get_user_u32(val, addr);
	assert(segv == 0);
	env->exclusive_val = val;

#ifdef HASH_LLSC
	hash_addr = (addr & 0x0fffffff) | 0xa0000000;
    segv = put_user_u32(env->exclusive_tid, hash_addr);
	assert(segv == 0);
#endif
	
    env->regs[15] += 4;
    env->regs[(env->exclusive_info) & 0xf] = val;
	//fprintf(stderr, "ldrex reg = %d, reg15 = %d, val = %ld!, addr = %x\n",
	//		(env->exclusive_info) & 0xf , env->regs[15], val, addr);

#ifdef LLSC_LOG
	fprintf(stderr, "thread %d ldrex done! val %lx, addr %x\n", env->exclusive_tid, env->exclusive_val, addr);
#endif
    end_exclusive();
    return segv;
}

#ifdef PF_LLSC
extern int x_monitor_check_exclusive(void* p_node, uint32_t addr);
extern int x_monitor_check_and_clean(int tid, uint32_t addr);
#endif
/* Store exclusive handling for AArch32 */
static int do_strex(CPUARMState *env)
{
    uint64_t val;
    int size;
    int rc = 1;
    int segv = 0;
    uint32_t addr;
#ifdef HASH_LLSC
	uint32_t hash_addr;
	uint32_t hash_entry;
#endif
    //fprintf(stderr, "[do_strex]\tdo_strex\n");
    start_exclusive();

    if (env->exclusive_addr != env->exclusive_test) {
#ifdef LLSC_LOG
		fprintf(stderr, "thread %d strex fail! val %lx, oldval %lx\n", env->exclusive_tid, val, env->exclusive_val);
#endif
        goto fail;
    }
    /* We know we're always AArch32 so the address is in uint32_t range
     * unless it was the -1 exclusive-monitor-lost value (which won't
     * match exclusive_test above).
     */
    assert(extract64(env->exclusive_addr, 32, 32) == 0);
    addr = env->exclusive_addr;
#ifdef PF_LLSC
	target_ulong page_addr = addr & 0xfffff000;
	target_mprotect(page_addr, 0x1000, PROT_READ|PROT_WRITE);
	//target_mremap();
	if (x_monitor_check_exclusive((void*)env->exclusive_node, addr) != 1) {
#ifdef LLSC_LOG
		fprintf(stderr, "thread %d strex fail! val %lx, oldval %lx, exclusive mark lost.\n", env->exclusive_tid, val, env->exclusive_val);
#endif
		goto fail;
	}
#endif
#ifdef HASH_LLSC
	hash_addr = (addr & 0x0fffffff) | 0xa0000000;
	segv = get_user_u32(hash_entry, hash_addr);
	assert(segv == 0);
	if (hash_entry != env->exclusive_tid) {

#ifdef LLSC_LOG
		fprintf(stderr, "thread %d strex fail! val %lx, oldval %lx, hash_entry %x, addr %x\n", env->exclusive_tid, val, env->exclusive_val, hash_entry, addr);
#endif
        goto fail;
    }

#endif
	
    size = env->exclusive_info & 0xf;
    switch (size) {
    case 0:
        segv = get_user_u8(val, addr);
        break;
    case 1:
        segv = get_user_u16(val, addr);
        break;
    case 2:
    case 3:
        segv = get_user_u32(val, addr);
        break;
    default:
        abort();
    }
    if (segv) {
        env->exception.vaddress = addr;
        goto done;
    }
    if (size == 3) {
        uint32_t valhi;
        segv = get_user_u32(valhi, addr + 4);
        if (segv) {
            env->exception.vaddress = addr + 4;
            goto done;
        }
        val = deposit64(val, 32, 32, valhi);
    }
    if (val != env->exclusive_val) {
#ifdef LLSC_LOG
	fprintf(stderr, "thread %d strex fail! val %lx, oldval %lx, addr %x\n", env->exclusive_tid, val, env->exclusive_val, addr);
#endif
        goto fail;
    }

    val = env->regs[(env->exclusive_info >> 8) & 0xf];
#ifdef LLSC_LOG
	fprintf(stderr, "thread %d strex suc! newval %lx, oldval %lx, addr %x\n", env->exclusive_tid, val, env->exclusive_val, addr);
#endif
#ifdef PF_LLSC
	x_monitor_check_and_clean(env->exclusive_tid, addr);
#endif
    switch (size) {
    case 0:
        segv = put_user_u8(val, addr);
        break;
    case 1:
        segv = put_user_u16(val, addr);
        break;
    case 2:
    case 3:
        segv = put_user_u32(val, addr);
        break;
    }
    if (segv) {
		assert(segv);
        env->exception.vaddress = addr;
        goto done;
    }
    if (size == 3) {
        val = env->regs[(env->exclusive_info >> 12) & 0xf];
        segv = put_user_u32(val, addr + 4);
        if (segv) {
            env->exception.vaddress = addr + 4;
            goto done;
        }
    }
    rc = 0;
fail:
    env->regs[15] += 4;
    env->regs[(env->exclusive_info >> 4) & 0xf] = rc;
	
done:
    end_exclusive();
    return segv;
}

void cpu_loop(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);
    int trapnr;
    unsigned int n, insn;
    target_siginfo_t info;
    uint32_t addr;
    abi_ulong ret;

    for(;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        process_queued_cpu_work(cs);

        switch(trapnr) {
        case EXCP_UDEF:
        case EXCP_NOCP:
        case EXCP_INVSTATE:
            {
                TaskState *ts = cs->opaque;
                uint32_t opcode;
                int rc;

                /* we handle the FPU emulation here, as Linux */
                /* we get the opcode */
                /* FIXME - what to do if get_user() fails? */
                get_user_code_u32(opcode, env->regs[15], env);

                rc = EmulateAll(opcode, &ts->fpa, env);
                if (rc == 0) { /* illegal instruction */
                    info.si_signo = TARGET_SIGILL;
                    info.si_errno = 0;
                    info.si_code = TARGET_ILL_ILLOPN;
                    info._sifields._sigfault._addr = env->regs[15];
                    queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
                } else if (rc < 0) { /* FP exception */
                    int arm_fpe=0;

                    /* translate softfloat flags to FPSR flags */
                    if (-rc & float_flag_invalid)
                      arm_fpe |= BIT_IOC;
                    if (-rc & float_flag_divbyzero)
                      arm_fpe |= BIT_DZC;
                    if (-rc & float_flag_overflow)
                      arm_fpe |= BIT_OFC;
                    if (-rc & float_flag_underflow)
                      arm_fpe |= BIT_UFC;
                    if (-rc & float_flag_inexact)
                      arm_fpe |= BIT_IXC;

                    FPSR fpsr = ts->fpa.fpsr;
                    //printf("fpsr 0x%x, arm_fpe 0x%x\n",fpsr,arm_fpe);

                    if (fpsr & (arm_fpe << 16)) { /* exception enabled? */
                      info.si_signo = TARGET_SIGFPE;
                      info.si_errno = 0;

                      /* ordered by priority, least first */
                      if (arm_fpe & BIT_IXC) info.si_code = TARGET_FPE_FLTRES;
                      if (arm_fpe & BIT_UFC) info.si_code = TARGET_FPE_FLTUND;
                      if (arm_fpe & BIT_OFC) info.si_code = TARGET_FPE_FLTOVF;
                      if (arm_fpe & BIT_DZC) info.si_code = TARGET_FPE_FLTDIV;
                      if (arm_fpe & BIT_IOC) info.si_code = TARGET_FPE_FLTINV;

                      info._sifields._sigfault._addr = env->regs[15];
                      queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
                    } else {
                      env->regs[15] += 4;
                    }

                    /* accumulate unenabled exceptions */
                    if ((!(fpsr & BIT_IXE)) && (arm_fpe & BIT_IXC))
                      fpsr |= BIT_IXC;
                    if ((!(fpsr & BIT_UFE)) && (arm_fpe & BIT_UFC))
                      fpsr |= BIT_UFC;
                    if ((!(fpsr & BIT_OFE)) && (arm_fpe & BIT_OFC))
                      fpsr |= BIT_OFC;
                    if ((!(fpsr & BIT_DZE)) && (arm_fpe & BIT_DZC))
                      fpsr |= BIT_DZC;
                    if ((!(fpsr & BIT_IOE)) && (arm_fpe & BIT_IOC))
                      fpsr |= BIT_IOC;
                    ts->fpa.fpsr=fpsr;
                } else { /* everything OK */
                    /* increment PC */
                    env->regs[15] += 4;
                }
            }
            break;
        case EXCP_SWI:
        case EXCP_BKPT:
            {
                env->eabi = 1;
                /* system call */
                if (trapnr == EXCP_BKPT) {
                    if (env->thumb) {
                        /* FIXME - what to do if get_user() fails? */
                        get_user_code_u16(insn, env->regs[15], env);
                        n = insn & 0xff;
                        env->regs[15] += 2;
                    } else {
                        /* FIXME - what to do if get_user() fails? */
                        get_user_code_u32(insn, env->regs[15], env);
                        n = (insn & 0xf) | ((insn >> 4) & 0xff0);
                        env->regs[15] += 4;
                    }
                } else {
                    if (env->thumb) {
                        /* FIXME - what to do if get_user() fails? */
                        get_user_code_u16(insn, env->regs[15] - 2, env);
                        n = insn & 0xff;
                    } else {
                        /* FIXME - what to do if get_user() fails? */
                        get_user_code_u32(insn, env->regs[15] - 4, env);
                        n = insn & 0xffffff;
                    }
                }

                if (n == ARM_NR_cacheflush) {
                    /* nop */
                } else if (n == ARM_NR_semihosting
                           || n == ARM_NR_thumb_semihosting) {
                    env->regs[0] = do_arm_semihosting (env);
                } else if (n == 0 || n >= ARM_SYSCALL_BASE || env->thumb) {
                    /* linux syscall */
                    if (env->thumb || n == 0) {
                        n = env->regs[7];
                    } else {
                        n -= ARM_SYSCALL_BASE;
                        env->eabi = 0;
                    }
                    if ( n > ARM_NR_BASE) {
                        switch (n) {
                        case ARM_NR_cacheflush:
                            /* nop */
                            break;
                        case ARM_NR_set_tls:
                            cpu_set_tls(env, env->regs[0]);
                            env->regs[0] = 0;
                            break;
                        case ARM_NR_breakpoint:
                            env->regs[15] -= env->thumb ? 2 : 4;
                            goto excp_debug;
                        case ARM_NR_get_tls:
                            env->regs[0] = cpu_get_tls(env);
                            break;
                        default:
                            gemu_log("qemu: Unsupported ARM syscall: 0x%x\n",
                                     n);
                            env->regs[0] = -TARGET_ENOSYS;
                            break;
                        }
                    } else {
                        ret = do_syscall(env,
                                         n,
                                         env->regs[0],
                                         env->regs[1],
                                         env->regs[2],
                                         env->regs[3],
                                         env->regs[4],
                                         env->regs[5],
                                         0, 0);
                        if (ret == -TARGET_ERESTARTSYS) {
                            env->regs[15] -= env->thumb ? 2 : 4;
                        } else if (ret != -TARGET_QEMU_ESIGRETURN) {
                            env->regs[0] = ret;
                        }
                    }
                } else {
                    goto error;
                }
            }
            break;
        case EXCP_SEMIHOST:
            env->regs[0] = do_arm_semihosting(env);
            break;
        case EXCP_INTERRUPT:
            /* just indicate that signals should be handled asap */
            break;
        case EXCP_PREFETCH_ABORT:
        case EXCP_DATA_ABORT:
            addr = env->exception.vaddress;
            {
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                /* XXX: check env->error_code */
                info.si_code = TARGET_SEGV_MAPERR;
                info._sifields._sigfault._addr = addr;
                queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
            }
            break;
        case EXCP_DEBUG:
        excp_debug:
            info.si_signo = TARGET_SIGTRAP;
            info.si_errno = 0;
            info.si_code = TARGET_TRAP_BRKPT;
            queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
            break;
        case EXCP_KERNEL_TRAP:
            if (do_kernel_trap(env))
              goto error;
            break;
        case EXCP_YIELD:
            /* nothing to do here for user-mode, just resume guest code */
            break;
        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;
		case EXCP_LDREX:
			if (!do_ldrex(env)) {
				break;
			}
			else {
				EXCP_DUMP(env, "qemu: unhandled CPU exception 0x%x - aborting\n", trapnr);
            	abort();
			}
		case EXCP_STREX:
            if (!do_strex(env)) {
                break;
            }
            /* fall through for segv */
        default:
        error:
            EXCP_DUMP(env, "qemu: unhandled CPU exception 0x%x - aborting\n", trapnr);
            abort();
        }
        process_pending_signals(env);
    }
}

void target_cpu_copy_regs(CPUArchState *env, struct target_pt_regs *regs)
{
    CPUState *cpu = env_cpu(env);
    TaskState *ts = cpu->opaque;
    struct image_info *info = ts->info;
    int i;

    cpsr_write(env, regs->uregs[16], CPSR_USER | CPSR_EXEC,
               CPSRWriteByInstr);
    for(i = 0; i < 16; i++) {
        env->regs[i] = regs->uregs[i];
    }
#ifdef TARGET_WORDS_BIGENDIAN
    /* Enable BE8.  */
    if (EF_ARM_EABI_VERSION(info->elf_flags) >= EF_ARM_EABI_VER4
        && (info->elf_flags & EF_ARM_BE8)) {
        env->uncached_cpsr |= CPSR_E;
        env->cp15.sctlr_el[1] |= SCTLR_E0E;
    } else {
        env->cp15.sctlr_el[1] |= SCTLR_B;
    }
#endif

    ts->stack_base = info->start_stack;
    ts->heap_base = info->brk;
    /* This will be filled in on the first SYS_HEAPINFO call.  */
    ts->heap_limit = 0;
}
