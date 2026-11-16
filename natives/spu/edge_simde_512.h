/* edge_simde_512.h -- the 512-bit fused multiply-add family for edge223_native.c's SIMDe forms (see
 * edge_simde.h item 2): included after SIMDe's avx512 type headers. */
#ifndef EDGE_SIMDE_512_H
#define EDGE_SIMDE_512_H
EDGE_FMA_FAMILY(512, simde__m512, , edge_f16)
#undef EDGE_FMA_FAMILY
#define simde_mm512_fmadd_ps  edge_fmadd512
#define simde_mm512_fmsub_ps  edge_fmsub512
#define simde_mm512_fnmadd_ps edge_fnmadd512
#endif
