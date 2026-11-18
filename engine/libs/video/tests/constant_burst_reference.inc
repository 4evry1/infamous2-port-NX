/* Frozen pre-optimization helper for state-equivalence regression tests. */
static __attribute__((noinline)) void reference_const_run(rsx_dispatch* rsx, u32 m, const u32* be_words, u32 n)
{
    const u32 idx0 = m >> 2, w0 = (m - M_VP_UPLOAD_CONST) >> 2;
    const u32 base = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2];
    const int cs = !rsx->no_seen;
    u32 chg = 0;   /* (rsx-vtx) const_gen: any word that changed value */
    for (u32 k = 0; k < n; k++) {
        const u32 arg = __builtin_bswap32(__atomic_load_n(&be_words[k], __ATOMIC_RELAXED));
        if (cs) rsx->seen[idx0 + k]++;
        rsx->regs[idx0 + k] = arg;
        const u32 word = w0 + k, slot = base + (word >> 2);
        if (slot < RSX_DSP_NUM_CONSTANTS) {
            chg |= rsx->constants[slot][word & 3] ^ arg;
            rsx->constants[slot][word & 3] = arg;
        }
    }
    if (chg) rsx->const_gen++;   /* (rsx-vtx) */
    /* (rsx-par) the slots written (the window's own registers are not followed:
     * nothing outside the dispatcher reads them) */
    if (rsx->dirty && n) {
        const u32 s0 = base + (w0 >> 2), s1 = base + ((w0 + n - 1u) >> 2);
        if (s0 < RSX_DSP_NUM_CONSTANTS)
            dsp_dirty_range(rsx, RSX_DSP_BLK_CONST0 + s0 / 4u,
                            RSX_DSP_BLK_CONST0 + (s1 < RSX_DSP_NUM_CONSTANTS ? s1 : RSX_DSP_NUM_CONSTANTS - 1u) / 4u);
    }
}
