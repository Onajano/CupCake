; SHADER-1404: image_sample (implicit LOD) on a SINT 2D array reads the signed alias of binding 3, not the float view.
; vk_texture_test array-sint binds the same 8x8, 12-layer array as array-uint, as R8G8B8A8_SINT: texel (x, y) of
; layer l holds the bytes (32x, 32y, 20l, 255), each read as a signed 8-bit value. Pixel (px, py) reads texel
; (px / 8, py / 8) of layer 3, so mrt0 = (int8(32 * (px / 8)), int8(32 * (py / 8)), 60, -1) as floats: columns and
; rows 4..7 are negative (-128, -96, -64, -32) and alpha is -1.
; Reading a SINT view through the float-typed image is undefined in Vulkan (the Sampled Type must match the
; view's numeric format). On an RTX 3060 (Windows) the float path that image_sample took before SHADER-1404
; returned the same integer bits, so this fixture passes there with or without the fix; it checks that the
; typed path reads the right texels on hardware, and tells the two paths apart only on a driver that honours
; the type.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mov_b32_e32 v4, 0x40400000
s_load_dwordx8 s[8:15], s[0:1], 0
s_load_dwordx4 s[16:19], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
image_sample v[5:8], v[2:4], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY
s_waitcnt vmcnt(0)
v_cvt_f32_i32_e32 v5, v5
v_cvt_f32_i32_e32 v6, v6
v_cvt_f32_i32_e32 v7, v7
v_cvt_f32_i32_e32 v8, v8
exp mrt0 v5, v6, v7, v8 done vm
s_endpgm
