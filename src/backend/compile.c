#include "graphviz/dot.h"
#include "x86_64/abi.h"
#include "x86_64/assemble.h"
#include "x86_64/elf.h"
#include "x86_64/instr.h"
#include "compile.h"
#include <lacc/cli.h>

#include <assert.h>
#include <stdarg.h>

static enum compile_target compile_target;
static FILE *output_stream;

static int (*enter_context)(const struct symbol *);
static int (*emit_instruction)(struct instruction);
static int (*emit_data)(struct immediate);
static int (*flush_backend)(void);

/* Values from va_list initialization.
 */
static struct {
    int gp_offset;
    int fp_offset;
    int overflow_arg_area_offset;
    int reg_save_area_offset;
} vararg;

/* Registers used for parameter passing and return value.
 */
static enum reg param_int_reg[] = {DI, SI, DX, CX, R8, R9};
static enum reg ret_int_reg[] = {AX, DX};

/* Store incoming PARAM operations before CALL.
 */
static array_of(struct var) func_args;

static void compile_block(struct block *block, const struct typetree *type);

static void emit(enum opcode opcode, enum instr_optype optype, ...)
{
    va_list args;
    struct instruction instr = {0};

    instr.opcode = opcode;
    instr.optype = optype;
    va_start(args, optype);
    switch (optype) {
    case OPT_IMM:
        instr.source.imm = va_arg(args, struct immediate);
        break;
    case OPT_REG:
        instr.source.reg = va_arg(args, struct registr);
        break;
    case OPT_MEM:
        instr.source.mem = va_arg(args, struct memory);
        break;
    case OPT_REG_REG:
        instr.source.reg = va_arg(args, struct registr);
        instr.dest.reg = va_arg(args, struct registr);
        break;
    case OPT_REG_MEM:
        instr.source.reg = va_arg(args, struct registr);
        instr.dest.mem = va_arg(args, struct memory);
        break;
    case OPT_MEM_REG:
        instr.source.mem = va_arg(args, struct memory);
        instr.dest.reg = va_arg(args, struct registr);
        break;
    case OPT_IMM_REG:
        instr.source.imm = va_arg(args, struct immediate);
        instr.dest.reg = va_arg(args, struct registr);
        break;
    case OPT_IMM_MEM:
        instr.source.imm = va_arg(args, struct immediate);
        instr.dest.mem = va_arg(args, struct memory);
        break;
    case OPT_NONE:
        break;
    }

    va_end(args);
    emit_instruction(instr);
}

static struct registr reg(enum reg r, int w)
{
    struct registr v = {0};
    v.r = r;
    v.w = w;
    return v;
}

static int is_string(struct var val)
{
    return
        val.kind == IMMEDIATE && val.symbol &&
        val.symbol->symtype == SYM_STRING_VALUE;
}

static struct immediate value_of(struct var var, int w)
{
    struct immediate imm = {0};
    assert(var.kind == IMMEDIATE);
    assert(!is_array(var.type));

    imm.w = w;
    if (is_string(var)) {
        assert(is_pointer(var.type));
        imm.type = IMM_ADDR;
        imm.d.addr.sym = var.symbol;
        imm.d.addr.disp = var.offset;
    } else {
        assert(!var.offset);
        assert(is_scalar(var.type));
        assert(w == 1 || w == 2 || w == 4 || w == 8);
        imm.d.qword = var.imm.i;
    }

    return imm;
}

static struct memory location(struct address addr, int w)
{
    struct memory loc = {{0}};
    loc.addr = addr;
    loc.w = w;
    return loc;
}

static struct address address_of(struct var var)
{
    struct address addr = {0};
    assert(var.kind == DIRECT);

    if (var.symbol->linkage != LINK_NONE) {
        addr.base = IP;
        addr.disp = var.offset;
        addr.sym = var.symbol;
    } else {
        addr.disp = var.symbol->stack_offset + var.offset;
        addr.base = BP;
    }

    return addr;
}

static struct memory location_of(struct var var, int w)
{
    return location(address_of(var), w);
}

static struct address address(int disp, enum reg base, enum reg off, int mult)
{
    struct address addr = {0};
    addr.disp = disp;
    addr.base = base;
    addr.offset = off;
    addr.mult = mult;
    return addr;
}

static struct immediate addr(const struct symbol *sym)
{
    struct immediate imm = {IMM_ADDR};
    imm.d.addr.sym = sym;
    return imm;
}

static struct immediate constant(int n, int w)
{
    return value_of(var_int(n), w);
}

/* Load variable v to register r, sign extended to fit register size.
 * Width must be either 4 (as in %eax) or 8 (as in %rax).
 */
static enum reg load_value(struct var v, enum reg r, int w)
{
    enum opcode opcode = INSTR_MOV;
    int s = size_of(v.type);

    /* We operate only with 32 or 64 bit register values, but variables
     * can be stored with byte or short width. Promote to 32 bit if
     * required. */
    assert(w == 4 || w == 8);
    assert(s <= w);

    if (is_unsigned(v.type)) {
        if (s < w && s < 4)
            opcode = INSTR_MOVZX;
    } else if (s != w)
        opcode = INSTR_MOVSX;

