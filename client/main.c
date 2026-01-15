#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LINUX 1
#define X86_64 1

// #include "dr_api.h"
//
// #include "drmgr.h"
// #include "drreg.h"
// #include "drutil.h"
// #include "drvector.h"
// #include "drx.h"

#include "/opt/dynamorio/include/dr_api.h"

#include "/opt/dynamorio/ext/include/drmgr.h"
#include "/opt/dynamorio/ext/include/drreg.h"
#include "/opt/dynamorio/ext/include/drutil.h"
#include "/opt/dynamorio/ext/include/drvector.h"
#include "/opt/dynamorio/ext/include/drx.h"

/* Each mem_ref_t includes the type of reference (read or write),
 * the address referenced, and the size of the reference.
 */
typedef struct __attribute__((aligned(32))) {
    bool write;
    void *addr;
    size_t size;
    app_pc pc;
} mem_ref_t;

/* Max number of mem_ref a buffer can have */
#define MAX_NUM_MEM_REFS (1 << 13)
/* The size of memory buffer for holding mem_refs. When it fills up,
 * we dump data from the buffer to shm.
 */
#define MEM_BUF_SIZE (sizeof(mem_ref_t) * MAX_NUM_MEM_REFS)

#define SHM_RING_SIZE (1 << 20)

/* thread-private counter */
typedef struct __attribute__((aligned(64))) {
    char *buf_ptr;
    char *buf_base;
    /* buf_end holds the negative value of real address of buffer end. */
    ptr_int_t buf_end;
    void *cache;
    uint64 num_refs;
} per_thread_t;

/* cross-instrumentation-phase data */
typedef struct {
    app_pc last_pc;
} instru_data_t;

/* shared memory ring buffer */
typedef struct __attribute__((aligned(128))) {
    _Atomic uint64_t write_idx;
    _Atomic uint64_t read_idx;
    _Atomic uint64_t dropped;
    uint64_t _pad;
    mem_ref_t entries[SHM_RING_SIZE];
} shm_ring_t;

static size_t page_size;
static client_id_t client_id;
static app_pc code_cache;
static int tls_index;
static shm_ring_t *ring;

static void event_exit(void);
static void event_thread_init(void *drcontext);
static void event_thread_exit(void *drcontext);
static dr_emit_flags_t event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                        bool translating);
static dr_emit_flags_t event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                                         bool for_trace, bool translating, void **user_data);
static dr_emit_flags_t event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *where,
                                       bool for_trace, bool translating, void *user_data);

static void clean_call(void);
static void memtrace(void *drcontext);
static void code_cache_init(void);
static void code_cache_exit(void);
static void instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, app_pc pc,
                           instr_t *memref_instr, int pos, bool write);
static void shm_init(void);
static void shm_exit(void);

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    /* We need 2 reg slots beyond drreg's eflags slots => 3 slots */
    drreg_options_t ops = {sizeof(ops), 3, false};
    /* Specify priority relative to other instrumentation operations: */
    drmgr_priority_t priority = {sizeof(priority), /* size of struct */
                                 "brainium",       /* name of our operation */
                                 NULL, /* optional name of operation we should precede */
                                 NULL, /* optional name of operation we should follow */
                                 0};   /* numeric priority */
    dr_set_client_name("Brainium Client", "https://github.com/chip44/brainium/issues");
    page_size = dr_page_size();
    drmgr_init();
    drutil_init();
    client_id = id;
    dr_register_exit_event(event_exit);
    if (!drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit) ||
        !drmgr_register_bb_app2app_event(event_bb_app2app, &priority) ||
        !drmgr_register_bb_instrumentation_event(event_bb_analysis, event_bb_insert, &priority) ||
        drreg_init(&ops) != DRREG_SUCCESS || !drx_init()) {
        /* something is wrong: can't continue */
        DR_ASSERT(false);
        return;
    }
    tls_index = drmgr_register_tls_field();
    DR_ASSERT(tls_index != -1);

    shm_init();
    code_cache_init();
    /* make it easy to tell, by looking at log file, which client executed */
    dr_log(NULL, DR_LOG_ALL, 1, "Brainium client initializing\n");
}

