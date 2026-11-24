"""image_sample (implicit LOD) on a UINT/SINT image: the T# FORMAT selects the sampled view in every dimension.

The measured rule (`docs/design.md`, "Implicit integer samples" and the 2D `image_sample_lz` /
`image_sample_l` rows) is that the T# FORMAT picks the float, UINT or SINT sampled view, so an integer texel
arrives as its integer bits. Until this change only a 2D pixel shader took that dispatch: a 2D-array, cube, 3D
or 1D implicit sample, and any implicit sample outside the pixel stage, read the float view of an integer T#.

Standard library only (spirv-as/spirv-val are used when they are on PATH); no GPU, no LLVM, no game files.

    python -I tests/semantic/test_sample_implicit_dims.py -v
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import support  # noqa: E402

DESC = "s[8:15], s[16:19]"
PRO = ["v_mov_b32 v4, 1.0", "v_mov_b32 v5, 0.5", "v_mov_b32 v6, 0.25", "v_mov_b32 v7, 0.75"]
# A compute-shader header (RSRC2, group size 32x1x1) so `image_sample` outside the pixel stage is translated.
CS_FSSM = bytearray(0x148)
struct.pack_into("<4I", CS_FSSM, 0x138, 0x8088, 32, 1, 1)
CS_FSSM = bytes(CS_FSSM)
# dimension, address registers, the index image_read_typed uses for its integer aliases (STORE_DIMS), Grad width
DIMS = [
    ("dim:SQ_RSRC_IMG_2D", "v[4:6]", 0, 2),
    ("dim:SQ_RSRC_IMG_2D_ARRAY", "v[4:7]", 1, 2),
    ("dim:SQ_RSRC_IMG_CUBE", "v[4:7]", 1, 2),
    ("dim:SQ_RSRC_IMG_3D", "v[4:7]", 2, 3),
    ("dim:SQ_RSRC_IMG_1D", "v[4:5]", 3, 1),
]


def translate(name, dim, regs, pixel):
    line = f"image_sample v8, {regs}, {DESC} dmask:0xf {dim}"
    if pixel:
        lines = PRO + [line, "exp mrt0 v8, v8, v8, v8 done vm", "s_endpgm"]
        return support.translate_lines(name, lines)
    return support.translate_lines(name, PRO + [line, "s_endpgm"], fssm=CS_FSSM)


def validate(spv):
    """Assemble and validate the module when SPIRV-Tools is on PATH (Linux CI installs it)."""
    spirv_as, spirv_val = shutil.which("spirv-as"), shutil.which("spirv-val")
    if not (spirv_as and spirv_val):
        return
    with tempfile.TemporaryDirectory() as tmp:
        asm, obj = os.path.join(tmp, "m.spvasm"), os.path.join(tmp, "m.spv")
        with open(asm, "w", encoding="utf-8", newline="\n") as f:
            f.write(spv)
        r = subprocess.run([spirv_as, "--target-env", "vulkan1.2", "-o", obj, asm],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f"spirv-as failed:\n{r.stderr}")
        r = subprocess.run([spirv_val, "--target-env", "vulkan1.2", obj], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f"spirv-val failed:\n{r.stderr}")


def samples(spv, ty):
    return [ln.strip() for ln in spv.splitlines() if f"= OpImageSampleExplicitLod {ty} " in ln]


class SampleImplicitDims(unittest.TestCase):
    def test_every_dimension_samples_the_view_the_format_selects(self):
        for dim, regs, di, width in DIMS:
            with self.subTest(dim=dim):
                spv = translate("ps_implicit_" + re.sub(r"\W", "_", dim), dim, regs, pixel=True)
                validate(spv)
                # The three views of the dimension's sampled binding are read at the descriptor index.
                for cls in (1, 2):
                    self.assertIn(f"OpAccessChain %_ptr_UC_itx{cls}_{di} %itx{cls}_{di} ", spv)
                self.assertEqual(len(samples(spv, "%v4uint_st")), 1)
                self.assertEqual(len(samples(spv, "%v4int_st")), 1)
                self.assertEqual(len(samples(spv, "%v4float")), 1)
                # The pixel shader keeps its implicit LOD: the quad derivatives, at the width Grad takes for
                # this dimension (no array layer, three components for 3D, scalar for 1D).
                gty = "%float" if width == 1 else f"%v{width}float"
                self.assertEqual(len(re.findall(rf"= OpDPdx {re.escape(gty)} ", spv)), 1)
                self.assertEqual(len(re.findall(rf"= OpDPdy {re.escape(gty)} ", spv)), 1)

    def test_float_image_still_reads_the_dimension_float_view(self):
        for dim, regs, di, width in DIMS:
            with self.subTest(dim=dim):
                spv = translate("ps_float_" + re.sub(r"\W", "_", dim), dim, regs, pixel=True)
                view = {0: "%images", 1: "%images_arr", 2: "%images_3d", 3: "%images_1d"}[di]
                img_ty = {0: "%img2d", 1: "%img2da", 2: "%img3d", 3: "%img1d"}[di]
                ptr, load = "OpAccessChain", "OpLoad"
                self.assertRegex(spv, rf"{ptr} %_ptr_UC_\w+ {view} ")
                # The float branch samples the image loaded from that view, not one of the integer aliases.
                float_sample = samples(spv, "%v4float")[0]
                sampled = float_sample.split()[4]
                self.assertRegex(spv, rf"{re.escape(sampled)} = OpSampledImage \S+ %t\d+ ")
                src = re.search(rf"{re.escape(sampled)} = OpSampledImage \S+ (%\w+) ", spv).group(1)
                self.assertRegex(spv, rf"{re.escape(src)} = {load} {img_ty} ")

    def test_outside_the_pixel_stage_an_integer_image_samples_lod_zero(self):
        spv = translate("cs_implicit_2d", "dim:SQ_RSRC_IMG_2D", "v[4:6]", pixel=False)
        validate(spv)
        self.assertIn("OpAccessChain %_ptr_UC_itx1_0 %itx1_0 ", spv)
        self.assertIn("OpAccessChain %_ptr_UC_itx2_0 %itx2_0 ", spv)
        for ty in ("%v4uint_st", "%v4int_st", "%v4float"):
            (line,) = samples(spv, ty)
            self.assertTrue(line.endswith(" Lod %f0"), line)
        self.assertNotIn("OpDPdx", spv)
        self.assertNotIn("OpDPdy", spv)


if __name__ == "__main__":
    unittest.main()