    /* Special case for unsigned extension from 32 to 64 bit, for which
     * there is no instruction 'movzlq', but rather just 'movl'. */
    if (s == 4 && is_unsigned(v.type) && w == 8) {
        opcode = INSTR_MOV;
        w = 4;
    }

    switch (v.kind) {
    case DIRECT:
        emit(opcode, OPT_MEM_REG, location_of(v, s), reg(r, w));
        break;
    case DEREF:
        assert(is_pointer(&v.symbol->type));
        load_value(var_direct(v.symbol), R11, size_of(&v.symbol->type));
        emit(opcode, OPT_MEM_REG,
            location(address(v.offset, R11, 0, 0), s), reg(r, w));
        break;
    case IMMEDIATE:
        emit(INSTR_MOV, OPT_IMM_REG, value_of(v, w), reg(r, w));
        break;
    }

    return r;
}

/* Load variable to register, automatically sign/width extended to
 * either 32 or 64 bit.
 */
static enum reg load(struct var v, enum reg r)
{
    int w = size_of(v.type);
    if (w <= 4) w = 4;
    else
        assert(w == 8);

    return load_value(v, r, w);
}

static void load_address(struct var v, enum reg r)
{
    if (v.kind == DIRECT) {
        emit(INSTR_LEA, OPT_MEM_REG, location_of(v, 8), reg(r, 8));
    } else {
        assert(v.kind == DEREF);
        assert(v.symbol->stack_offset);
        assert(is_pointer(&v.symbol->type));

        load(var_direct(v.symbol), r);
        if (v.offset)
            emit(INSTR_ADD, OPT_IMM_REG, constant(v.offset, 8), reg(r, 8));
    }
}

static void store(enum reg r, struct var v)
{
    const int w = size_of(v.type);
    assert(is_scalar(v.type) || w <= 8);

    if (v.width) {
        assert(w == size_of(&basic_type__int));
        emit(INSTR_AND, OPT_IMM_REG, constant((1 << v.width) - 1, w), reg(r, w));
    }

    if (v.kind == DIRECT) {
        assert(!is_array(v.type));

        emit(INSTR_MOV, OPT_REG_MEM, reg(r, w), location_of(v, w));
    } else {
        assert(v.kind == DEREF);
        assert(is_pointer(&v.symbol->type));

        load_value(var_direct(v.symbol), R11, size_of(&v.symbol->type));
        emit(INSTR_MOV, OPT_REG_MEM,
            reg(r, w), location(address(v.offset, R11, 0, 0), w));
    }
}

/* Push value to stack, rounded up to always be 8 byte aligned.
 */
static void push(struct var v)
{
    int eb;

    if (is_scalar(v.type)) {
        if (v.kind == IMMEDIATE && size_of(v.type) == 8)
            emit(INSTR_PUSH, OPT_IMM, value_of(v, 8));
        else {
            load(v, AX);
            emit(INSTR_PUSH, OPT_REG, reg(AX, 8));
        }
    } else {
        eb = EIGHTBYTES(v.type);
        emit(INSTR_SUB, OPT_IMM_REG, constant(eb * 8, 8), reg(SP, 8));
        emit(INSTR_MOV, OPT_IMM_REG, constant(eb, 4), reg(CX, 4));
        emit(INSTR_MOV, OPT_REG_REG, reg(SP, 8), reg(DI, 8));
        load_address(v, SI);
        emit(INSTR_REP_MOVSQ, OPT_NONE);
    }
}

/* Compile a function call. For now this assumes only integer arguments
 * passed in %rdi, %rsi, %rdx, %rcx, %r8 and %r9, and arguments passed
 * on stack.
 */
static void call(
    int n_args,
    const struct var *args,
    struct var res,
    struct var func)
{
    int i, j, n,
        register_args = 0,
        mem_used = 0,
        next_integer_reg = 0;
    const struct typetree *type;
    struct var var;
    struct param_class respc, arg;
    struct {
        struct param_class pc;
        int i;
    } argpc[MAX_REGISTER_ARGS];

    /* Handle both function call by direct reference and pointer. The
     * former is a special case. */
    type = is_pointer(func.type) ? func.type->next : func.type;
    respc = classify(type->next);
    if (respc.eightbyte[0] == PC_MEMORY) {
        next_integer_reg = 1;
    }

    /* Classify parameters, handing out register slots on first come,
     * first serve basis. */
    for (i = 0; i < n_args; ++i) {
        arg = classify(args[i].type);
        n = EIGHTBYTES(args[i].type);
        if (arg.eightbyte[0] != PC_MEMORY &&
            next_integer_reg + n <= MAX_INTEGER_ARGS)
        {
            assert(register_args < MAX_INTEGER_ARGS);
            argpc[register_args].pc = arg;
            argpc[register_args].i = i;
            register_args++;
            next_integer_reg += n;
        } else {
            mem_used += 8*EIGHTBYTES(args[i].type);
        }
    }

    /* Make sure stack is aligned to 16 byte after pushing all memory
     * arguments. Insert padding if necessary. */
    if (mem_used % 16) {
        mem_used += 8;
        assert((mem_used % 16) == 0);
        emit(INSTR_SUB, OPT_IMM_REG, constant(8, 8), reg(SP, 8));
    }