static void
event_exit()
{
    code_cache_exit();
    shm_exit();

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
    per_thread_t *data = NULL;

    /* allocate thread private data */
    data = dr_thread_alloc(drcontext, sizeof(per_thread_t));
    drmgr_set_tls_field(drcontext, tls_index, data);
    data->buf_base = dr_thread_alloc(drcontext, MEM_BUF_SIZE);
    data->buf_ptr = data->buf_base;
    /* set buf_end to be negative of address of buffer end for the lea later */
    data->buf_end = -(ptr_int_t)(data->buf_base + MEM_BUF_SIZE);
    data->num_refs = 0;
}

static void
event_thread_exit(void *drcontext)
{
    per_thread_t *data = NULL;

    memtrace(drcontext);
    data = drmgr_get_tls_field(drcontext, tls_index);
    dr_thread_free(drcontext, data->buf_base, MEM_BUF_SIZE);
    dr_thread_free(drcontext, data, sizeof(per_thread_t));
}

/* we transform string loops into regular loops so we can more easily
 * monitor every memory reference they make
 */
static dr_emit_flags_t
event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
    if (!drutil_expand_rep_string(drcontext, bb)) {
        DR_ASSERT(false);
        /* in release build, carry on: we'll just miss per-iter refs */
    }
    if (!drx_expand_scatter_gather(drcontext, bb, NULL)) {
        DR_ASSERT(false);
    }
    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating,
                  void **user_data)
{
    instru_data_t *data = (instru_data_t *)dr_thread_alloc(drcontext, sizeof(*data));
    data->last_pc = NULL;
    *user_data = (void *)data;
    return DR_EMIT_DEFAULT;
}

/* event_bb_insert calls instrument_mem to instrument every
 * application memory reference.
 */
static dr_emit_flags_t
event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *where, bool for_trace,
                bool translating, void *user_data)
{
    int i = 0;
    instru_data_t *data = (instru_data_t *)user_data;
    /* Use the drmgr_orig_app_instr_* interface to properly handle our own use
     * of drutil_expand_rep_string() and drx_expand_scatter_gather() (as well
     * as another client/library emulating the instruction stream).
     */
    instr_t *instr_fetch = drmgr_orig_app_instr_for_fetch(drcontext);
    if (instr_fetch != NULL) {
        data->last_pc = instr_get_app_pc(instr_fetch);
    }
    app_pc last_pc = data->last_pc;
    if (drmgr_is_last_instr(drcontext, where)) {
        dr_thread_free(drcontext, data, sizeof(*data));
    }

    instr_t *instr_operands = drmgr_orig_app_instr_for_operands(drcontext);
    if (instr_operands == NULL ||
        (!instr_writes_memory(instr_operands) && !instr_reads_memory(instr_operands))) {
        return DR_EMIT_DEFAULT;
    }

    DR_ASSERT(instr_is_app(instr_operands));
    DR_ASSERT(last_pc != NULL);

    if (instr_reads_memory(instr_operands)) {
        for (i = 0; i < instr_num_srcs(instr_operands); i++) {
            if (opnd_is_memory_reference(instr_get_src(instr_operands, i))) {
                instrument_mem(drcontext, bb, where, last_pc, instr_operands, i, false);
            }
        }
    }

    if (instr_writes_memory(instr_operands)) {
        for (i = 0; i < instr_num_dsts(instr_operands); i++) {
            if (opnd_is_memory_reference(instr_get_dst(instr_operands, i))) {
                instrument_mem(drcontext, bb, where, last_pc, instr_operands, i, true);
            }
        }
    }

    return DR_EMIT_DEFAULT;
}

