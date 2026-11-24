; The 128x128 unsigned view minifies to mip 1 on the 64x64 target. First clamp the sampler to mip 0,
; then permit mip 1. The view is 2D_ARRAY (one layer), sampled with an explicit layer 0, so this is the
; non-2D typed path: the implicit gradients still come from (s, t) and the layer coordinate is rounded.
; Every input byte is 0 or 255, so an unsigned D16 sample must produce 0 or 255.
; Divide each channel by 255 to compare all output components with the shared 0/1 color reference.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mov_b32_e32 v4, 0
s_load_dwordx8 s[8:15], s[0:1], 0
s_load_dwordx4 s[16:19], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
image_sample v[6:7], v[2:4], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY d16
s_waitcnt vmcnt(0)
v_bfe_u32 v9, v7, 16, 16
v_bfe_u32 v8, v7, 0, 16
v_bfe_u32 v7, v6, 16, 16
v_bfe_u32 v6, v6, 0, 16
v_cvt_f32_u32_e32 v6, v6
v_cvt_f32_u32_e32 v7, v7
v_cvt_f32_u32_e32 v8, v8
v_cvt_f32_u32_e32 v9, v9
v_mul_f32_e32 v6, 0x3b808081, v6
v_mul_f32_e32 v7, 0x3b808081, v7
v_mul_f32_e32 v8, 0x3b808081, v8
v_mul_f32_e32 v9, 0x3b808081, v9
exp mrt0 v6,v7,v8,v9 done vm
s_endpgm