    /* Pass arguments on stack from right to left. Do this before
     * populating registers, because %rdi, %rsi etc will be used to do
     * the pushing. Reverse traversal in register class list to filter
     * out non-memory arguments. */
    for (i = n_args - 1, n = register_args - 1; i >= 0; --i) {
        if (n >= 0 && argpc[n].i == i) {
            n--;
        } else {
            push(args[i]);
        }
    }

    /* When return value is MEMORY, pass a pointer to stack as hidden
     * first argument. */
    if (respc.eightbyte[0] == PC_MEMORY) {
        next_integer_reg = 1;
        load_address(res, param_int_reg[0]);
    } else {
        next_integer_reg = 0;
    }

    /* Pass arguments in registers from left to right. Partition
     * arguments into eightbyte slices and load into appropriate
     * registers. */
    for (i = 0; i < register_args; ++i) {
        var = args[argpc[i].i];
        n = EIGHTBYTES(var.type);
        if (is_struct_or_union(var.type)) {
            for (j = 0; j < n; ++j) {
                var.type = (j < n - 1) ?
                    &basic_type__unsigned_long :
                    BASIC_TYPE_UNSIGNED(size_of(args[argpc[i].i].type) % 8);
                load(var, param_int_reg[next_integer_reg++]);
                var.offset += size_of(var.type);
            }
        } else {
            assert(size_of(args[argpc[i].i].type) <= 8);
            load(var, param_int_reg[next_integer_reg++]);
        }
    }

    /* For variable argument lists, %al contains the number of vector
     * registers used. */
    if (is_vararg(type)) {
        emit(INSTR_MOV, OPT_IMM_REG, constant(0, 4), reg(AX, 4));
    }

    /* Call. */
    if (is_pointer(func.type)) {
        load(func, R11);
        emit(INSTR_CALL, OPT_REG, reg(R11, 8));
    } else {
        if (func.kind == DIRECT)
            emit(INSTR_CALL, OPT_IMM, addr(func.symbol));
        else {
            assert(func.kind == DEREF);
            load_address(func, R11);
            emit(INSTR_CALL, OPT_REG, reg(R11, 8));
        }
    }

    /* Reset stack pointer from overflow arguments. */
    if (mem_used) {
        emit(INSTR_ADD, OPT_IMM_REG, constant(mem_used, 8), reg(SP, 8));
    }

    /* Move return value from register(s) to memory. Return values with
     * class MEMORY have already been written by callee. */
    if (respc.eightbyte[0] != PC_MEMORY) {
        n = EIGHTBYTES(res.type);
        assert(n <= 2);
        var = res;
        for (i = 0; i < n; ++i) {
            var.type = (i < n - 1) ?
                &basic_type__unsigned_long :
                BASIC_TYPE_UNSIGNED(size_of(res.type) % 8);
            store(ret_int_reg[i], var);
            var.offset += size_of(var.type);
        }
    }
}

static void enter(
    const struct typetree *type,
    struct list *params,
    struct list *locals)
{
    int i, j, n,
        register_args = 0,  /* Arguments passed in registers. */
        next_integer_reg = 0,
        mem_offset = 16,    /* Offset of PC_MEMORY parameters. */
        stack_offset = 0;   /* Offset of %rsp to for local variables. */
    struct var ref;
    struct symbol *sym;
    struct param_class res, arg;
    struct {
        struct param_class pc;
        int i;
    } argpc[MAX_REGISTER_ARGS];
    assert(is_function(type));

    /* Address of return value is passed as first integer argument. If
     * return value is MEMORY, store the address at stack offset -8. */
    res = classify(type->next);
    if (res.eightbyte[0] == PC_MEMORY) {
        stack_offset = -8;
        next_integer_reg = 1;
    }

    /* For functions with variable argument list, reserve a fixed area
     * at the beginning of the stack fram for register values. In total
     * there are 8 bytes for each of the 6 integer registers, and 16
     * bytes for each of the 8 SSE registers, for a total of 176 bytes.
     * If return type is MEMORY, the return address is automatically
     * included in register spill area. */
    if (is_vararg(type)) {
        stack_offset = -176;
    }

    /* Assign storage to parameters. Guarantee that parameters are 8-
     * byte aligned also for those passed by register, which makes it
     * easier to store in local frame after entering function. Assumes
     * only integer or memory classification. */
    for (i = 0; i < list_len(params); ++i) {
        sym = (struct symbol *) list_get(params, i);
        arg = classify(&sym->type);
        n = EIGHTBYTES(&sym->type);
        assert(!sym->stack_offset);
        assert(sym->linkage == LINK_NONE);
        if (arg.eightbyte[0] != PC_MEMORY) {
            if (next_integer_reg + n <= 6) {
                next_integer_reg += n;
            } else {
                arg.eightbyte[0] = PC_MEMORY;
            }
        }

        if (arg.eightbyte[0] == PC_MEMORY) {
            sym->stack_offset = mem_offset;
            mem_offset += n * 8;
        } else {
            assert(register_args < MAX_REGISTER_ARGS);
            argpc[register_args].pc = arg;
            argpc[register_args].i = i;
            register_args++;
            stack_offset -= n * 8;
            sym->stack_offset = stack_offset;
        }
    }

