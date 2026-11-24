; ps_sample_implicit_uint on a one-layer 2D array (vk_texture_test --two-mip-uint-array binds the same 128x128
; two-mip unsigned image at binding 3). An implicit sample outside 2D must also take the UINT view the T# FORMAT
; selects (SHADER-1404); through the float view the D16 halves are not 0 or 255. Layer coordinate 0.0.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mov_b32 v4, 0
s_load_dwordx8 s[8:15], s[0:1], 0
s_load_dwordx4 s[16:19], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
image_sample v[6:7], v[2:4], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY d16
s_waitcnt vmcnt(0)
v_bfe_u32 v5, v6, 16, 16
v_bfe_u32 v4, v6, 0, 16
v_bfe_u32 v6, v7, 0, 16
v_bfe_u32 v7, v7, 16, 16
v_cvt_f32_u32_e32 v4, v4
v_cvt_f32_u32_e32 v5, v5
v_cvt_f32_u32_e32 v6, v6
v_cvt_f32_u32_e32 v7, v7
v_mul_f32_e32 v4, 0x3b808081, v4
v_mul_f32_e32 v5, 0x3b808081, v5
v_mul_f32_e32 v6, 0x3b808081, v6
v_mul_f32_e32 v7, 0x3b808081, v7
exp mrt0 v4,v5,v6,v7 done vm
s_endpgm
