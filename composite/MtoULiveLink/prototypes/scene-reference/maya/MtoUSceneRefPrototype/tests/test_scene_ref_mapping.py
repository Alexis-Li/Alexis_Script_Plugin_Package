"""Pure-Python checks for the scene reference mapping.

Runs under plain CPython 3 and under mayapy; nothing here imports Maya. The
synthetic ASCII FBX fragments are the controls for the texture detector both
hosts share, so the test states what the rule counts and what it ignores.

    python -m unittest discover -s <this directory> -t <package root>
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
REPO = Path(__file__).resolve().parents[7]
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import mtou_scene_ref_mapping as mapping  # noqa: E402


def scratch_directory(prefix):
    """A disposable directory under the repository's scratch folder."""
    for candidate in (REPO.parent / ".tmp", REPO / ".tmp"):
        if candidate.is_dir() and os.access(str(candidate), os.W_OK):
            return Path(tempfile.mkdtemp(prefix=prefix, dir=str(candidate)))
    return Path(tempfile.mkdtemp(prefix=prefix))

#: An engine style ASCII FBX fragment: one texture record that names files
#: directly, one Maya style record whose name lines follow a nested property
#: block, a take clip, the scene's own file name property, a commented out
#: record, and the Definitions template that mentions texture objects.
ENGINE_FRAGMENT = '''; FBX 7.7.0 project file
FBXHeaderExtension:  {
\tSceneInfo: "SceneInfo::GlobalInfo", "UserData" {
\t\tProperties70:  {
\t\t\tP: "Original|FileName", "KString", "", "", "level.fbx"
\t\t}
\t}
}
Definitions:  {
\tObjectType: "Texture" {
\t\tCount: 1
\t\tPropertyTemplate: "FbxFileTexture" {
\t\t\tProperties70:  {
\t\t\t\tP: "FileName", "KString", "", "", ""
\t\t\t\tP: "RelativeFilename", "KString", "", "", ""
\t\t\t}
\t\t}
\t}
}
Objects:  {
\tTexture: "T_Wall_Brick", "" {
\t\tType: "TextureVideoClip"
\t\tFileName: "assets/textures/wall_brick.png"
\t\tRelativeFilename: "textures/wall_brick.png"
\t}
\tTexture: 2443664220208, "Texture::file1", "" {
\t\tType: "TextureVideoClip"
\t\tTextureName: "Texture::file1"
\t\tProperties70:  {
\t\t\tP: "CurrentTextureBlendMode", "enum", "", "", 0
\t\t}
\t\tFileName: "handoff/assets/door.png"
\t\tRelativeFilename: "assets/door.png"
\t}
\tVideo: 12, "Video::Take_001", "Clip" {
\t\tType: "Clip"
\t\tProperties70:  {
\t\t\tP: "Path", "KString", "Url", "", ""
\t\t}
\t\tFileName: "Take_001.tak"
\t}
\t; Texture: "T_Commented_Out", "" {
\tModel: 77, "Model::SM_Wall", "Mesh" {
\t}
}
'''

CLEAN_FRAGMENT = '''; FBX 7.7.0 project file
Objects:  {
\tModel: 77, "Model::SM_Wall", "Mesh" {
\t}
\tMaterial: 88, "Material::lambert1", "" {
\t}
}
Connections:  {
\tC: "OO",77,0
}
'''


def close(test, first, second, places=9, message=""):
    test.assertAlmostEqual(first, second, places=places, msg=message)


def candidate_matrix(name):
    """The matrix of the named axis candidate, so tests never rely on order."""
    for candidate in mapping.AXIS_CANDIDATES:
        if candidate["name"] == name:
            return mapping.matrix3(candidate["matrix"])
    raise KeyError(name)


CAMERA = mapping.CAMERA_SYNC_CANDIDATE
ENGINE = mapping.ENGINE_HANDOFF_CANDIDATE
CLASSIC = "maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y"
MIRROR = "maya_x=-ue_y, maya_y=ue_z, maya_z=ue_x"
IDENTITY = "identity"