    /* Assign storage to locals. */
    for (i = 0; i < list_len(locals); ++i) {
        sym = (struct symbol *) list_get(locals, i);
        assert(!sym->stack_offset);
        if (sym->linkage == LINK_NONE) {
            stack_offset -= size_of(&sym->type);
            sym->stack_offset = stack_offset;
        }
    }

    /* Make invariant to have %rsp aligned to 0x10, which is mandatory
     * according to the ABI for function calls. On entry, (%rsp + 8) is
     * a multiple of 16, and after pushing %rbp we have alignment at
     * this point. Alignment must also be considered when calling other
     * functions, to push memory divisible by 16. */
    if (stack_offset < 0) {
        i = (-stack_offset) % 16;
        stack_offset -= 16 - i;
    }

    /* Allocate space in the call frame to hold local variables. Store
     * return address to well known location -8(%rbp). */
    if (stack_offset < 0) {
        emit(INSTR_SUB, OPT_IMM_REG, constant(-stack_offset, 8), reg(SP, 8));
        if (res.eightbyte[0] == PC_MEMORY) {
            emit(INSTR_MOV, OPT_REG_MEM,
                reg(param_int_reg[0], 8), location(address(-8, BP, 0, 0), 8));
        }
    }

    /* Store all potential parameters to register save area. This
     * includes parameters that are known to be passed as registers,
     * that will anyway be stored to another stack location. It is
     * desireable to skip touching floating point unit if possible,
     * %al holds the number of registers passed. */
    if (is_vararg(type)) {
        sym = sym_create_label();
        vararg.gp_offset = 8 * next_integer_reg;
        vararg.fp_offset = 0;
        vararg.overflow_arg_area_offset = mem_offset;
        vararg.reg_save_area_offset = 0;
        emit(INSTR_TEST, OPT_REG_REG, reg(AX, 1), reg(AX, 1));
        emit(INSTR_JZ, OPT_IMM, addr(sym));
        for (i = 0; i < 8; ++i) {
            vararg.reg_save_area_offset -= 16;
            emit(INSTR_MOVAPS, OPT_REG_MEM,
                reg(XMM0 + (7 - i), 16),
                location(address(vararg.reg_save_area_offset, BP, 0, 0), 16));
        }

        enter_context(sym);
        for (i = 0; i < 6; ++i) {
            vararg.reg_save_area_offset -= 8;
            emit(INSTR_MOV, OPT_REG_MEM,
                reg(param_int_reg[5 - i], 8),
                location(address(vararg.reg_save_area_offset, BP, 0, 0), 8));
        }
    }

    /* Move arguments from register to stack, looking only in array of
     * stored classifications that are not memory. Remember not to load
     * from the first register if used for return address. */
    next_integer_reg = (res.eightbyte[0] == PC_MEMORY);
    for (i = 0; i < register_args; ++i) {
        assert(argpc[i].pc.eightbyte[0] != PC_MEMORY);
        assert(argpc[i].i < list_len(params));

        sym = (struct symbol *) list_get(params, argpc[i].i);
        ref = var_direct(sym);
        n = EIGHTBYTES(&sym->type);
        for (j = 0; j < n; ++j) {
            assert(argpc[i].pc.eightbyte[j] == PC_INTEGER);
            ref.type = (j < n - 1) ?
                &basic_type__unsigned_long :
                BASIC_TYPE_UNSIGNED(size_of(&sym->type) % 8);
            store(param_int_reg[next_integer_reg++], ref);
            ref.offset += size_of(ref.type);
        }
    }
}

/* Return value from function, placing it in register(s) or writing it
 * to stack, based on parameter class.
 */
static void ret(struct var val, const struct typetree *type)
{
    int i, n;
    struct var var;
    struct param_class pc;

    pc = classify(val.type);
    if (pc.eightbyte[0] != PC_MEMORY) {
        n = EIGHTBYTES(val.type);
        if (n > 1) {
            var = val;
            for (i = 0; i < n; ++i) {
                assert(pc.eightbyte[i] == PC_INTEGER);
                var.type = (i < n - 1) ?
                    &basic_type__unsigned_long :
                    BASIC_TYPE_UNSIGNED(size_of(val.type) % 8);
                load(var, ret_int_reg[i]);
                var.offset += size_of(var.type);
            }
        } else {
            assert(n == 1);
            assert(pc.eightbyte[0] == PC_INTEGER);
            load(val, ret_int_reg[0]);
        }
    } else {
        /* Load return address from magic stack offset and copy result.
         * Return address is stored in -8(%rbp), unless the function
         * takes variable argument lists, in which case it is read
         * from register spill area. */
        if (is_vararg(type)) {
            emit(INSTR_MOV, OPT_MEM_REG,
                location(address(-176, BP, 0, 0), 8), reg(DI, 8));
        } else {
            emit(INSTR_MOV, OPT_MEM_REG,
                location(address(-8, BP, 0, 0), 8), reg(DI, 8));
        }

        load_address(val, SI);
        emit(INSTR_MOV, OPT_IMM_REG,
            constant(size_of(val.type), 8), reg(DX, 4));
        emit(INSTR_CALL, OPT_IMM, addr(decl_memcpy));

        /* The ABI specifies that the address should be in %rax on
         * return. */
        emit(INSTR_MOV, OPT_MEM_REG,
            location(address(-8, BP, 0, 0), 8), reg(AX, 8));
    }
}