static void
memtrace(void *drcontext)
{
    per_thread_t *data = drmgr_get_tls_field(drcontext, tls_index);

    mem_ref_t *mem_ref = (mem_ref_t *)data->buf_base;
    size_t num_refs = (mem_ref_t *)data->buf_ptr - mem_ref;
    if (num_refs == 0) {
        return;
    }

    uint64_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);

    uint64_t free_slots = SHM_RING_SIZE - (w - r);

    if (num_refs > free_slots) {
        // drop newest events that don't fit
        atomic_fetch_add_explicit(&ring->dropped, num_refs - free_slots, memory_order_relaxed);
        num_refs = free_slots;
    }

    for (size_t i = 0; i < num_refs; i++) {
        ring->entries[(w + i) & (SHM_RING_SIZE - 1)] = mem_ref[i];
    }

    atomic_store_explicit(&ring->write_idx, w + num_refs, memory_order_release);

    memset(data->buf_base, 0, MEM_BUF_SIZE);
    data->num_refs += num_refs;
    data->buf_ptr = data->buf_base;
}

/* clean_call dumps the memory reference info to shm */
static void
clean_call(void)
{
    void *drcontext = dr_get_current_drcontext();
    memtrace(drcontext);
}

static void
code_cache_init(void)
{
    void *drcontext = NULL;
    instrlist_t *ilist = NULL;
    instr_t *where = NULL;
    byte *end = NULL;

    drcontext = dr_get_current_drcontext();
    code_cache = dr_nonheap_alloc(page_size, DR_MEMPROT_READ | DR_MEMPROT_WRITE | DR_MEMPROT_EXEC);
    ilist = instrlist_create(drcontext);
    /* The lean procedure simply performs a clean call, and then jumps back
     * to the DR code cache.
     */
    where = INSTR_CREATE_jmp_ind(drcontext, opnd_create_reg(DR_REG_XCX));
    instrlist_meta_append(ilist, where);
    /* clean call */
    dr_insert_clean_call(drcontext, ilist, where, (void *)clean_call, false, 0);
    /* Encodes the instructions into memory and then cleans up. */
    end = instrlist_encode(drcontext, ilist, code_cache, false);
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
 * It inserts code before the memory reference to to fill the memory buffer
 * and jump to our own code cache to call the clean_call when the buffer is full.
 */
static void
instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, app_pc pc,
               instr_t *memref_instr, int pos, bool write)
{
    instr_t *instr = NULL;
    instr_t *call = NULL;
    instr_t *restore = NULL;
    opnd_t ref;
    opnd_t opnd1;
    opnd_t opnd2;
    reg_id_t reg1 = 0;
    reg_id_t reg2 = 0;
    drvector_t allowed;

    /* Steal two scratch registers.
     * reg2 must be ECX or RCX for jecxz.
     */
    drreg_init_and_fill_vector(&allowed, false);
    drreg_set_vector_entry(&allowed, DR_REG_XCX, true);
    if (drreg_reserve_register(drcontext, ilist, where, &allowed, &reg2) != DRREG_SUCCESS ||
        drreg_reserve_register(drcontext, ilist, where, NULL, &reg1) != DRREG_SUCCESS) {
        DR_ASSERT(false); /* cannot recover */
        drvector_delete(&allowed);
        return;
    }
    drvector_delete(&allowed);

    if (write) {
        ref = instr_get_dst(memref_instr, pos);
    } else {
        ref = instr_get_src(memref_instr, pos);
    }

    /* use drutil to get mem address */
    drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg1, reg2);

    /* The following assembly performs the following instructions
     * buf_ptr->write = write;
     * buf_ptr->addr  = addr;
     * buf_ptr->size  = size;
     * buf_ptr->pc    = pc;
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

    /* Store pc in memory ref */
    /* For 64-bit, we can't use a 64-bit immediate so we split pc into two halves.
     * We could alternatively load it into reg1 and then store reg1.
     * We use a convenience routine that does the two-step store for us.
     */
    opnd1 = OPND_CREATE_MEMPTR(reg2, offsetof(mem_ref_t, pc));
    instrlist_insert_mov_immed_ptrsz(drcontext, (ptr_int_t)pc, opnd1, ilist, where, NULL, NULL);

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

static void
shm_init(void)
{
    int fd = shm_open("/brainium_shm", O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(shm_ring_t));

    ring = mmap(NULL, sizeof(shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);

    memset(ring, 0, sizeof(*ring));
}

static void
shm_exit(void)
{
    if (ring) {
        munmap(ring, sizeof(shm_ring_t));
        ring = NULL;
    }
    shm_unlink("/brainium_shm");
}
