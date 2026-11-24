"""image_sample, image_sample_l and image_sample_lz read a UINT/SINT image through its integer view in every
dimension and stage.

Standard library only (spirv-as and spirv-val are used when they are on PATH); no GPU, no game files.

The hardware picks the sampled view from T# word 1 FORMAT in every dimension, and image_load has read the uint/sint
alias of the dimension's sampled binding in all of them (SHADER-151). Sampling took that path for 2D only, so a UINT
2D-array, 3D, cube or 1D texture was read through the float view and the VGPRs held a converted float instead of the
integer texel. A plain image_sample took it only in the pixel shader's 2D form (SHADER-1404): in the PS it now
samples every dimension with the gradients of the coordinates an implicit LOD derives from (not the array layer),
and outside the PS at LOD 0, as its float form does. Here: the alias each dimension loads, the sampled-image type it pairs with the sampler, that the float
view is still sampled for a float format, and that the modules validate. The GPU pixel fixtures are
runtime/test/shaders/ps_sample_implicit_uint.asm and ps_sample_implicit_sint.asm (2D); a non-2D integer texture is the
remaining pixel check.
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

import support

DESC = "s[8:15], s[16:19]"
PRO = ["v_mov_b32 v4, 1.0", "v_mov_b32 v5, 0.5", "v_mov_b32 v6, 0.25", "v_mov_b32 v7, 0.75",
       "v_mov_b32 v8, 0"]

# dim -> (address range, vdata, dmask, STORE_DIMS index the typed aliases are declared for, sampled binding)
DIMS = {
    "1D": ("v[4:5]", "v9", "0x1", 3, 5),
    "2D": ("v[4:6]", "v[9:12]", "0xf", 0, 0),
    "2D_ARRAY": ("v[4:7]", "v[9:12]", "0xf", 1, 3),
    "3D": ("v[4:7]", "v[9:12]", "0xf", 2, 6),
    "CUBE": ("v[4:7]", "v[9:12]", "0xf", 1, 3),      # a cube samples its 6-layer 2D-array view
}


# A compute shader's FSSM: the dispatch state the translator reads (wave32, 32x1x1 threads).
CS_FSSM = bytearray(0x148)
struct.pack_into("<4I", CS_FSSM, 0x138, 0x8088, 32, 1, 1)
CS_FSSM = bytes(CS_FSSM)


def translate(name, line):
    if name.startswith("cs_"):
        return support.translate_lines(name, PRO + [line, "s_endpgm"], fssm=CS_FSSM)
    return support.translate_lines(name, PRO + [line, "exp mrt0 v9, v9, v9, v9 done vm", "s_endpgm"])


def sample(op, dim):
    addr, vdata, dmask, _, _ = DIMS[dim]
    if op == "image_sample":          # no LOD dword
        first, last = addr.strip("v[]").split(":")
        addr = f"v{first}" if int(last) - int(first) == 1 else f"v[{first}:{int(last) - 1}]"
    return f"{op} {vdata}, {addr}, {DESC} dmask:{dmask} dim:SQ_RSRC_IMG_{dim}"


def validate(spv):
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, "m.spvasm"), os.path.join(tmp, "m.spv")
        with open(a, "w", encoding="utf-8", newline="\n") as f:
            f.write(spv)
        subprocess.run(["spirv-as", "--target-env", "vulkan1.2", a, "-o", b], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", b], check=True)


class SampleTypedDims(unittest.TestCase):
    def test_both_integer_classes_alias_the_dimension_sampled_binding(self):
        for op in ("image_sample_l", "image_sample_lz"):
            for dim, (_, _, _, di, binding) in DIMS.items():
                with self.subTest(op=op, dim=dim):
                    spv = translate("ps_sampletyped", sample(op, dim))
                    for cls in (1, 2):
                        self.assertRegex(spv, rf"OpAccessChain %_ptr_UC_itx{cls}_{di} %itx{cls}_{di} ")
                        self.assertIn(f"OpDecorate %itx{cls}_{di} Binding {binding}", spv)
                        self.assertIn(f"%itxsamp{cls}_{di} = OpTypeSampledImage %itximg{cls}_{di}", spv)
                    self.assertRegex(spv, r"= OpImageSampleExplicitLod %v4uint_st %")
                    self.assertRegex(spv, r"= OpImageSampleExplicitLod %v4int_st %")
                    # A float format still samples the float view the runtime binds at the same index.
                    self.assertRegex(spv, r"= OpImageSampleExplicitLod %v4float %")

    def test_cube_samples_its_2d_array_view_with_three_coordinates(self):
        spv = translate("ps_samplecubetyped", sample("image_sample_lz", "CUBE"))
        self.assertRegex(spv, r"OpAccessChain %_ptr_UC_imga %images_arr ")
        self.assertRegex(spv, r"OpAccessChain %_ptr_UC_itx1_1 %itx1_1 ")
        self.assertIn("OpSampledImage %simg2da", spv)
        self.assertIn("OpCompositeConstruct %v3float", spv)

    def test_the_2d_form_is_unchanged(self):
        spv = translate("ps_sample2dtyped", sample("image_sample_lz", "2D"))
        self.assertRegex(spv, r"OpAccessChain %_ptr_UC_itx1_0 %itx1_0 ")
        self.assertNotIn("%itx1_1", spv)

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"),
                         "needs spirv-as and spirv-val on PATH")
    def test_modules_validate(self):
        for op in ("image_sample_l", "image_sample_lz"):
            for dim in DIMS:
                with self.subTest(op=op, dim=dim):
                    validate(translate("ps_sampletypedval", sample(op, dim)))


# dim -> the type of the gradients a pixel shader's image_sample takes: the coordinates an implicit LOD derives from.
GRAD_TYPES = {"1D": "%float", "2D": "%v2float", "2D_ARRAY": "%v2float", "3D": "%v3float", "CUBE": "%v2float"}


class ImplicitSampleTypedDims(unittest.TestCase):
    """image_sample (no LOD dword) in every dimension, in the pixel shader and in a compute shader."""

    def test_every_dimension_reads_both_integer_aliases(self):
        for stage in ("ps", "cs"):
            for dim, (_, _, _, di, binding) in DIMS.items():
                with self.subTest(stage=stage, dim=dim):
                    spv = translate(f"{stage}_sampleimpl", sample("image_sample", dim))
                    for cls in (1, 2):
                        self.assertRegex(spv, rf"OpAccessChain %_ptr_UC_itx{cls}_{di} %itx{cls}_{di} ")
                        self.assertIn(f"OpDecorate %itx{cls}_{di} Binding {binding}", spv)
                        self.assertIn(f"%itxsamp{cls}_{di} = OpTypeSampledImage %itximg{cls}_{di}", spv)
                    for ty in ("%v4uint_st", "%v4int_st", "%v4float"):
                        self.assertRegex(spv, rf"= OpImageSampleExplicitLod {ty} %")
                    self.assertNotIn("OpImageSampleImplicitLod", spv)

    def test_pixel_shader_samples_with_the_coordinate_gradients(self):
        for dim, ty in GRAD_TYPES.items():
            with self.subTest(dim=dim):
                spv = translate("ps_sampleimplgrad", sample("image_sample", dim))
                dx = re.findall(rf"(%t\d+) = OpDPdx {ty} (%t\d+)", spv)
                dy = re.findall(rf"(%t\d+) = OpDPdy {ty} (%t\d+)", spv)
                self.assertEqual(len(dx), 1, spv)
                self.assertEqual(len(dy), 1)
                self.assertEqual(dx[0][1], dy[0][1], "both gradients of the same coordinates")
                # All three views (float, uint, sint) take the same gradients.
                grads = re.findall(r"OpImageSampleExplicitLod %\w+ %t\d+ %t\d+ Grad (%t\d+) (%t\d+)", spv)
                self.assertEqual(sorted(set(grads)), [(dx[0][0], dy[0][0])])
                self.assertEqual(len(grads), 3)
                if dim in ("2D_ARRAY", "CUBE"):
                    # The layer has no derivative: the gradients cover the first two coordinates only.
                    self.assertRegex(spv, rf"{dx[0][1]} = OpCompositeConstruct %v2float %t\d+ %t\d+\n")

    def test_compute_shader_samples_at_lod_zero(self):
        for dim in DIMS:
            with self.subTest(dim=dim):
                spv = translate("cs_sampleimpl", sample("image_sample", dim))
                self.assertNotIn("OpDPdx", spv)
                lods = re.findall(r"OpImageSampleExplicitLod %\w+ %t\d+ %t\d+ (\w+ %\w+)", spv)
                self.assertEqual(lods, ["Lod %f0"] * 3)

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"),
                         "needs spirv-as and spirv-val on PATH")
    def test_modules_validate(self):
        for stage in ("ps", "cs"):
            for dim in DIMS:
                with self.subTest(stage=stage, dim=dim):
                    validate(translate(f"{stage}_sampleimplval", sample("image_sample", dim)))


if __name__ == "__main__":
    unittest.main()
