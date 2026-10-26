// ASTC LDR decoder on the CPU (docs/windows-port-plan.md C20): NVIDIA / AMD / Intel desktop GPUs have no ASTC sampling
// (textureCompressionASTC_LDR = 0), so ASTC textures are decoded to RGBA8 when uploaded. 2D blocks, all LDR colour
// endpoint modes, 1-4 partitions, dual plane, void-extent; HDR content decodes to the error colour (magenta) as an LDR
// decoder must. Output follows the decode_unorm8 rule (top 8 bits of the interpolated UNORM16 value; sRGB endpoints
// are expanded with 0x80 as the low byte).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Decodes one 16-byte block into a bw x bh RGBA8 tile (row stride dst_pitch bytes). Returns 0, or 1 when the block is
// invalid / HDR (the tile is then the error colour).
int nvn_astc_decode_block(const uint8_t blk[16], int bw, int bh, int srgb, uint8_t *dst, size_t dst_pitch);
// Decodes a w x h image whose blocks are laid out by block rows (src_pitch bytes per block row) into RGBA8.
void nvn_astc_decode_image(const uint8_t *src, size_t src_pitch, int bw, int bh, uint32_t w, uint32_t h, int srgb, uint8_t *dst, size_t dst_pitch);
