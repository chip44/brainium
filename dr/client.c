#include <assert.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dr_api.h"
#include "dr_tools.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"
#include "drvector.h"
#include "drx.h"

#include "ring.h"
#include "shm.h"

/*
 * max number of mem_ref_t a buffer can have
 */
#define MAX_NUM_MEM_REFS (1 << 13)

/*
 * the size of memory buffer for holding mem_ref_t's.
 * when it fills up, we dump data from the buffer to shm.
 */
#define MEM_BUF_SIZE (sizeof(mem_ref_t) * MAX_NUM_MEM_REFS)

_Static_assert(RING_SIZE >= MEM_BUF_SIZE / sizeof(mem_ref_t),
               "ring size must be >= largest possible num_refs");

/* thread-private counter */
typedef struct __attribute__((aligned(64))) {
    char *buf_ptr;
    char *buf_base;
    /* buf_end holds the negative value of real address of buffer end. */
    ptr_int_t buf_end;
    void *cache;
    uint64 num_refs;
} per_thread_t;

static size_t page_size;
static app_pc code_cache;
static int tls_index;
static ring_buf_t *ring_shm = NULL;

static void event_exit(void);
static void event_thread_init(void *drcontext);
static void event_thread_exit(void *drcontext);
static dr_emit_flags_t event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                        bool translating);
static dr_emit_flags_t event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *where,
                                       bool for_trace, bool translating, void *user_data);

static void clean_call(void);
static void memtrace(void *drcontext);
static void code_cache_init(void);
static void code_cache_exit(void);
static void instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where,
                           instr_t *memref_instr, uint pos, bool write);

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    (void)id;
    (void)argc;
    (void)argv;

    drreg_options_t ops = {
        .struct_size = sizeof(ops),
        /* We need 2 reg slots beyond drreg's eflags slots => 3 slots */
        .num_spill_slots = 3,
    };
    /* Specify priority relative to other instrumentation operations: */
    drmgr_priority_t priority = {
        .struct_size = sizeof(priority),
        .name = "brainium",
        .priority = 0,
    };
    dr_set_client_name("Brainium client", "https://github.com/chip44/brainium/issues");
    page_size = dr_page_size();
    drmgr_init();
    drutil_init();
    drmgr_register_exit_event(event_exit);
    if (!drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit) ||
        !drmgr_register_bb_app2app_event(event_bb_app2app, &priority) ||
        !drmgr_register_bb_instrumentation_event(NULL, event_bb_insert, &priority) ||
        drreg_init(&ops) != DRREG_SUCCESS || !drx_init()) {
        DR_ASSERT(false);
        return;
    }
    tls_index = drmgr_register_tls_field();
    DR_ASSERT(tls_index != -1);

    ring_shm = shm_init(false);
    if (!ring_shm) {
        DR_ASSERT(false);
        return;
    }

    code_cache_init();
    /* make it easy to tell, by looking at log file, which client executed */
    dr_log(NULL, DR_LOG_ALL, 1, "Brainium client initializing\n");
}

static void
event_exit(void)
{
    code_cache_exit();

    if (!drmgr_unregister_tls_field(tls_index) ||
        !drmgr_unregister_thread_init_event(event_thread_init) ||
        !drmgr_unregister_thread_exit_event(event_thread_exit) ||
        !drmgr_unregister_bb_insertion_event(event_bb_insert) || drreg_exit() != DRREG_SUCCESS) {
        DR_ASSERT(false);
    }

    drutil_exit();
    drmgr_exit();
    drx_exit();
}

static void
event_thread_init(void *drcontext)
{
    // allocate thread-private data
    per_thread_t *data = dr_thread_alloc(drcontext, sizeof(per_thread_t));
    drmgr_set_tls_field(drcontext, tls_index, data);

    /*
     * use dr_raw_mem_alloc instead of dr_thread_alloc to get page-aligned memory for mem_ref_t's
     * 32-byte alignment requirement (dr_thread_alloc only guarantees pointer-size alignment).
     */
    data->buf_base = dr_raw_mem_alloc(MEM_BUF_SIZE, DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
    DR_ASSERT(data->buf_base != NULL);

    data->buf_ptr = data->buf_base;

    // set buf_end to be negative of address of buffer end for the lea later
    data->buf_end = -(ptr_int_t)(data->buf_base + MEM_BUF_SIZE);

    data->num_refs = 0;
}

static void
event_thread_exit(void *drcontext)
{
    memtrace(drcontext);
    per_thread_t *data = drmgr_get_tls_field(drcontext, tls_index);
    dr_raw_mem_free(data->buf_base, MEM_BUF_SIZE);
    dr_thread_free(drcontext, data, sizeof(per_thread_t));
}

/*
 * transform string loops into regular loops so we can more easily monitor every memory reference
 * they make
 */
static dr_emit_flags_t
event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
    (void)tag;
    (void)for_trace;
    (void)translating;

    if (!drutil_expand_rep_string(drcontext, bb)) {
        DR_ASSERT(false);
        /* in release build, carry on: we'll just miss per-iter refs */
    }

    if (!drx_expand_scatter_gather(drcontext, bb, NULL)) {
        DR_ASSERT(false);
    }

    return DR_EMIT_DEFAULT;
}

