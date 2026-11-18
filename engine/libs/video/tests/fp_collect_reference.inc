/* Frozen pre-pointer-hoist collector; unchanged public word decoders are shared. */
static int reference_collect(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & 1u)
            return (int)out->count;
    }
    return -1;
}