/* Execute call to va_start, initializing the provided va_list object.
 * Values are taken from static context set during enter().
 */
static void compile__builtin_va_start(struct var args)
{
    assert(args.kind == DIRECT);

    emit(INSTR_MOV, OPT_IMM_MEM,
        constant(vararg.gp_offset, 4),
        location_of(args, 4));
    args.offset += 4;
    emit(INSTR_MOV, OPT_IMM_MEM,
        constant(vararg.fp_offset, 4),
        location_of(args, 4));
    args.offset += 4;
    emit(INSTR_LEA, OPT_MEM_REG,
        location(address(vararg.overflow_arg_area_offset, BP, 0, 0), 8),
        reg(AX, 8));
    emit(INSTR_MOV, OPT_REG_MEM,
        reg(AX, 8),
        location_of(args, 8));
    args.offset += 8;
    emit(INSTR_LEA, OPT_MEM_REG,
        location(address(vararg.reg_save_area_offset, BP, 0, 0), 8),
        reg(AX, 8));
    emit(INSTR_MOV, OPT_REG_MEM,
        reg(AX, 8),
        location_of(args, 8));
}

/* Output logic for fetching a parameter from calling va_arg(args, T).
 */
static void compile__builtin_va_arg(struct var res, struct var args)
{
    const int w = size_of(res.type);
    struct param_class pc;

    struct var
        var_gp_offset = args,
        var_fp_offset = args,
        var_overflow_arg_area = args,
        var_reg_save_area = args;

    /* Get some unique jump labels. */
    const struct symbol
        *memory = sym_create_label(),
        *done = sym_create_label();

    /* Might be too restrictive for res, but simplifies some codegen. */
    assert(res.kind == DIRECT);
    assert(args.kind == DIRECT);

    pc = classify(res.type);

    /* References into va_list object. */
    var_gp_offset.type = &basic_type__unsigned_int;
    var_fp_offset.offset += 4;
    var_fp_offset.type = &basic_type__unsigned_int;
    var_overflow_arg_area.offset += 8;
    var_overflow_arg_area.type = &basic_type__unsigned_long;
    var_reg_save_area.offset += 16;
    var_reg_save_area.type = &basic_type__unsigned_long;

    /* Integer or SSE parameters are read from registers, if there are
     * enough of them left. Otherwise read from overflow area. */
    if (pc.eightbyte[0] != PC_MEMORY) {
        struct var slice = res;
        int i,
            size = w,
            num_gp = 0, /* general purpose registers needed */
            num_fp = 0; /* floating point registers needed */

        for (i = 0; i < EIGHTBYTES(res.type); ++i) {
            if (pc.eightbyte[i] == PC_INTEGER) {
                num_gp++;
            } else {
                assert(pc.eightbyte[i] == PC_SSE);
                num_fp++;
            }
        }

        /* Keep reg_save_area in register for the remainder, a pointer
         * to stack where registers are stored. This value does not
         * change. */
        load(var_reg_save_area, SI);

        /* Check whether there are enough registers left for the
         * argument to be passed in. */
        if (num_gp) {
            load(var_gp_offset, CX);
            emit(INSTR_CMP, OPT_IMM_REG,
                constant(6*8 - 8*num_gp, 4), reg(CX, 4));
            emit(INSTR_JA, OPT_IMM, addr(memory));
        }
        if (num_fp) {
            assert(0); /* No actual float support yet. */
            load(var_fp_offset, DX);
            emit(INSTR_CMP, OPT_IMM_REG,
                constant(6*8 - 8*num_fp, 4), reg(DX, 4));
            emit(INSTR_JA, OPT_IMM, addr(memory));
        }

        /* Load argument, one eightbyte at a time. This code has a lot
         * in common with enter, ret etc, potential for refactoring
         * probably. */
        for (i = 0; i < EIGHTBYTES(res.type); ++i) {
            int width = (size < 8) ? size : 8;

            size -= width;
            assert(pc.eightbyte[i] == PC_INTEGER);
            assert(width == 1 || width == 2 || width == 4 || width == 8);

            slice.type = BASIC_TYPE_UNSIGNED(width);
            slice.offset = res.offset + i * 8;

            /* Advanced addressing, loading (%rsi + 8*i + (%rcx * 1))
             * into %rax. Base of registers are stored in %rsi, first
             * pending register is at offset %rcx, and i counts number
             * of registers done. In GNU assembly it is
             * {i*8}(%rsi, %rcx, 1). */
            emit(INSTR_MOV, OPT_MEM_REG,
                location(address(i*8, SI, CX, 1), width), reg(AX, width));
            store(AX, slice);
        }

        /* Store updated offsets to va_list. */
        if (num_gp) {
            assert(var_gp_offset.kind == DIRECT);
            emit(INSTR_ADD, OPT_IMM_MEM,
                constant(8 * num_gp, 4), location_of(var_gp_offset, 4));
        }
        if (num_fp) {
            assert(0);
            assert(var_fp_offset.kind == DIRECT);
            emit(INSTR_ADD, OPT_IMM_MEM,
                constant(16 * num_fp, 4), location_of(var_fp_offset, 4));
        }

        emit(INSTR_JMP, OPT_IMM, addr(done));
        enter_context(memory);
    }

    /* Parameters that are passed on stack will be read from
     * overflow_arg_area. This is also the fallback when arguments
     * do not fit in remaining registers. */
    load(var_overflow_arg_area, SI);
    if (w <= 8) {
        assert(res.kind == DIRECT);
        emit(INSTR_MOV, OPT_MEM_REG,
            location(address(0, SI, 0, 0), w), reg(AX, w));
        emit(INSTR_MOV, OPT_REG_MEM, reg(AX, w), location_of(res, w));
    } else {
        load_address(res, DI);
        emit(INSTR_MOV, OPT_IMM_REG, constant(w, 8), reg(DX, 8));
        emit(INSTR_CALL, OPT_IMM, addr(decl_memcpy));
    }

    /* Move overflow_arg_area pointer to position of next memory
     * argument, aligning to 8 byte. */
    emit(INSTR_ADD, OPT_IMM_MEM,
        constant(EIGHTBYTES(res.type) * 8, 4),
        location_of(var_overflow_arg_area, 4));

    if (pc.eightbyte[0] != PC_MEMORY) {
        enter_context(done);
    }
}