/*
 * event_bb_insert calls instrument_mem to instrument every application memory reference.
 */
static dr_emit_flags_t
event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *where, bool for_trace,
                bool translating, void *user_data)
{
    (void)tag;
    (void)for_trace;
    (void)translating;
    (void)user_data;

    instr_t *instr_operands = drmgr_orig_app_instr_for_operands(drcontext);
    if (instr_operands == NULL ||
        (!instr_writes_memory(instr_operands) && !instr_reads_memory(instr_operands))) {
        return DR_EMIT_DEFAULT;
    }

    DR_ASSERT(instr_is_app(instr_operands));

    if (instr_reads_memory(instr_operands)) {
        for (uint i = 0; i < (uint)instr_num_srcs(instr_operands); i++) {
            if (opnd_is_memory_reference(instr_get_src(instr_operands, i))) {
                instrument_mem(drcontext, bb, where, instr_operands, i, false);
            }
        }
    }

    if (instr_writes_memory(instr_operands)) {
        for (uint i = 0; i < (uint)instr_num_dsts(instr_operands); i++) {
            if (opnd_is_memory_reference(instr_get_dst(instr_operands, i))) {
                instrument_mem(drcontext, bb, where, instr_operands, i, true);
            }
        }
    }

    return DR_EMIT_DEFAULT;
}

static void
memtrace(void *drcontext)
{
    per_thread_t *data = drmgr_get_tls_field(drcontext, tls_index);

    const mem_ref_t *mem_ref = (mem_ref_t *)data->buf_base;
    const size_t num_refs = (size_t)((mem_ref_t *)data->buf_ptr - mem_ref);
    if (num_refs == 0) {
        return;
    }

    // write entries to ring buffer
    ring_push_bulk(ring_shm, mem_ref, num_refs);

    // reset buffer pointers
    data->num_refs += num_refs;
    data->buf_ptr = data->buf_base;
}

/*
 * clean_call dumps the memory reference info to shm
 */
static void
clean_call(void)
{
    void *drcontext = dr_get_current_drcontext();
    memtrace(drcontext);
}

static void
code_cache_init(void)
{
    void *drcontext = dr_get_current_drcontext();
    code_cache = dr_nonheap_alloc(page_size, DR_MEMPROT_READ | DR_MEMPROT_WRITE | DR_MEMPROT_EXEC);
    instrlist_t *ilist = instrlist_create(drcontext);
    /* The lean procedure simply performs a clean call, and then jumps back
     * to the DR code cache.
     */
    instr_t *where = INSTR_CREATE_jmp_ind(drcontext, opnd_create_reg(DR_REG_XCX));
    instrlist_meta_append(ilist, where);
    /* clean call */
    dr_insert_clean_call(drcontext, ilist, where, (void *)clean_call, false, 0);
    /* Encodes the instructions into memory and then cleans up. */
    byte *end = instrlist_encode(drcontext, ilist, code_cache, false);
    DR_ASSERT((size_t)(end - code_cache) < page_size);
    instrlist_clear_and_destroy(drcontext, ilist);
    /* set the memory as just +rx now */
    dr_memory_protect(code_cache, page_size, DR_MEMPROT_READ | DR_MEMPROT_EXEC);
}

static void
code_cache_exit(void)
{
    dr_nonheap_free(code_cache, page_size);
}

/*
 * instrument_mem is called whenever a memory reference is identified.
 * it inserts code before the memory reference to to fill the memory buffer and jump to our own code
 * cache to call the clean_call when the buffer is full.
 */
