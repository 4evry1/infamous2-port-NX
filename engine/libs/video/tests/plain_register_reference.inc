/* Frozen pre-classification dispatcher routines for state-equivalence tests. */
static inline void reference_tex_gen_note(rsx_dispatch* rsx, u32 idx, u32 old, u32 arg)
{
    if (old != arg) {
        if ((idx - (M_TEX_SIZE1 >> 2)) < 16u || (idx - (M_TEX_OFFSET >> 2)) < RSX_DSP_NUM_TEXTURES * 8u)
            ++*(rsx->tgen ? rsx->tgen : &g_rsx_dsp_tex_gen);
        else if ((idx - (0x0900u >> 2)) < (0x80u >> 2) || idx == (0x1478u >> 2))
            ++*(rsx->mgen ? rsx->mgen : &g_rsx_dsp_misc_gen);
    }
}
static void reference_plain_init(void)
{
    for (u32 m = 0; m < 0x2000u; m += 4u) {
        int p = 1;
        if (m >= M_VP_UPLOAD_INST && m < M_VP_UPLOAD_INST + 32u * 4u) p = 0;
        if (m >= M_VP_UPLOAD_CONST && m < M_VP_UPLOAD_CONST + 32u * 4u) p = 0;
        if ((m >= 0x1C00 && m < 0x1D00) || (m >= 0x1880 && m < 0x1A00) || (m >= 0x1500 && m < 0x15C0) ||
            (m >= 0x1E40 && m < 0x1E80) || (m >= 0x0A80 && m < 0x0B00)) p = 0;   /* imm_method */
        if (m == M_CLEAR_BUFFERS || m == M_VERTEX_BEGIN_END || m == M_INLINE_ARRAY ||
            m == M_VB_VERTEX_BATCH || m == M_VB_INDEX_BATCH) p = 0;
        s_plain[m >> 2] = (u8)p;
    }
    s_plain_ready = 1;
}
void reference_rsx_dispatch_burst(rsx_dispatch* rsx, u32 method, int incr, const u32* be_words, u32 count)
{
    if (!count) return;
    method &= 0xFFFFC;
    /* The fast forms only below 0x2000, with no per-word diagnostic, and once the
     * switches are read (the first call takes the slow form and reads them). */
    if (__builtin_expect(s_ed_on != 0 || s_clog_lo == -2 || !s_plain_ready, 0) ||
        method + (incr ? (count - 1u) * 4u : 0u) >= 0x2000u) {
        burst_slow(rsx, method, incr, be_words, count);
        return;
    }
    u32 i = 0;
    const int cs = !rsx->no_seen;
    /* (rsx5) A header clear of both upload windows -- most of them -- in one
     * loop without the two per-word window tests (the same per-word actions
     * as the loop below takes for such words). */
    {
        const u32 hi = incr ? method + (count - 1u) * 4u : method;
        if (s_burst_lean > 0 && (hi < M_VP_UPLOAD_CONST || method >= M_VP_UPLOAD_CONST + 32u * 4u) &&
            (hi < M_VP_UPLOAD_INST || method >= M_VP_UPLOAD_INST + 32u * 4u)) {
            /* scalar on purpose: a vectorised loop would make every call save XMM registers (rsx3) */
            /* (rsx-par) dirty tracking: the header's register range, before
             * (stores ahead of an event a cold word fires) and after (stores
             * behind it) */
            if (rsx->dirty) dsp_dirty_range(rsx, method >> 6, hi >> 6);
#pragma clang loop vectorize(disable) interleave(disable)
            for (; i < count; i++) {
                const u32 m = incr ? method + i * 4u : method;
                const u32 arg = __builtin_bswap32(__atomic_load_n(&be_words[i], __ATOMIC_RELAXED));
                if (s_plain[m >> 2]) { if (cs) rsx->seen[m >> 2]++; reference_tex_gen_note(rsx, m >> 2, rsx->regs[m >> 2], arg); rsx->regs[m >> 2] = arg; }
                else dispatch_method_cold(rsx, m, arg);
            }
            if (rsx->dirty) dsp_dirty_range(rsx, method >> 6, hi >> 6);
            return;
        }
    }
    while (i < count) {
        const u32 m = incr ? method + i * 4u : method;
        if (incr && s_clog_lo < 0 && m >= M_VP_UPLOAD_CONST && m < M_VP_UPLOAD_CONST + 32u * 4u) {
            u32 n = (M_VP_UPLOAD_CONST + 32u * 4u - m) >> 2;
            if (n > count - i) n = count - i;
            burst_const_run(rsx, m, be_words + i, n);
            i += n;
            continue;
        }
        if (incr && m >= M_VP_UPLOAD_INST && m < M_VP_UPLOAD_INST + 32u * 4u) {
            u32 n = (M_VP_UPLOAD_INST + 32u * 4u - m) >> 2;
            if (n > count - i) n = count - i;
            burst_inst_run(rsx, m, be_words + i, n);
            i += n;
            continue;
        }
        const u32 arg = __builtin_bswap32(__atomic_load_n(&be_words[i], __ATOMIC_RELAXED));
        if (s_plain[m >> 2]) {
            if (cs) rsx->seen[m >> 2]++;
            reference_tex_gen_note(rsx, m >> 2, rsx->regs[m >> 2], arg);
            rsx->regs[m >> 2] = arg;
            if (rsx->dirty) dsp_dirty(rsx, m >> 6);   /* (rsx-par) */
        }
        else dispatch_method_cold(rsx, m, arg);
        i++;
    }
}