static void compile_op(const struct op *op)
{
    static int w;

    switch (op->type) {
    case IR_ASSIGN:
        /* Handle special case of char [] = string literal. This will
         * only occur as part of initializer, at block scope. External
         * definitions are handled before this. At no other point should
         * array types be seen in assembly backend. We handle these
         * assignments with memcpy, other compilers load the string into
         * register as ascii numbers. */
        if (is_array(op->a.type) || is_array(op->b.type)) {
            int size = size_of(op->a.type);

            assert(op->a.kind == DIRECT);
            assert(is_string(op->b));
            assert(type_equal(op->a.type, op->b.type));

            load_address(op->a, DI);
            emit(INSTR_MOV, OPT_IMM_REG, addr(op->b.symbol), reg(SI, 8));
            emit(INSTR_MOV, OPT_IMM_REG, constant(size, 8), reg(DX, 8));
            emit(INSTR_CALL, OPT_IMM, addr(decl_memcpy));
            break;
        }
        /* Struct or union assignment, values that cannot be loaded into
         * a single register. */
        else if (size_of(op->a.type) > 8) {
            int size = size_of(op->a.type);
            assert(size_of(op->a.type) == size_of(op->b.type));

            load_address(op->a, DI);
            load_address(op->b, SI);

            emit(INSTR_MOV, OPT_IMM_REG, constant(size, 8), reg(DX, 8));
            emit(INSTR_CALL, OPT_IMM, addr(decl_memcpy));
            break;
        }
        /* Fallthrough, assignment has implicit cast for convenience and
         * to make static initialization work without explicit casts. */
    case IR_CAST:
        w = (size_of(op->a.type) > size_of(op->b.type)) ?
            size_of(op->a.type) : size_of(op->b.type);
        w = (w < 4) ? 4 : w;
        assert(w == 4 || w == 8);
        load_value(op->b, AX, w);
        store(AX, op->a);
        break;
    case IR_DEREF:
        load(op->b, CX);
        emit(INSTR_MOV, OPT_MEM_REG,
            location(address(0, CX, 0, 0), size_of(op->a.type)),
            reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_PARAM:
        array_push_back(&func_args, op->a);
        break;
    case IR_CALL:
        call(array_len(&func_args), func_args.data, op->a, op->b);
        array_empty(&func_args);
        break;
    case IR_ADDR:
        load_address(op->b, AX);
        store(AX, op->a);
        break;
    case IR_NOT:
        load(op->b, AX);
        emit(INSTR_NOT, OPT_REG, reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_OP_ADD:
        load_value(op->b, AX, size_of(op->a.type));
        load_value(op->c, CX, size_of(op->a.type));
        emit(INSTR_ADD, OPT_REG_REG,
            reg(CX, size_of(op->a.type)), reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_OP_SUB:
        load_value(op->b, AX, size_of(op->a.type));
        load_value(op->c, CX, size_of(op->a.type));
        emit(INSTR_SUB, OPT_REG_REG,
            reg(CX, size_of(op->a.type)), reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_OP_MUL:
        load(op->c, AX);
        if (op->b.kind == DIRECT) {
            emit(INSTR_MUL, OPT_MEM, location_of(op->b, size_of(op->b.type)));
        } else {
            load(op->b, CX);
            emit(INSTR_MUL, OPT_REG, reg(CX, size_of(op->b.type)));
        }
        store(AX, op->a);
        break;
    case IR_OP_DIV:
    case IR_OP_MOD:
        /* %rdx must be zero to avoid SIGFPE. */
        emit(INSTR_XOR, OPT_REG_REG, reg(DX, 8), reg(DX, 8));
        load(op->b, AX);
        if (op->c.kind == DIRECT) {
            emit(INSTR_DIV, OPT_MEM, location_of(op->c, size_of(op->c.type)));
        } else {
            load(op->c, CX);
            emit(INSTR_DIV, OPT_REG, reg(CX, size_of(op->c.type)));
        }
        store((op->type == IR_OP_DIV) ? AX : DX, op->a);
        break;
    case IR_OP_AND:
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_AND, OPT_REG_REG, reg(CX, 8), reg(AX, 8));
        store(AX, op->a);
        break;
    case IR_OP_OR:
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_OR, OPT_REG_REG, reg(CX, 8), reg(AX, 8));
        store(AX, op->a);
        break;
    case IR_OP_XOR:
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_XOR, OPT_REG_REG, reg(CX, 8), reg(AX, 8));
        store(AX, op->a);
        break;
    case IR_OP_SHL:
        /* Shift instruction encoding is either by immediate, or
         * implicit %cl register. Encode as if something other than %cl
         * could be chosen. Behavior is undefined if shift is greater
         * than integer width, so don't care about overflow or sign. */
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_SHL, OPT_REG_REG, reg(CX, 1), reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_OP_SHR:
        load(op->b, AX);
        load(op->c, CX);
        emit((is_unsigned(op->a.type)) ? INSTR_SHR : INSTR_SAR, OPT_REG_REG,
            reg(CX, 1), reg(AX, size_of(op->a.type)));
        store(AX, op->a);
        break;
    case IR_OP_EQ:
        assert(size_of(op->a.type) == 4);
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_CMP, OPT_REG_REG,
            reg(CX, size_of(op->a.type)), reg(AX, size_of(op->a.type)));
        emit(INSTR_SETZ, OPT_REG, reg(AX, 1));
        emit(INSTR_MOVZX, OPT_REG_REG, reg(AX, 1), reg(AX, 4));
        store(AX, op->a);
        break;
    case IR_OP_GE:
        assert(size_of(op->a.type) == 4);
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_CMP, OPT_REG_REG,
            reg(CX, size_of(op->a.type)), reg(AX, size_of(op->a.type)));
        if (is_unsigned(op->b.type)) {
            assert(is_unsigned(op->c.type));
            emit(INSTR_SETAE, OPT_REG, reg(AX, 1));
        } else {
            emit(INSTR_SETGE, OPT_REG, reg(AX, 1));
        }
        emit(INSTR_MOVZX, OPT_REG_REG, reg(AX, 1), reg(AX, 4));
        store(AX, op->a);
        break;
    case IR_OP_GT:
        assert(size_of(op->a.type) == 4);
        load(op->b, AX);
        load(op->c, CX);
        emit(INSTR_CMP, OPT_REG_REG,
            reg(CX, size_of(op->a.type)), reg(AX, size_of(op->a.type)));
        if (is_unsigned(op->b.type)) {
            assert(is_unsigned(op->c.type));
            /* When comparison is unsigned, set flag without considering
             * overflow; CF=0 && ZF=0. */ 
            emit(INSTR_SETA, OPT_REG, reg(AX, 1));
        } else {
            emit(INSTR_SETG, OPT_REG, reg(AX, 1));
        }
        emit(INSTR_MOVZX, OPT_REG_REG, reg(AX, 1), reg(AX, 4));
        store(AX, op->a);
        break;
    case IR_VA_START:
        compile__builtin_va_start(op->a);
        break;
    case IR_VA_ARG:
        compile__builtin_va_arg(op->a, op->b);
        break;
    default:
        assert(0);
        break;
    }
}