class AxisCandidatesTest(unittest.TestCase):

    def test_the_engine_handoff_map_is_a_candidate(self):
        entry = [candidate for candidate in mapping.AXIS_CANDIDATES
                 if candidate["name"] == mapping.ENGINE_HANDOFF_CANDIDATE][0]
        matrix = mapping.matrix3(entry["matrix"])
        self.assertEqual(mapping.matrix3_apply(matrix, (1, 0, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(mapping.matrix3_apply(matrix, (0, 1, 0)), (0.0, 0.0, 1.0))
        self.assertEqual(mapping.matrix3_apply(matrix, (0, 0, 1)), (0.0, 1.0, 0.0))
        close(self, mapping.matrix3_determinant(matrix), -1.0)
        self.assertIn("2026-09-28", entry["note"])
        self.assertIn("determinant -1", entry["note"])

    def test_candidate_matrices_are_the_documented_maps(self):
        camera = candidate_matrix(mapping.CAMERA_SYNC_CANDIDATE)
        self.assertEqual(mapping.matrix3_apply(camera, (1, 0, 0)), (0.0, 0.0, -1.0))
        self.assertEqual(mapping.matrix3_apply(camera, (0, 1, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(mapping.matrix3_apply(camera, (0, 0, 1)), (0.0, 1.0, 0.0))
        upward = candidate_matrix(CLASSIC)
        self.assertEqual(mapping.matrix3_apply(upward, (0, 0, 1)), (0.0, 1.0, 0.0))
        self.assertEqual(mapping.matrix3_apply(upward, (0, 1, 0)), (0.0, 0.0, -1.0))
        engine = candidate_matrix(ENGINE)
        self.assertEqual(mapping.matrix3_apply(engine, (1, 0, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(mapping.matrix3_apply(engine, (0, 0, 1)), (0.0, 1.0, 0.0))

    def test_handedness_of_the_candidates(self):
        determinants = {candidate["name"]: mapping.matrix3_determinant(
            mapping.matrix3(candidate["matrix"])) for candidate in mapping.AXIS_CANDIDATES}
        close(self, determinants[CAMERA], -1.0)
        close(self, determinants[ENGINE], -1.0)
        close(self, determinants[MIRROR], -1.0)
        close(self, determinants[CLASSIC], 1.0)
        close(self, determinants[IDENTITY], 1.0)

    def test_the_handoff_and_the_camera_route_disagree(self):
        engine = candidate_matrix(ENGINE)
        camera = candidate_matrix(CAMERA)
        self.assertNotEqual(mapping.matrix3_values(engine),
                            mapping.matrix3_values(camera))
        for point in ((100.0, 200.0, 300.0), (-450.0, 25.0, 900.0)):
            self.assertNotEqual(mapping.matrix3_apply(engine, point),
                                mapping.matrix3_apply(camera, point))

    def test_every_candidate_is_a_signed_permutation(self):
        for candidate in mapping.AXIS_CANDIDATES:
            report = mapping.signed_permutation(candidate["matrix"])
            self.assertTrue(report["signed_permutation"], candidate["name"])
            self.assertEqual(sorted(report["permutation"]), [0, 1, 2])
            close(self, report["max_permutation_deviation"], 0.0)


class MatrixTest(unittest.TestCase):

    def test_determinant_and_axis_rows(self):
        matrix = mapping.matrix3((0, 2, 0, 0, 0, 3, -1, 0, 0))
        close(self, mapping.matrix3_determinant(matrix), -6.0)
        # The rows of a candidate are the three Unreal axes in Maya terms.
        self.assertEqual(mapping.matrix4_linear(
            mapping.matrix4_from_linear_and_translation(matrix, (0.0, 0.0, 0.0))), matrix)
        self.assertEqual(mapping.matrix3_transpose(matrix),
                         ((0.0, 0.0, -1.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0)))

    def test_multiply_and_apply_agree(self):
        first = mapping.matrix3((1, 2, 3, 4, 5, 6, 7, 8, 10))
        second = mapping.matrix3((0, 1, 0, 1, 0, 0, 0, 0, 1))
        product = mapping.matrix3_multiply(first, second)
        vector = (1.0, -2.0, 3.5)
        self.assertEqual(mapping.matrix3_apply(product, vector),
                         mapping.matrix3_apply(first, mapping.matrix3_apply(second, vector)))

    def test_inverse_round_trip(self):
        matrix = mapping.matrix3((2.0, 0.5, 1.0, -1.0, 3.0, 0.25, 0.0, 2.0, -4.0))
        inverse = mapping.matrix3_inverse(matrix)
        product = mapping.matrix3_multiply(matrix, inverse)
        for row in range(3):
            for column in range(3):
                close(self, product[row][column], 1.0 if row == column else 0.0, places=9)

    def test_singular_matrix_has_no_inverse(self):
        self.assertIsNone(mapping.matrix3_inverse(mapping.matrix3((1, 2, 3, 2, 4, 6, 7, 8, 9))))
        self.assertIsNone(mapping.matrix3_inverse(mapping.matrix3((0, 0, 0, 0, 0, 0, 0, 0, 0))))

    def test_matrix3_rejects_wrong_value_counts(self):
        with self.assertRaises(ValueError):
            mapping.matrix3((1, 2, 3))

    def test_matrix4_helpers_read_the_hosts_layout(self):
        # Rows are the axis images and the fourth row is the translation, which
        # is what both Maya's xform -m and Unreal's FMatrix write.
        values = [1.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 1.0, 0.0,
                  0.0, -1.0, 0.0, 0.0,
                  10.0, 20.0, 30.0, 1.0]
        self.assertEqual(mapping.matrix4_translation(values), (10.0, 20.0, 30.0))
        linear = mapping.matrix4_linear(values)
        self.assertEqual(mapping.matrix4_linear(values)[1], (0.0, 0.0, 1.0))
        rebuilt = mapping.matrix4_from_linear_and_translation(linear, (10.0, 20.0, 30.0))
        self.assertEqual(rebuilt, values)

    def test_a_carried_world_matrix_maps_axes_and_position(self):
        camera = candidate_matrix(CAMERA)
        unreal = mapping.matrix4_from_linear_and_translation(
            mapping.matrix3((1, 0, 0, 0, 1, 0, 0, 0, 1)), (300.0, 100.0, 200.0))
        maya = mapping.carry_world_matrix(camera, unreal)
        self.assertEqual(mapping.matrix4_translation(maya), (100.0, 200.0, -300.0))
        axes = mapping.matrix4_linear(maya)
        self.assertEqual(axes[0], (0.0, 0.0, -1.0))
        self.assertEqual(axes[1], (1.0, 0.0, 0.0))
        self.assertEqual(axes[2], (0.0, 1.0, 0.0))

    def test_carrying_a_world_matrix_round_trips(self):
        for candidate in mapping.AXIS_CANDIDATES:
            matrix = mapping.matrix3(candidate["matrix"])
            unreal = mapping.matrix4_from_linear_and_translation(
                mapping.matrix3((0.5 ** 0.5, -0.5 ** 0.5, 0.0,
                                 0.5 ** 0.5, 0.5 ** 0.5, 0.0,
                                 0.0, 0.0, 1.0)), (-70.0, 25.0, 400.0))
            maya = mapping.carry_world_matrix(matrix, unreal)
            back = mapping.uncarry_world_matrix(matrix, maya)
            for expected, actual in zip(unreal, back):
                close(self, expected, actual, places=9, message=candidate["name"])

    def test_uncarry_refuses_a_singular_map(self):
        with self.assertRaises(ValueError):
            mapping.uncarry_world_matrix((0, 0, 0, 0, 0, 0, 0, 0, 0),
                                         [1.0, 0.0, 0.0, 0.0,
                                          0.0, 1.0, 0.0, 0.0,
                                          0.0, 0.0, 1.0, 0.0,
                                          0.0, 0.0, 0.0, 1.0])

    def test_angle_between_degrees(self):
        close(self, mapping.angle_between_degrees((1, 0, 0), (1, 0, 0)), 0.0)
        close(self, mapping.angle_between_degrees((1, 0, 0), (0, 1, 0)), 90.0)
        close(self, mapping.angle_between_degrees((1, 0, 0), (-3, 0, 0)), 180.0)
        self.assertIsNone(mapping.angle_between_degrees((0, 0, 0), (1, 0, 0)))

    def test_expected_axis_sizes_permutes(self):
        camera = candidate_matrix(CAMERA)
        # ue (x, y, z) -> maya (y, z, -x), so the Maya size is (size_y, size_z, size_x).
        self.assertEqual(mapping.expected_axis_sizes(camera, (10.0, 20.0, 30.0)),
                         [20.0, 30.0, 10.0])


class SignedPermutationTest(unittest.TestCase):

    def test_identity_is_a_signed_permutation(self):
        report = mapping.signed_permutation((1, 0, 0, 0, 1, 0, 0, 0, 1))
        self.assertTrue(report["signed_permutation"])
        self.assertEqual(report["permutation"], [0, 1, 2])
        self.assertEqual(report["signs"], [1, 1, 1])

    def test_scale_is_not_a_signed_permutation(self):
        report = mapping.signed_permutation((2, 0, 0, 0, 1, 0, 0, 0, 1))
        self.assertFalse(report["signed_permutation"])
        close(self, report["max_permutation_deviation"], 1.0)

    def test_rotation_by_forty_five_degrees_is_not_a_signed_permutation(self):
        half = 2.0 ** -0.5
        report = mapping.signed_permutation((half, -half, 0, half, half, 0, 0, 0, 1))
        self.assertFalse(report["signed_permutation"])
        self.assertGreater(report["max_permutation_deviation"], 0.2)


class FitTest(unittest.TestCase):

    def _pairs(self, name=CAMERA):
        matrix = candidate_matrix(name)
        unreal = [(100.0, 200.0, 300.0), (-400.0, 50.0, 900.0), (0.0, 0.0, 0.0),
                  (700.0, -20.0, 10.0)]
        return unreal, [mapping.matrix3_apply(matrix, point) for point in unreal], matrix

    def test_fit_recovers_the_candidate(self):
        unreal, maya, matrix = self._pairs()
        fit = mapping.fit_linear_map(unreal, maya)
        self.assertTrue(fit["available"], fit["reason"])
        self.assertTrue(fit["signed_permutation"])
        close(self, fit["determinant"], -1.0)
        self.assertLess(fit["max_position_error_cm"], 1e-6)
        for expected, actual in zip(mapping.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=9)

    def test_fit_recovers_the_classic_map(self):
        unreal, maya, matrix = self._pairs(CLASSIC)
        fit = mapping.fit_linear_map(unreal, maya)
        close(self, fit["determinant"], 1.0)
        self.assertEqual(fit["permutation"], [0, 2, 1])
        self.assertEqual(fit["signs"], [1, 1, -1])

    def test_three_points_do_not_determine_a_map(self):
        unreal, maya, _ = self._pairs()
        fit = mapping.fit_linear_map(unreal[:3], maya[:3])
        self.assertFalse(fit["available"])
        self.assertIn("plane", fit["reason"])

    def test_two_points_do_not_determine_a_map(self):
        fit = mapping.fit_linear_map([(1.0, 2.0, 3.0), (4.0, 5.0, 6.0)],
                                     [(1.0, 2.0, 3.0), (4.0, 5.0, 6.0)])
        self.assertFalse(fit["available"])
        self.assertIn("three matched objects", fit["reason"])

    def test_the_fitted_matrix_ignores_a_common_translation(self):
        # The fit centres both point sets, so moving the Unreal points without
        # moving Maya leaves the map intact and shows the shift as a constant
        # residual: that is what a handoff that lost a placement looks like.
        unreal, maya, matrix = self._pairs()
        offset = (500.0, -250.0, 75.0)
        moved = [tuple(point[axis] + offset[axis] for axis in range(3))
                 for point in unreal]
        fit = mapping.fit_linear_map(moved, maya)
        self.assertTrue(fit["available"])
        for expected, actual in zip(mapping.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=6)
        expected_error = mapping.vector_length(mapping.matrix3_apply(matrix, offset))
        close(self, fit["max_position_error_cm"], expected_error, places=3)
        close(self, fit["rms_position_error_cm"], expected_error, places=3)

    def test_a_moved_level_still_fits(self):
        # The same world offset on both sides is an exact relation: the fitted
        # map is recovered and the residual stays at machine precision.
        unreal, maya, matrix = self._pairs()
        offset = (500.0, -250.0, 75.0)
        moved_unreal = [tuple(point[axis] + offset[axis] for axis in range(3))
                        for point in unreal]
        moved_maya = [mapping.matrix3_apply(matrix, point) for point in moved_unreal]
        fit = mapping.fit_linear_map(moved_unreal, moved_maya)
        self.assertTrue(fit["available"])
        self.assertLess(fit["max_position_error_cm"], 1e-6)
        for expected, actual in zip(mapping.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=6)

    def test_a_wrong_map_is_reported_by_its_residual(self):
        unreal, maya, _ = self._pairs()
        broken = [(x, y, z + 25.0) for x, y, z in maya]
        fit = mapping.fit_linear_map(unreal, broken)
        self.assertTrue(fit["available"])
        close(self, fit["max_position_error_cm"], 25.0, places=6)


class OrientationMeasurabilityTest(unittest.TestCase):

    def test_distinct_extents_are_measurable(self):
        self.assertTrue(mapping.orientation_measurable([10.0, 20.0, 30.0]))
        self.assertTrue(mapping.orientation_measurable([10.0, 10.5, 30.0]))

    def test_symmetric_meshes_are_not_measurable(self):
        self.assertFalse(mapping.orientation_measurable([20.0, 20.0, 20.0]))
        self.assertFalse(mapping.orientation_measurable([20.0, 20.0, 30.0]))
        self.assertFalse(mapping.orientation_measurable([0.0, 0.0, 0.0]))

    def test_missing_size_is_not_measurable(self):
        self.assertFalse(mapping.orientation_measurable(None))
        self.assertFalse(mapping.orientation_measurable([1.0, 2.0]))


class TextureScanTextTest(unittest.TestCase):

    def test_the_engine_fragment_control(self):
        scan = mapping.scan_fbx_texture_records(ENGINE_FRAGMENT)
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["texture_references"], 1)
        self.assertEqual(scan["video_references"], 1)
        self.assertEqual(scan["files"], ["assets/textures/wall_brick.png",
                                         "textures/wall_brick.png"])
        self.assertEqual(scan["texture_files"], ["assets/textures/wall_brick.png",
                                                 "textures/wall_brick.png"])
        self.assertEqual(scan["video_files"], [])
        self.assertEqual(scan["image_files"], ["assets/textures/wall_brick.png",
                                               "textures/wall_brick.png"])
        self.assertEqual(len(scan["records"]), 2)
        self.assertEqual(scan["records"][0]["line"], 21)
        self.assertEqual(scan["records"][0]["file_names"],
                         ["assets/textures/wall_brick.png", "textures/wall_brick.png"])
        self.assertEqual(scan["records"][1]["file_names"], [])

    def test_a_nested_block_ends_the_record_under_the_rule(self):
        scan = mapping.scan_fbx_texture_records(ENGINE_FRAGMENT)
        # The Maya style record names its files after Properties70, which the
        # contract's line rule treats as the end of the record, so it is a
        # texture record with no name -- and the importer still refuses it.
        self.assertEqual(scan["records"][1]["header"],
                         'Texture: 2443664220208, "Texture::file1", "" {')
        self.assertEqual(scan["records"][1]["file_names"], [])
        self.assertNotIn("handoff/assets/door.png", scan["files"])

    def test_the_take_clip_is_not_a_texture(self):
        scan = mapping.scan_fbx_texture_records(ENGINE_FRAGMENT)
        self.assertNotIn("Take_001.tak", scan["files"])
        self.assertEqual(scan["video_records"][0]["header"],
                         'Video: 12, "Video::Take_001", "Clip" {')

    def test_the_definitions_template_and_comments_are_ignored(self):
        scan = mapping.scan_fbx_texture_records(ENGINE_FRAGMENT)
        headers = [record["header"] for record in scan["records"]]
        self.assertEqual(len(headers), 2)
        for header in headers:
            self.assertTrue(header.startswith("Texture:"), header)
        self.assertNotIn("T_Commented_Out", " ".join(headers))

    def test_a_clean_fragment_records_nothing(self):
        scan = mapping.scan_fbx_texture_records(CLEAN_FRAGMENT)
        self.assertEqual(scan["texture_records"], 0)
        self.assertEqual(scan["texture_references"], 0)
        self.assertEqual(scan["video_references"], 0)
        self.assertEqual(scan["files"], [])

    def test_only_the_last_quoted_token_names_the_file(self):
        text = ('Objects:  {\n'
                '\tTexture: "T_A", "" {\n'
                '\t\tRelativeFilename: "ignored.png", "kept/last_token.jpg"\n'
                '\t}\n'
                '}\n')
        scan = mapping.scan_fbx_texture_records(text)
        self.assertEqual(scan["files"], ["kept/last_token.jpg"])

    def test_a_record_with_empty_names_is_still_a_record(self):
        text = 'Objects:  {\n\tTexture: "T_Empty", "" {\n\t\tFileName: ""\n\t}\n}\n'
        scan = mapping.scan_fbx_texture_records(text)
        self.assertEqual(scan["texture_records"], 1)
        self.assertEqual(scan["texture_references"], 0)
        self.assertEqual(scan["files"], [])

    def test_file_names_are_distinct_and_ordered(self):
        text = ('Objects:  {\n'
                '\tTexture: "T_A", "" {\n'
                '\t\tFileName: "b.png"\n'
                '\t\tRelativeFilename: "a.png"\n'
                '\t}\n'
                '\tTexture: "T_B", "" {\n'
                '\t\tFileName: "b.png"\n'
                '\t}\n'
                '}\n')
        scan = mapping.scan_fbx_texture_records(text)
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["texture_references"], 2)
        self.assertEqual(scan["files"], ["b.png", "a.png"])

    def test_closing_brace_ends_a_record(self):
        text = ('Objects:  {\n'
                '\tTexture: "T_A", "" {\n'
                '\t}\n'
                '\tFileName: "outside.png"\n'
                '}\n')
        scan = mapping.scan_fbx_texture_records(text)
        self.assertEqual(scan["texture_records"], 1)
        self.assertEqual(scan["files"], [])


class ImageFileNameTest(unittest.TestCase):

    def test_image_suffixes(self):
        for name in ("a.png", "a.PNG", "b.bmp", "c.tga", "d.jpg", "e.jpeg", "f.exr",
                     "g.hdr", "h.dds", "i.fbm/tile.tga", "textures\\wall.tga"):
            self.assertTrue(mapping.is_image_file_name(name), name)

    def test_non_image_names(self):
        for name in ("", None, "T_Wall", "wall.uasset", "Take_001.tak", "mesh.fbx"):
            self.assertFalse(mapping.is_image_file_name(name), str(name))


class FbxFileScanTest(unittest.TestCase):

    def setUp(self):
        self.directory = scratch_directory("mtou_scene_ref_scan_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def _write(self, name, text, encoding="utf-8"):
        path = self.directory / name
        path.write_text(text, encoding=encoding)
        return path

    def test_an_ascii_handoff_is_scanned(self):
        path = self._write("handoff.fbx", ENGINE_FRAGMENT)
        scan = mapping.scan_fbx_file(str(path))
        self.assertTrue(scan["exists"] and scan["readable"])
        self.assertEqual(scan["format"], "ascii")
        self.assertFalse(scan["binary"])
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["files"], ["assets/textures/wall_brick.png",
                                         "textures/wall_brick.png"])
        self.assertGreater(scan["bytes"], 0)

    def test_a_clean_handoff_reports_nothing(self):
        path = self._write("clean.fbx", CLEAN_FRAGMENT)
        scan = mapping.scan_fbx_file(str(path))
        self.assertEqual(scan["texture_records"], 0)
        self.assertEqual(scan["texture_references"], 0)
        self.assertEqual(scan["files"], [])

    def test_a_binary_magic_file_is_not_read_as_text(self):
        path = self.directory / "binary.fbx"
        path.write_bytes(mapping.FBX_BINARY_MAGIC + b"\x00\x1a\x00\x14" + b"\x00" * 64)
        scan = mapping.scan_fbx_file(str(path))
        self.assertEqual(scan["format"], "binary")
        self.assertTrue(scan["binary"])
        self.assertIn("heuristic", scan["note"])

    def test_a_non_fbx_file_is_reported_as_unknown(self):
        path = self._write("notes.txt", "this is not an fbx file\n")
        scan = mapping.scan_fbx_file(str(path))
        self.assertEqual(scan["format"], "unknown")
        self.assertIn("not an FBX file", scan["note"])

    def test_a_missing_file_is_reported(self):
        scan = mapping.scan_fbx_file(str(self.directory / "missing.fbx"))
        self.assertFalse(scan["exists"])
        self.assertFalse(scan["readable"])
        self.assertTrue(scan["error"])

    def test_sniffing_reads_the_header_not_the_extension(self):
        path = self._write("handoff.txt", ENGINE_FRAGMENT)
        sniffed = mapping.sniff_fbx_file(str(path))
        self.assertEqual(sniffed["format"], "ascii")
        self.assertTrue(sniffed["magic"].startswith("; FBX"))


class ManifestTest(unittest.TestCase):

    def setUp(self):
        self.directory = scratch_directory("mtou_scene_ref_manifest_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def _write(self, name, payload):
        path = self.directory / name
        if isinstance(payload, str):
            path.write_text(payload, encoding="utf-8")
        else:
            path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return path

    def _manifest(self):
        return {
            "schema": "mtou-scene-ref-manifest/1",
            "generated_utc": "2026-09-28T12:00:00Z",
            "engine_version": "5.7.4",
            "world": {"package": "/Game/Map", "name": "Map", "up_axis": "Z",
                      "linear_unit": "cm", "world_partition": False},
            "scope": {"kind": "level_range", "persistent_level": "/Game/Map",
                      "requested_sublevels": ["/Game/Sub"],
                      "loaded_sublevels": ["/Game/Sub"],
                      "unloaded_sublevels": [{"package": "/Game/Other",
                                              "streaming_state": "not_in_world",
                                              "visible": False}],
                      "excluded_sublevels": ["/Game/Third"]},
            "scale": {"actors": 2, "components": 2, "objects": 2, "triangles": 24,
                      "vertices": 16},
            "objects": [
                {"id": "static_mesh_actor:SM_A:component:StaticMeshComponent0:instance:none",
                 "node_name": "SM_A", "actor": "SM_A", "actor_class": "StaticMeshActor",
                 "exported": True, "world_matrix": [1.0, 0.0, 0.0, 0.0,
                                                    0.0, 1.0, 0.0, 0.0,
                                                    0.0, 0.0, 1.0, 0.0,
                                                    10.0, 20.0, 30.0, 1.0],
                 "world_bounds_cm": {"min": {"x": 0.0, "y": 0.0, "z": 0.0},
                                     "max": {"x": 10.0, "y": 20.0, "z": 30.0},
                                     "size": {"x": 10.0, "y": 20.0, "z": 30.0}}},
                {"id": "static_mesh_actor:SM_B:component:StaticMeshComponent0:instance:none",
                 "node_name": "SM_B", "exported": False, "world_matrix": [0.0] * 16,
                 "world_bounds_cm": {"size": {"x": 1.0, "y": 1.0, "z": 1.0}}},
            ],
            "unsupported": [{"actor": "LI_House", "class": "LevelInstance",
                             "reason": "level instance is not baked"}],
            "skipped": [{"actor": "SkyLight", "class": "SkyLight",
                         "reason": "not_static_geometry"}],
            "output": {"directory": "handoff", "geometry_file": "Map.fbx",
                       "geometry_bytes": 2048, "files": [{"name": "Map.fbx",
                                                          "bytes": 2048}],
                       "image_files": [], "texture_records": 0,
                       "texture_references": 0, "video_references": 1,
                       "texture_reference_files": [],
                       "texture_reference_files_present": [],
                       "node_names": ["SM_A"]},
            "timing": {"scope_seconds": 0.1, "export_seconds": 1.2,
                       "inspect_seconds": 0.2},
            "memory": {"used_physical_mb": 1234.5},
            "warnings": [],
        }

    def test_a_valid_manifest_is_read(self):
        path = self._write("good.json", self._manifest())
        manifest = mapping.load_manifest(str(path))
        self.assertEqual(manifest["schema"], mapping.MANIFEST_SCHEMA)

    def test_a_missing_manifest_is_refused(self):
        with self.assertRaises(mapping.ManifestError) as caught:
            mapping.load_manifest(str(self.directory / "missing.json"))
        self.assertEqual(caught.exception.category, "MANIFEST_UNREADABLE")

    def test_invalid_json_is_refused(self):
        path = self._write("broken.json", "{not json")
        with self.assertRaises(mapping.ManifestError) as caught:
            mapping.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_INVALID_JSON")

    def test_a_json_array_is_refused(self):
        path = self._write("array.json", [1, 2, 3])
        with self.assertRaises(mapping.ManifestError) as caught:
            mapping.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_INVALID_JSON")

    def test_another_schema_is_refused(self):
        manifest = self._manifest()
        manifest["schema"] = "mtou-scene-ref-manifest/2"
        path = self._write("schema.json", manifest)
        with self.assertRaises(mapping.ManifestError) as caught:
            mapping.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_SCHEMA_MISMATCH")
        self.assertIn("mtou-scene-ref-manifest/1", caught.exception.detail)

    def test_a_missing_objects_list_is_refused(self):
        manifest = self._manifest()
        del manifest["objects"]
        path = self._write("objects.json", manifest)
        with self.assertRaises(mapping.ManifestError) as caught:
            mapping.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_MISSING_OBJECTS")

    def test_only_objects_exported_true_are_required(self):
        manifest = self._manifest()
        exported = mapping.exported_objects(manifest)
        self.assertEqual([entry["node_name"] for entry in exported], ["SM_A"])
        manifest["objects"][1]["exported"] = "yes"
        self.assertEqual(len(mapping.exported_objects(manifest)), 1)

    def test_the_summary_reports_the_scope_and_the_scale(self):
        summary = mapping.manifest_summary(self._manifest())
        self.assertEqual(summary["engine_version"], "5.7.4")
        self.assertEqual(summary["world"]["up_axis"], "Z")
        self.assertEqual(summary["world"]["linear_unit"], "cm")
        self.assertEqual(summary["scope"]["persistent_level"], "/Game/Map")
        self.assertEqual(summary["scope"]["requested_sublevels"], ["/Game/Sub"])
        self.assertEqual(summary["scope"]["unloaded_sublevels"][0]["streaming_state"],
                         "not_in_world")
        self.assertEqual(summary["scope"]["excluded_sublevels"], ["/Game/Third"])
        self.assertEqual(summary["scale"]["triangles"], 24)
        self.assertEqual(summary["objects_total"], 2)
        self.assertEqual(summary["objects_exported"], 1)
        self.assertEqual(summary["unsupported"][0]["class"], "LevelInstance")
        self.assertEqual(summary["output"]["geometry_file"], "Map.fbx")

    def test_requirement_reads_the_named_fields(self):
        manifest = self._manifest()
        name, matrix, size, offset, centroid, problems = \
            mapping.manifest_object_requirement(manifest["objects"][0])
        self.assertEqual(name, "SM_A")
        self.assertEqual(len(matrix), 16)
        self.assertEqual(matrix[12], 10.0)
        self.assertEqual(size, [10.0, 20.0, 30.0])
        # The fixture carries min (0, 0, 0), max (10, 20, 30) and no location, so
        # the offset is the box centre minus the world matrix's translation.
        self.assertEqual(offset, [-5.0, -10.0, -15.0])
        self.assertIsNone(centroid)
        self.assertEqual(problems, [])

    def test_requirement_prefers_the_declared_location_for_the_offset(self):
        entry = {
            "node_name": "SM_A",
            "world_matrix": [1.0, 0.0, 0.0, 0.0,
                             0.0, 1.0, 0.0, 0.0,
                             0.0, 0.0, 1.0, 0.0,
                             100.0, 200.0, 300.0, 1.0],
            "world_location_cm": {"x": 40.0, "y": 50.0, "z": 60.0},
            "world_bounds_cm": {"min": {"x": 40.0, "y": 50.0, "z": 60.0},
                                "max": {"x": 140.0, "y": 250.0, "z": 260.0},
                                "size": {"x": 100.0, "y": 200.0, "z": 200.0}},
        }
        _, _, size, offset, centroid, problems = \
            mapping.manifest_object_requirement(entry)
        self.assertEqual(size, [100.0, 200.0, 200.0])
        self.assertEqual(offset, [50.0, 100.0, 100.0])
        self.assertIsNone(centroid)
        self.assertEqual(problems, [])

    def test_requirement_reads_the_world_surface_centroid(self):
        entry = {
            "node_name": "SM_Cone",
            "world_location_cm": {"x": 0.0, "y": 0.0, "z": 0.0},
            "world_matrix": [1.0, 0.0, 0.0, 0.0,
                             0.0, 1.0, 0.0, 0.0,
                             0.0, 0.0, 1.0, 0.0,
                             0.0, 0.0, 0.0, 1.0],
            "world_bounds_cm": {"min": {"x": -50.0, "y": -50.0, "z": 0.0},
                                "max": {"x": 50.0, "y": 50.0, "z": 100.0},
                                "size": {"x": 100.0, "y": 100.0, "z": 100.0}},
            "world_surface_centroid_cm": {"x": 1.0, "y": -2.0, "z": 25.0},
        }
        _, _, _, offset, centroid, problems = \
            mapping.manifest_object_requirement(entry)
        self.assertEqual(centroid, [1.0, -2.0, 25.0])
        self.assertEqual(offset, [0.0, 0.0, 50.0])
        self.assertEqual(problems, [])

    def test_a_manifest_without_a_centroid_is_not_a_problem(self):
        manifest = self._manifest()
        name, _, _, _, centroid, problems = mapping.manifest_object_requirement(
            manifest["objects"][0])
        self.assertEqual(name, "SM_A")
        self.assertIsNone(centroid)
        self.assertEqual(problems, [])

    def test_requirement_reports_missing_fields(self):
        name, matrix, size, offset, centroid, problems = \
            mapping.manifest_object_requirement({"id": "x", "exported": True})
        self.assertEqual(name, "")
        self.assertIsNone(matrix)
        self.assertIsNone(size)
        self.assertIsNone(offset)
        self.assertIsNone(centroid)
        self.assertEqual(len(problems), 4)
        self.assertTrue(any("node_name" in problem for problem in problems))
        self.assertTrue(any("world_matrix" in problem for problem in problems))
        self.assertTrue(any("world_bounds_cm.size" in problem for problem in problems))
        self.assertTrue(any("world_bounds_cm" in problem for problem in problems))

    def test_requirement_reports_a_short_matrix(self):
        _, matrix, _, offset, centroid, problems = \
            mapping.manifest_object_requirement(
                {"node_name": "SM_C", "world_matrix": [1.0, 2.0, 3.0],
                 "world_bounds_cm": {"size": {"x": 1.0, "y": 1.0, "z": 1.0}}})
        self.assertIsNone(matrix)
        self.assertIsNone(offset)
        self.assertIsNone(centroid)
        self.assertEqual(len(problems), 2)
        self.assertTrue(any("world_matrix" in problem for problem in problems))
        self.assertTrue(any("min/max" in problem for problem in problems))


class ComparisonTest(unittest.TestCase):
    """The comparison half: what a good, a wrong and a missing object report."""

    def _camera_candidate(self, name=CAMERA):
        return candidate_matrix(name)

    def _measurement(self, name, maya_matrix, maya_size, local_size, unreal_matrix=None,
                     unreal_size=None, entry_id=None, maya_offset=None,
                     maya_centroid=None):
        matrix = self._camera_candidate()
        inverse = mapping.matrix3_inverse(matrix)
        if unreal_matrix is None:
            unreal_matrix = mapping.uncarry_world_matrix(matrix, maya_matrix)
        if unreal_size is None:
            unreal_size = mapping.expected_axis_sizes(inverse, maya_size)
        if maya_offset is None:
            maya_offset = (0.0, 0.0, 0.0)
        unreal_offset = mapping.matrix3_apply(inverse, maya_offset)
        position = mapping.matrix4_translation(maya_matrix)
        if maya_centroid is None:
            maya_centroid = position
        unreal_centroid = mapping.matrix3_apply(inverse, maya_centroid)
        return {
            "node_name": name,
            "matched_id": entry_id or "id:{0}".format(name),
            "found": True,
            "path": "|MtoU_UE_SceneRef:MtoU_UE_SceneRef|MtoU_UE_SceneRef:{0}".format(name),
            "matched_by": "name",
            "ambiguous_matches": [],
            "problems": [],
            "ue_matrix": unreal_matrix,
            "ue_size": unreal_size,
            "ue_offset": unreal_offset,
            "ue_centroid": unreal_centroid,
            "maya_centroid": list(maya_centroid),
            "maya_matrix": maya_matrix,
            "maya_size": maya_size,
            "maya_offset": list(maya_offset),
            "local_size": local_size,
        }

    def _scene_for(self, map_name):
        """The four reference objects as a handoff in ``map_name`` would present them."""
        return self._scene_for_values(candidate_matrix(map_name))

    def _scene_for_values(self, map_to_use):
        """The four reference objects, carried through one map into Unreal terms."""
        scene = []
        for entry in self._scene():
            entry = dict(entry)
            entry["ue_matrix"] = mapping.uncarry_world_matrix(
                map_to_use, entry["maya_matrix"])
            inverse = mapping.matrix3_inverse(map_to_use)
            entry["ue_size"] = mapping.expected_axis_sizes(inverse, entry["maya_size"])
            entry["ue_offset"] = mapping.matrix3_apply(inverse, entry["maya_offset"])
            entry["ue_centroid"] = mapping.matrix3_apply(inverse, entry["maya_centroid"])
            scene.append(entry)
        return scene

    def _scene(self):
        """Four placed objects, one of them a symmetric cube."""
        return [
            self._measurement("SM_Pillar",
                              [1.0, 0.0, 0.0, 0.0,
                               0.0, 1.0, 0.0, 0.0,
                               0.0, 0.0, 1.0, 0.0,
                               100.0, 200.0, -300.0, 1.0],
                              [100.0, 200.0, 50.0], [100.0, 200.0, 50.0],
                              maya_offset=(0.0, 0.0, 25.0)),
            self._measurement("SM_Rock",
                              [0.7071067811865476, 0.0, -0.7071067811865476, 0.0,
                               0.0, 1.0, 0.0, 0.0,
                               0.7071067811865476, 0.0, 0.7071067811865476, 0.0,
                               -450.0, 25.0, 900.0, 1.0],
                              [50.0, 50.0, 50.0], [50.0, 50.0, 50.0],
                              # A box centred on its pivot with an asymmetric
                              # mesh: the offset is zero and only the vertex
                              # centroid can see a mirror.
                              maya_centroid=(-450.0, 25.0, 880.0)),
            self._measurement("SM_Wall",
                              [0.0, 1.0, 0.0, 0.0,
                               -1.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 1.0, 0.0,
                               1000.0, 0.0, 10.0, 1.0],
                              [400.0, 20.0, 300.0], [400.0, 20.0, 300.0]),
            self._measurement("SM_Box",
                              [1.0, 0.0, 0.0, 0.0,
                               0.0, 1.0, 0.0, 0.0,
                               0.0, 0.0, 1.0, 0.0,
                               -800.0, 1000.0, -1200.0, 1.0],
                              [300.0, 100.0, 60.0], [300.0, 100.0, 60.0],
                              maya_offset=(-150.0, 0.0, 30.0)),
            # A mesh whose local extents are all 100 but whose box is off the
            # pivot: the orientation check skips it, the offset check must not.
            self._measurement("SM_Wedge",
                              [1.0, 0.0, 0.0, 0.0,
                               0.0, 1.0, 0.0, 0.0,
                               0.0, 0.0, 1.0, 0.0,
                               700.0, -250.0, 90.0, 1.0],
                              [100.0, 100.0, 100.0], [100.0, 100.0, 100.0],
                              maya_offset=(-50.0, -50.0, -50.0)),
        ]

    def test_a_correct_scene_matches_the_camera_candidate(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        check = result["transform_check"]
        self.assertEqual(check["candidate"], CAMERA)
        self.assertEqual(check["best"], CAMERA)
        self.assertEqual(check["decided_by"], "candidate")
        self.assertEqual(check["winner"]["name"], CAMERA)
        self.assertEqual(check["winner"]["source"], "candidate")
        self.assertTrue(check["matched"])
        self.assertEqual(check["objects_compared"], len(self._scene()))
        self.assertEqual(check["objects_missing"], 0)
        self.assertLess(check["max_position_error_cm"], 1e-6)
        self.assertLess(check["max_size_error_cm"], 1e-6)
        self.assertEqual(check["max_orientation_error_deg"], 0.0)
        self.assertEqual(result["problems"], [])
        self.assertTrue(check["fit"]["available"])
        self.assertTrue(check["fit"]["signed_permutation"])
        close(self, check["fit"]["determinant"], -1.0)
        self.assertEqual(check["fit"]["permutation"], [1, 2, 0])
        self.assertEqual(check["fit"]["signs"], [1, 1, -1])

    def test_the_candidate_list_scores_every_axis_map(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        tc = result["transform_check"]
        scores = {entry["name"]: entry for entry in tc["candidates"]}
        self.assertEqual(len(scores), len(mapping.AXIS_CANDIDATES))
        for name, entry in scores.items():
            self.assertEqual(entry["objects_compared"], len(self._scene()), name)
        self.assertLess(scores[CAMERA]["max_position_error_cm"], 1e-6)
        self.assertGreater(scores[IDENTITY]["max_position_error_cm"], 100.0)
        self.assertGreater(scores[ENGINE]["max_position_error_cm"], 100.0)
        self.assertGreater(scores[CLASSIC]["max_orientation_error_deg"], 10.0)
        camera_contract = tc["camera_contract"]
        self.assertEqual(camera_contract["candidate"], CAMERA)
        self.assertLess(camera_contract["max_position_error_cm"], 1e-6)
        self.assertTrue(camera_contract["within_tolerance"])
        self.assertEqual(len(camera_contract["matrix"]), 9)
        self.assertTrue(camera_contract["note"])

    def test_a_scene_in_the_engine_map_names_the_engine_candidate(self):
        scene = self._scene_for(ENGINE)
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertEqual(tc["best"], ENGINE)
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertTrue(tc["matched"])
        self.assertEqual(result["problems"], [])
        self.assertLess(tc["max_position_error_cm"], 1e-6)
        self.assertLess(tc["max_size_error_cm"], 1e-6)
        contract = tc["camera_contract"]
        self.assertEqual(contract["candidate"], CAMERA)
        self.assertGreater(contract["max_position_error_cm"], 100.0)
        self.assertGreater(contract["max_size_error_cm"], 10.0)
        self.assertFalse(contract["within_tolerance"])
        self.assertIn("reconcile", contract["note"])
        self.assertIn("does not correct", contract["note"])

    def test_the_classic_candidate_alone_is_not_the_engine_match(self):
        scene = self._scene_for(ENGINE)
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        scores = {entry["name"]: entry for entry in
                  result["transform_check"]["candidates"]}
        self.assertEqual(result["transform_check"]["best"], ENGINE)
        self.assertNotEqual(result["transform_check"]["best"], CLASSIC)
        self.assertGreater(scores[CLASSIC]["max_position_error_cm"], 100.0)

    def test_a_consistent_unknown_map_is_accepted_through_the_fit(self):
        # A signed permutation that is not one of the named candidates still
        # places every object exactly; the fit has to decide it.
        unknown = mapping.matrix3((-1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0))
        scene = self._scene_for_values(unknown)
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertEqual(tc["best"], mapping.FITTED_CANDIDATE_NAME)
        self.assertEqual(tc["decided_by"], "fit")
        self.assertTrue(tc["matched"])
        self.assertEqual(tc["winner"]["source"], "fit")
        self.assertEqual(result["problems"], [])
        self.assertTrue(tc["fit"]["available"] and tc["fit"]["signed_permutation"])
        self.assertLess(tc["fit"]["max_position_error_cm"], 1e-6)
        for entry in result["objects"]:
            self.assertLess(entry["position_error_cm"], 1e-6, entry["node_name"])
            self.assertLess(entry["size_error_cm"], 1e-6, entry["node_name"])
            self.assertEqual(entry["problems"], [])
        for name, score in {entry["name"]: entry for entry in tc["candidates"]}.items():
            self.assertGreater(score["max_position_error_cm"], 100.0, name)

    def test_the_fit_may_not_decide_when_it_is_not_a_signed_permutation(self):
        scene = self._scene_for_values(
            mapping.matrix3((1.1, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0)))
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertNotEqual(tc["best"], mapping.FITTED_CANDIDATE_NAME)
        self.assertFalse(tc["matched"])
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertTrue(result["problems"])

    def test_an_object_whitelists_its_measured_values(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        first = result["objects"][0]
        self.assertEqual(first["node_name"], "SM_Pillar")
        self.assertTrue(first["found"])
        self.assertEqual(first["matched_by"], "name")
        self.assertEqual(len(first["world_matrix"]), 16)
        self.assertAlmostEqual(first["world_bounds_size_cm"]["x"], 100.0)
        self.assertAlmostEqual(first["world_bounds_size_cm"]["y"], 200.0)
        self.assertAlmostEqual(first["world_bounds_size_cm"]["z"], 50.0)
        self.assertLess(first["position_error_cm"], 1e-6)
        self.assertLess(first["size_error_cm"], 1e-6)
        self.assertTrue(first["orientation_checked"])
        self.assertEqual(first["problems"], [])

    def test_a_symmetric_mesh_skips_the_orientation_check(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        rock = [entry for entry in result["objects"] if entry["node_name"] == "SM_Rock"][0]
        self.assertFalse(rock["orientation_checked"])
        self.assertIsNone(rock["orientation_error_deg"])
        self.assertLess(rock["position_error_cm"], 1e-6)

    def test_a_wrong_position_is_reported_with_both_values(self):
        scene = self._scene()
        camera = self._camera_candidate()
        # Move the manifest's Unreal position 25 cm along the object's maya -Z.
        offset = mapping.matrix3_inverse(camera)
        delta = mapping.matrix3_apply(offset, (0.0, 0.0, -25.0))
        position = mapping.matrix4_translation(scene[0]["ue_matrix"])
        scene[0]["ue_matrix"] = mapping.matrix4_from_linear_and_translation(
            mapping.matrix4_linear(scene[0]["ue_matrix"]),
            tuple(position[axis] + delta[axis] for axis in range(3)))
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertFalse(result["transform_check"]["matched"])
        self.assertEqual(len(result["problems"]), 1)
        problem = result["problems"][0]
        self.assertIn("POSITION_MISMATCH", problem)
        self.assertIn("SM_Pillar", problem)
        self.assertIn("25.0000 cm", problem)
        entry = result["objects"][0]
        close(self, entry["position_error_cm"], 25.0, places=6)
        self.assertEqual(entry["problems"], [problem])

    def test_the_offset_is_reported_for_every_object(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        tc = result["transform_check"]
        self.assertEqual(tc["tolerances"]["offset_cm"], 1.0)
        self.assertLess(tc["max_offset_error_cm"], 1e-6)
        for entry in result["objects"]:
            self.assertIsNotNone(entry["offset_error_cm"], entry["node_name"])
            self.assertIsNotNone(entry["measured_offset_cm"], entry["node_name"])
            self.assertIsNotNone(entry["expected_offset_cm"], entry["node_name"])
        pillar = [entry for entry in result["objects"]
                  if entry["node_name"] == "SM_Pillar"][0]
        self.assertEqual(pillar["measured_offset_cm"], {"x": 0.0, "y": 0.0, "z": 25.0})

    def test_the_centroid_is_reported_for_every_object(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, self._scene())
        tc = result["transform_check"]
        self.assertEqual(tc["tolerances"]["centroid_cm"], 1.0)
        self.assertLess(tc["max_centroid_error_cm"], 1e-6)
        for entry in result["objects"]:
            self.assertTrue(entry["centroid_checked"], entry["node_name"])
            self.assertLess(entry["centroid_error_cm"], 1e-6, entry["node_name"])
            self.assertIsNotNone(entry["measured_centroid_cm"], entry["node_name"])
            self.assertIsNotNone(entry["expected_centroid_cm"], entry["node_name"])
        rock = [entry for entry in result["objects"]
                if entry["node_name"] == "SM_Rock"][0]
        self.assertEqual(rock["measured_centroid_cm"],
                         {"x": -450.0, "y": 25.0, "z": 880.0})
        self.assertEqual(rock["measured_offset_cm"], {"x": 0.0, "y": 0.0, "z": 0.0})
        self.assertEqual(result["problems"], [])

    def test_a_mirrored_centroid_is_reported(self):
        # The cone case: the box is centred on the pivot, so position, size and
        # offset all agree; only the vertex centroid moves.
        scene = self._scene()
        rock = [entry for entry in scene if entry["node_name"] == "SM_Rock"][0]
        rock["maya_centroid"] = [rock["maya_centroid"][0], rock["maya_centroid"][1],
                                 -rock["maya_centroid"][2]]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Rock"][0]
        self.assertIn("CENTROID_MISMATCH", " ".join(result["problems"]))
        self.assertIn("SM_Rock", " ".join(result["problems"]))
        self.assertFalse(result["transform_check"]["matched"])
        self.assertGreater(result["transform_check"]["max_centroid_error_cm"], 1.0)
        close(self, entry["centroid_error_cm"], 1760.0, places=4)
        self.assertLess(entry["position_error_cm"], 1e-6)
        self.assertLess(entry["size_error_cm"], 1e-6)
        self.assertLess(entry["offset_error_cm"], 1e-6)
        self.assertFalse(entry["orientation_checked"])

    def test_a_centroid_mismatch_stops_a_map_from_winning(self):
        scene = self._scene()
        rock = [entry for entry in scene if entry["node_name"] == "SM_Rock"][0]
        rock["maya_centroid"] = [rock["maya_centroid"][0], rock["maya_centroid"][1],
                                 -rock["maya_centroid"][2]]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertFalse(tc["matched"])
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertNotEqual(tc["best"], mapping.FITTED_CANDIDATE_NAME)
        for entry in tc["candidates"]:
            self.assertGreater(entry["max_centroid_error_cm"], 1.0, entry["name"])

    def test_a_manifest_without_centroids_reports_the_check_unavailable(self):
        scene = self._scene()
        for entry in scene:
            entry["ue_centroid"] = None
            entry["maya_centroid"] = None
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertTrue(result["transform_check"]["matched"])
        self.assertEqual(result["problems"], [])
        self.assertIsNone(result["transform_check"]["max_centroid_error_cm"])
        self.assertTrue(any("CENTROID_UNAVAILABLE" in warning
                            for warning in result["warnings"]),
                        json.dumps(result["warnings"]))
        for entry in result["objects"]:
            self.assertFalse(entry["centroid_checked"], entry["node_name"])
            self.assertIsNone(entry["centroid_error_cm"], entry["node_name"])
            self.assertEqual(entry["problems"], [], entry["node_name"])

    def test_a_mirrored_offset_is_reported(self):
        # A mirrored arrival keeps the position and the box size and moves the
        # box centre: only the offset shows it.
        scene = self._scene()
        wedge = [entry for entry in scene if entry["node_name"] == "SM_Wedge"][0]
        wedge["maya_offset"] = [-value for value in wedge["maya_offset"]]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        problems = " ".join(result["problems"])
        self.assertIn("OFFSET_MISMATCH", problems)
        self.assertIn("SM_Wedge", problems)
        self.assertFalse(result["transform_check"]["matched"])
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Wedge"][0]
        close(self, entry["offset_error_cm"],
              mapping.vector_length([2 * value for value in wedge["maya_offset"]]),
              places=6)
        self.assertEqual(entry["measured_offset_cm"]["x"], 50.0)
        self.assertLess(entry["position_error_cm"], 1e-6)
        self.assertLess(entry["size_error_cm"], 1e-6)

    def test_the_offset_is_checked_where_the_orientation_is_skipped(self):
        # The wedge's local extents are all 100, so its orientation cannot be
        # measured; the offset still can, and a mirror still shows up.
        scene = self._scene()
        wedge = [entry for entry in scene if entry["node_name"] == "SM_Wedge"][0]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Wedge"][0]
        self.assertFalse(entry["orientation_checked"])
        self.assertIsNotNone(entry["offset_error_cm"])
        self.assertLess(entry["offset_error_cm"], 1e-6)
        wedge["maya_offset"] = [wedge["maya_offset"][0], -wedge["maya_offset"][1],
                                wedge["maya_offset"][2]]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Wedge"][0]
        self.assertFalse(entry["orientation_checked"])
        self.assertIn("OFFSET_MISMATCH", " ".join(entry["problems"]))
        self.assertFalse(result["transform_check"]["matched"])

    def test_an_offset_mismatch_stops_a_map_from_winning(self):
        # Positions and sizes are consistent under the camera map, the offsets
        # are not: no map may be reported as the match.
        scene = self._scene()
        wedge = [entry for entry in scene if entry["node_name"] == "SM_Wedge"][0]
        wedge["maya_offset"] = [-value for value in wedge["maya_offset"]]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertFalse(tc["matched"])
        self.assertGreater(tc["max_offset_error_cm"], 1.0)
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertNotEqual(tc["best"], mapping.FITTED_CANDIDATE_NAME)
        self.assertTrue(any("OFFSET_MISMATCH" in problem for problem in result["problems"]))

    def test_a_wrong_size_is_reported(self):
        scene = self._scene()
        scene[2]["maya_size"] = [400.0, 20.0, 360.0]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        problems = " ".join(result["problems"])
        self.assertIn("SIZE_MISMATCH", problems)
        self.assertIn("SM_Wall", problems)
        close(self, result["objects"][2]["size_error_cm"], 60.0, places=6)

    def test_a_wrong_orientation_is_reported(self):
        scene = self._scene()
        # The wall's own axes are turned a further 90 degrees about Maya's up
        # axis: X goes to -Z and Z goes to X, while the position is unchanged.
        expected = mapping.matrix4_linear(
            mapping.carry_world_matrix(self._camera_candidate(), scene[2]["ue_matrix"]))
        rotated = [tuple(-value for value in expected[2]), expected[1], expected[0]]
        scene[2]["maya_matrix"] = mapping.matrix4_from_linear_and_translation(
            mapping.matrix3(rotated), mapping.matrix4_translation(scene[2]["maya_matrix"]))
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        problems = " ".join(result["problems"])
        self.assertIn("ORIENTATION_MISMATCH", problems)
        close(self, result["objects"][2]["orientation_error_deg"], 90.0, places=4)

    def test_a_missing_node_is_a_problem_not_a_skip(self):
        scene = self._scene()
        scene[1]["found"] = False
        scene[1]["path"] = None
        scene[1]["maya_matrix"] = None
        scene[1]["maya_size"] = None
        scene[1]["local_size"] = None
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertFalse(result["transform_check"]["matched"])
        self.assertEqual(result["transform_check"]["objects_missing"], 1)
        self.assertEqual(result["transform_check"]["objects_compared"],
                         len(self._scene()) - 1)
        self.assertIn("MISSING_NODE", result["problems"][0])
        self.assertIn("SM_Rock", result["problems"][0])
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Rock"][0]
        self.assertFalse(entry["found"])
        self.assertIsNone(entry["position_error_cm"])

    def test_an_unmatched_axis_map_fails_every_object(self):
        scene = self._scene()
        for entry in scene:
            position = mapping.matrix4_translation(entry["ue_matrix"])
            entry["ue_matrix"] = mapping.matrix4_from_linear_and_translation(
                mapping.matrix4_linear(entry["ue_matrix"]),
                (position[0] + 5000.0, position[1], position[2]))
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertFalse(result["transform_check"]["matched"])
        self.assertEqual(len(result["problems"]), len(scene))
        for problem in result["problems"]:
            self.assertIn("POSITION_MISMATCH", problem)
        for entry in result["transform_check"]["candidates"]:
            self.assertGreater(entry["max_position_error_cm"], 1000.0, entry["name"])

    def test_no_objects_compares_nothing(self):
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, [])
        check = result["transform_check"]
        self.assertFalse(check["matched"])
        self.assertIsNone(check["candidate"])
        self.assertEqual(check["objects_compared"], 0)
        self.assertEqual(result["objects"], [])
        self.assertEqual(result["problems"], [])
        self.assertFalse(check["fit"]["available"])

    def test_tolerances_can_be_tightened(self):
        scene = self._scene()
        position = mapping.matrix4_translation(scene[0]["maya_matrix"])
        scene[0]["maya_matrix"] = mapping.matrix4_from_linear_and_translation(
            mapping.matrix4_linear(scene[0]["maya_matrix"]),
            (position[0] + 0.5, position[1], position[2]))
        relaxed = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertTrue(relaxed["transform_check"]["matched"])
        strict = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene,
                                              tolerances={"position_cm": 0.1})
        self.assertFalse(strict["transform_check"]["matched"])
        self.assertIn("POSITION_MISMATCH", " ".join(strict["problems"]))

    def test_two_objects_on_one_node_are_reported_as_shared(self):
        scene = self._scene()
        scene[1]["path"] = scene[0]["path"]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertTrue(any("SHARED_NODE" in warning for warning in result["warnings"]))

    def test_manifest_field_problems_reach_the_report(self):
        scene = self._scene()
        scene[0]["problems"] = ["SM_Pillar: manifest object has no world_matrix"]
        result = mapping.compare_measurements(mapping.AXIS_CANDIDATES, scene)
        self.assertTrue(any("MANIFEST_FIELD" in problem for problem in result["problems"]))


if __name__ == "__main__":
    unittest.main()