static void
instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, instr_t *memref_instr, uint pos,
               bool write)
{
    instr_t *instr = NULL;
    instr_t *call = NULL;
    instr_t *restore = NULL;
    opnd_t opnd1;
    opnd_t opnd2;
    reg_id_t reg1 = 0;
    reg_id_t reg2 = 0;

    /* Steal two scratch registers.
     * reg2 must be ECX or RCX for jecxz.
     */
    drvector_t allowed;
    drreg_init_and_fill_vector(&allowed, false);
    drreg_set_vector_entry(&allowed, DR_REG_XCX, true);
    if (drreg_reserve_register(drcontext, ilist, where, &allowed, &reg2) != DRREG_SUCCESS ||
        drreg_reserve_register(drcontext, ilist, where, NULL, &reg1) != DRREG_SUCCESS) {
        DR_ASSERT(false); /* cannot recover */
        drvector_delete(&allowed);
        return;
    }
    drvector_delete(&allowed);

    opnd_t ref = write ? instr_get_dst(memref_instr, pos) : instr_get_src(memref_instr, pos);

    /* use drutil to get mem address */
    drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg1, reg2);

    /* The following assembly performs the following instructions
     * buf_ptr->write = write;
     * buf_ptr->addr  = addr;
     * buf_ptr->size  = size;
     * buf_ptr++;
     * if (buf_ptr >= buf_end_ptr)
     *    clean_call();
     */
    drmgr_insert_read_tls_field(drcontext, tls_index, ilist, where, reg2);
    opnd1 = opnd_create_reg(reg2);
    opnd2 = OPND_CREATE_MEMPTR(reg2, offsetof(per_thread_t, buf_ptr));
    instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* Move write/read to write field */
    opnd1 = OPND_CREATE_MEM32(reg2, offsetof(mem_ref_t, write));
    opnd2 = OPND_CREATE_INT32(write);
    instr = INSTR_CREATE_mov_imm(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* Store address in memory ref */
    opnd1 = OPND_CREATE_MEMPTR(reg2, offsetof(mem_ref_t, addr));
    opnd2 = opnd_create_reg(reg1);
    instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* Store size in memory ref */
    opnd1 = OPND_CREATE_MEMPTR(reg2, offsetof(mem_ref_t, size));
    /* drutil_opnd_mem_size_in_bytes handles OP_enter */
    opnd2 = OPND_CREATE_INT32(drutil_opnd_mem_size_in_bytes(ref, memref_instr));
    instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* Increment reg value by pointer size using lea instr */
    opnd1 = opnd_create_reg(reg2);
    opnd2 = opnd_create_base_disp(reg2, DR_REG_NULL, 0, sizeof(mem_ref_t), OPSZ_lea);
    instr = INSTR_CREATE_lea(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    drmgr_insert_read_tls_field(drcontext, tls_index, ilist, where, reg1);
    opnd1 = OPND_CREATE_MEMPTR(reg1, offsetof(per_thread_t, buf_ptr));
    opnd2 = opnd_create_reg(reg2);
    instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* we use lea + jecxz trick for better performance
     * lea and jecxz won't disturb the eflags, so we won't insert
     * code to save and restore application's eflags.
     */
    /* lea [reg2 - buf_end] => reg2 */
    opnd1 = opnd_create_reg(reg1);
    opnd2 = OPND_CREATE_MEMPTR(reg1, offsetof(per_thread_t, buf_end));
    instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);
    opnd1 = opnd_create_reg(reg2);
    opnd2 = opnd_create_base_disp(reg1, reg2, 1, 0, OPSZ_lea);
    instr = INSTR_CREATE_lea(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);

    /* jecxz call */
    call = INSTR_CREATE_label(drcontext);
    opnd1 = opnd_create_instr(call);
    instr = INSTR_CREATE_jecxz(drcontext, opnd1);
    instrlist_meta_preinsert(ilist, where, instr);

    /* jump restore to skip clean call */
    restore = INSTR_CREATE_label(drcontext);
    opnd1 = opnd_create_instr(restore);
    instr = INSTR_CREATE_jmp(drcontext, opnd1);
    instrlist_meta_preinsert(ilist, where, instr);

    /* clean call */
    /* We jump to lean procedure which performs full context switch and
     * clean call invocation. This is to reduce the code cache size.
     */
    instrlist_meta_preinsert(ilist, where, call);
    /* mov restore DR_REG_XCX */
    opnd1 = opnd_create_reg(reg2);
    /* this is the return address for jumping back from lean procedure */
    opnd2 = opnd_create_instr(restore);
    /* We could use instrlist_insert_mov_instr_addr(), but with a register
     * destination we know we can use a 64-bit immediate.
     */
    instr = INSTR_CREATE_mov_imm(drcontext, opnd1, opnd2);
    instrlist_meta_preinsert(ilist, where, instr);
    /* jmp code_cache */
    opnd1 = opnd_create_pc(code_cache);
    instr = INSTR_CREATE_jmp(drcontext, opnd1);
    instrlist_meta_preinsert(ilist, where, instr);

    /* Restore scratch registers */
    instrlist_meta_preinsert(ilist, where, restore);
    if (drreg_unreserve_register(drcontext, ilist, where, reg1) != DRREG_SUCCESS ||
        drreg_unreserve_register(drcontext, ilist, where, reg2) != DRREG_SUCCESS) {
        DR_ASSERT(false);
    }
}