static void tail_cmp_jump(struct block *block, const struct typetree *type)
{
    struct instruction instr = {0};
    struct op *cmp = &array_get(&block->code, array_len(&block->code) - 1);

    /* Target of assignment should be temporary, thus we do not lose any
     * side effects from not storing the value to stack. */
    assert(!cmp->a.lvalue);

    load(cmp->c, CX);
    load(cmp->b, AX);
    emit(INSTR_CMP, OPT_REG_REG,
        reg(CX, size_of(cmp->a.type)), reg(AX, size_of(cmp->a.type)));

    switch (cmp->type) {
    case IR_OP_EQ:
        instr.opcode = INSTR_JZ;
        break;
    case IR_OP_GE:
        instr.opcode = (is_unsigned(cmp->b.type)) ? INSTR_JAE : INSTR_JGE;
        break;
    default:
        assert(cmp->type == IR_OP_GT);
        instr.opcode = (is_unsigned(cmp->b.type)) ? INSTR_JA : INSTR_JG;
        break;
    }

    instr.optype = OPT_IMM;
    instr.source.imm = addr(block->jump[1]->label);
    emit_instruction(instr);

    if (block->jump[0]->color == BLACK)
        emit(INSTR_JMP, OPT_IMM, addr(block->jump[0]->label));
    else
        compile_block(block->jump[0], type);

    compile_block(block->jump[1], type);
}

static void tail_generic(struct block *block, const struct typetree *type)
{
    if (!block->jump[0] && !block->jump[1]) {
        /* Don't actually check if the function should return what we
         * have, assume that is done before. */
        if (block->has_return_value) {
            assert(block->expr.type);
            assert(!is_void(block->expr.type));
            ret(block->expr, type);
        }

        emit(INSTR_LEAVE, OPT_NONE);
        emit(INSTR_RET, OPT_NONE);
    } else if (!block->jump[1]) {
        if (block->jump[0]->color == BLACK)
            emit(INSTR_JMP, OPT_IMM, addr(block->jump[0]->label));
        else
            compile_block(block->jump[0], type);
    } else {
        load(block->expr, AX);
        emit(INSTR_CMP, OPT_IMM_REG, constant(0, 4), reg(AX, 4));
        emit(INSTR_JZ, OPT_IMM, addr(block->jump[0]->label));
        if (block->jump[1]->color == BLACK)
            emit(INSTR_JMP, OPT_IMM, addr(block->jump[1]->label));
        else
            compile_block(block->jump[1], type);
        compile_block(block->jump[0], type);
    }
}

static void compile_block(struct block *block, const struct typetree *type)
{
    int i, length;
    struct op *op = NULL;

    if (block->color == WHITE) {
        block->color = BLACK;
        length = array_len(&block->code);
        enter_context(block->label);

        /* Visit all operations, except the last one. */
        for (i = 0; i < length; ++i) {
            op = &array_get(&block->code, i);
            if (i < length - 1)
                compile_op(op);
        }

        /* Special case on comparison + jump, saving some space by not
         * writing the result of comparison (always a temporary). */
        if (op && IS_COMPARISON(op->type) && block->jump[1]) {
            assert(block->jump[0]);
            tail_cmp_jump(block, type);
        } else {
            if (op)
                compile_op(op);
            tail_generic(block, type);
        }
    }
}

static void compile_data_assign(struct var target, struct var val)
{
    int size = target.type->size;
    struct immediate imm = {0};

    assert(target.kind == DIRECT);
    assert(val.kind == IMMEDIATE);

    imm.w = size;
    switch (target.type->type) {
    case T_POINTER:
        if (is_string(val)) {
            imm.type = IMM_ADDR;
            imm.d.addr.sym = val.symbol;
            imm.d.addr.disp = val.offset;
            break;
        }
    case T_SIGNED:
    case T_UNSIGNED:
        imm.type = IMM_INT;
        imm.d.qword = val.imm.i;
        break;
    case T_ARRAY:
        if (is_string(val)) {
            imm.type = IMM_STRING;
            imm.d.string = val.symbol->string_value;
            break;
        }
    default:
        assert(0);
        break;
    }

    emit_data(imm);
}

static void zero_fill_data(size_t bytes)
{
    struct immediate
        zero_byte = {IMM_INT, 1},
        zero_quad = {IMM_INT, 8};

    while (bytes > size_of(&basic_type__long)) {
        emit_data(zero_quad);
        bytes -= size_of(&basic_type__long);
    }

    while (bytes--)
        emit_data(zero_byte);
}

static void compile_data(struct definition *def)
{
    struct op *op;
    int i,
        total_size = size_of(&def->symbol->type),
        initialized = 0;

    enter_context(def->symbol);
    for (i = 0; i < array_len(&def->body->code); ++i) {
        op = &array_get(&def->body->code, i);

        assert(op->type == IR_ASSIGN);
        assert(op->a.kind == DIRECT);
        assert(op->a.symbol == def->symbol);
        assert(op->a.offset >= initialized);

        zero_fill_data(op->a.offset - initialized);
        compile_data_assign(op->a, op->b);
        initialized = op->a.offset + size_of(op->a.type);
    }

    assert(total_size >= initialized);
    zero_fill_data(total_size - initialized);
}

static void compile_function(struct definition *def)
{
    assert(is_function(&def->symbol->type));
    enter_context(def->symbol);
    emit(INSTR_PUSH, OPT_REG, reg(BP, 8));
    emit(INSTR_MOV, OPT_REG_REG, reg(SP, 8), reg(BP, 8));

    /* Make sure parameters and local variables are placed on stack.
     * Keep parameter class of return value for later assembling return
     * statement. */
    enter(&def->symbol->type, &def->params, &def->locals);

    /* Recursively assemble body. */
    compile_block(def->body, &def->symbol->type);
}

void set_compile_target(FILE *stream, enum compile_target target)
{
    compile_target = target;
    output_stream = stream;
    switch (target) {
    case TARGET_NONE:
    case TARGET_IR_DOT:
        break;
    case TARGET_x86_64_ASM:
        asm_output = stream;
        enter_context = asm_symbol;
        emit_instruction = asm_text;
        emit_data = asm_data;
        flush_backend = asm_flush;
        break;
    case TARGET_x86_64_ELF:
        object_file_output = stream;
        enter_context = elf_symbol;
        emit_instruction = elf_text;
        emit_data = elf_data;
        flush_backend = elf_flush;
        break;
    }
}

int compile(struct definition *def)
{
    assert(decl_memcpy);
    assert(def->symbol);

    switch (compile_target) {
    case TARGET_IR_DOT:
        fdotgen(output_stream, def);
    case TARGET_NONE:
        break;
    case TARGET_x86_64_ASM:
    case TARGET_x86_64_ELF:
        if (is_function(&def->symbol->type))
            compile_function(def);
        else
            compile_data(def);
        break;
    }

    return 0;
}

int declare(const struct symbol *sym)
{
    switch (compile_target) {
    case TARGET_x86_64_ASM:
    case TARGET_x86_64_ELF:
        return enter_context(sym);
    default:
        return 0;
    }
}

void flush(void)
{
    array_clear(&func_args);
    if (flush_backend) {
        flush_backend();
    }
}
