"""Pure-Python checks for ``mtou_livelink.reference``.

These are the prototype's pure checks ported to the product module
``mtou_livelink.reference``; they run under plain CPython 3 and nothing here
imports Maya. The synthetic ASCII FBX fragments are the controls for the
texture detector, so the test states what the rule counts and what it ignores.

    python -m unittest test_mtou_reference
"""

import hashlib
import json
import math
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))

from mtou_livelink import camera as camera_module  # noqa: E402
from mtou_livelink import reference  # noqa: E402

#: The repository root, so scratch directories land beside the checkout.
REPO = pathlib.Path(__file__).resolve().parents[5]


def scratch_directory(prefix):
    """A disposable directory under the repository's scratch folder."""
    for candidate in (REPO.parent / ".tmp", REPO / ".tmp"):
        if candidate.is_dir() and os.access(str(candidate), os.W_OK):
            return pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=str(candidate)))
    return pathlib.Path(tempfile.mkdtemp(prefix=prefix))

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
    for candidate in reference.AXIS_CANDIDATES:
        if candidate["name"] == name:
            return reference.matrix3(candidate["matrix"])
    raise KeyError(name)


CAMERA = reference.CAMERA_SYNC_CANDIDATE
ENGINE = reference.ENGINE_HANDOFF_CANDIDATE
CLASSIC = "maya_x=ue_x, maya_y=ue_z, maya_z=-ue_y"
MIRROR = "maya_x=-ue_y, maya_y=ue_z, maya_z=ue_x"
IDENTITY = "identity"


class AxisCandidatesTest(unittest.TestCase):

    def test_the_engine_handoff_map_is_a_candidate(self):
        entry = [candidate for candidate in reference.AXIS_CANDIDATES
                 if candidate["name"] == reference.ENGINE_HANDOFF_CANDIDATE][0]
        matrix = reference.matrix3(entry["matrix"])
        self.assertEqual(reference.matrix3_apply(matrix, (1, 0, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(reference.matrix3_apply(matrix, (0, 1, 0)), (0.0, 0.0, 1.0))
        self.assertEqual(reference.matrix3_apply(matrix, (0, 0, 1)), (0.0, 1.0, 0.0))
        close(self, reference.matrix3_determinant(matrix), -1.0)
        self.assertIn("2026-09-28", entry["note"])
        self.assertIn("determinant -1", entry["note"])

    def test_candidate_matrices_are_the_documented_maps(self):
        camera = candidate_matrix(reference.CAMERA_SYNC_CANDIDATE)
        self.assertEqual(reference.matrix3_apply(camera, (1, 0, 0)), (0.0, 0.0, -1.0))
        self.assertEqual(reference.matrix3_apply(camera, (0, 1, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(reference.matrix3_apply(camera, (0, 0, 1)), (0.0, 1.0, 0.0))
        upward = candidate_matrix(CLASSIC)
        self.assertEqual(reference.matrix3_apply(upward, (0, 0, 1)), (0.0, 1.0, 0.0))
        self.assertEqual(reference.matrix3_apply(upward, (0, 1, 0)), (0.0, 0.0, -1.0))
        engine = candidate_matrix(ENGINE)
        self.assertEqual(reference.matrix3_apply(engine, (1, 0, 0)), (1.0, 0.0, 0.0))
        self.assertEqual(reference.matrix3_apply(engine, (0, 0, 1)), (0.0, 1.0, 0.0))

    def test_handedness_of_the_candidates(self):
        determinants = {candidate["name"]: reference.matrix3_determinant(
            reference.matrix3(candidate["matrix"])) for candidate in reference.AXIS_CANDIDATES}
        close(self, determinants[CAMERA], -1.0)
        close(self, determinants[ENGINE], -1.0)
        close(self, determinants[MIRROR], -1.0)
        close(self, determinants[CLASSIC], 1.0)
        close(self, determinants[IDENTITY], 1.0)

    def test_the_handoff_and_the_camera_route_disagree(self):
        engine = candidate_matrix(ENGINE)
        camera = candidate_matrix(CAMERA)
        self.assertNotEqual(reference.matrix3_values(engine),
                            reference.matrix3_values(camera))
        for point in ((100.0, 200.0, 300.0), (-450.0, 25.0, 900.0)):
            self.assertNotEqual(reference.matrix3_apply(engine, point),
                                reference.matrix3_apply(camera, point))

    def test_every_candidate_is_a_signed_permutation(self):
        for candidate in reference.AXIS_CANDIDATES:
            report = reference.signed_permutation(candidate["matrix"])
            self.assertTrue(report["signed_permutation"], candidate["name"])
            self.assertEqual(sorted(report["permutation"]), [0, 1, 2])
            close(self, report["max_permutation_deviation"], 0.0)


class MatrixTest(unittest.TestCase):

    def test_determinant_and_axis_rows(self):
        matrix = reference.matrix3((0, 2, 0, 0, 0, 3, -1, 0, 0))
        close(self, reference.matrix3_determinant(matrix), -6.0)
        # The rows of a candidate are the three Unreal axes in Maya terms.
        self.assertEqual(reference.matrix4_linear(
            reference.matrix4_from_linear_and_translation(matrix, (0.0, 0.0, 0.0))), matrix)
        self.assertEqual(reference.matrix3_transpose(matrix),
                         ((0.0, 0.0, -1.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0)))

    def test_multiply_and_apply_agree(self):
        first = reference.matrix3((1, 2, 3, 4, 5, 6, 7, 8, 10))
        second = reference.matrix3((0, 1, 0, 1, 0, 0, 0, 0, 1))
        product = reference.matrix3_multiply(first, second)
        vector = (1.0, -2.0, 3.5)
        self.assertEqual(reference.matrix3_apply(product, vector),
                         reference.matrix3_apply(first, reference.matrix3_apply(second, vector)))

    def test_inverse_round_trip(self):
        matrix = reference.matrix3((2.0, 0.5, 1.0, -1.0, 3.0, 0.25, 0.0, 2.0, -4.0))
        inverse = reference.matrix3_inverse(matrix)
        product = reference.matrix3_multiply(matrix, inverse)
        for row in range(3):
            for column in range(3):
                close(self, product[row][column], 1.0 if row == column else 0.0, places=9)

    def test_singular_matrix_has_no_inverse(self):
        self.assertIsNone(reference.matrix3_inverse(reference.matrix3((1, 2, 3, 2, 4, 6, 7, 8, 9))))
        self.assertIsNone(reference.matrix3_inverse(reference.matrix3((0, 0, 0, 0, 0, 0, 0, 0, 0))))

    def test_matrix3_rejects_wrong_value_counts(self):
        with self.assertRaises(ValueError):
            reference.matrix3((1, 2, 3))

    def test_matrix4_helpers_read_the_hosts_layout(self):
        # Rows are the axis images and the fourth row is the translation, which
        # is what both Maya's xform -m and Unreal's FMatrix write.
        values = [1.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 1.0, 0.0,
                  0.0, -1.0, 0.0, 0.0,
                  10.0, 20.0, 30.0, 1.0]
        self.assertEqual(reference.matrix4_translation(values), (10.0, 20.0, 30.0))
        linear = reference.matrix4_linear(values)
        self.assertEqual(reference.matrix4_linear(values)[1], (0.0, 0.0, 1.0))
        rebuilt = reference.matrix4_from_linear_and_translation(linear, (10.0, 20.0, 30.0))
        self.assertEqual(rebuilt, values)

    def test_angle_between_degrees(self):
        close(self, reference.angle_between_degrees((1, 0, 0), (1, 0, 0)), 0.0)
        close(self, reference.angle_between_degrees((1, 0, 0), (0, 1, 0)), 90.0)
        close(self, reference.angle_between_degrees((1, 0, 0), (-3, 0, 0)), 180.0)
        self.assertIsNone(reference.angle_between_degrees((0, 0, 0), (1, 0, 0)))

    def test_expected_axis_sizes_permutes(self):
        camera = candidate_matrix(CAMERA)
        # the prototype map ue (x, y, z) -> maya (y, z, -x), so the Maya size is (size_y, size_z, size_x).
        self.assertEqual(reference.expected_axis_sizes(camera, (10.0, 20.0, 30.0)),
                         [20.0, 30.0, 10.0])


class SignedPermutationTest(unittest.TestCase):

    def test_identity_is_a_signed_permutation(self):
        report = reference.signed_permutation((1, 0, 0, 0, 1, 0, 0, 0, 1))
        self.assertTrue(report["signed_permutation"])
        self.assertEqual(report["permutation"], [0, 1, 2])
        self.assertEqual(report["signs"], [1, 1, 1])

    def test_scale_is_not_a_signed_permutation(self):
        report = reference.signed_permutation((2, 0, 0, 0, 1, 0, 0, 0, 1))
        self.assertFalse(report["signed_permutation"])
        close(self, report["max_permutation_deviation"], 1.0)

    def test_rotation_by_forty_five_degrees_is_not_a_signed_permutation(self):
        half = 2.0 ** -0.5
        report = reference.signed_permutation((half, -half, 0, half, half, 0, 0, 0, 1))
        self.assertFalse(report["signed_permutation"])
        self.assertGreater(report["max_permutation_deviation"], 0.2)


class FitTest(unittest.TestCase):

    def _pairs(self, name=CAMERA):
        matrix = candidate_matrix(name)
        unreal = [(100.0, 200.0, 300.0), (-400.0, 50.0, 900.0), (0.0, 0.0, 0.0),
                  (700.0, -20.0, 10.0)]
        return unreal, [reference.matrix3_apply(matrix, point) for point in unreal], matrix

    def test_fit_recovers_the_candidate(self):
        unreal, maya, matrix = self._pairs()
        fit = reference.fit_linear_map(unreal, maya)
        self.assertTrue(fit["available"], fit["reason"])
        self.assertTrue(fit["signed_permutation"])
        close(self, fit["determinant"], -1.0)
        self.assertLess(fit["max_position_error_cm"], 1e-6)
        for expected, actual in zip(reference.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=9)

    def test_fit_recovers_the_classic_map(self):
        unreal, maya, matrix = self._pairs(CLASSIC)
        fit = reference.fit_linear_map(unreal, maya)
        close(self, fit["determinant"], 1.0)
        self.assertEqual(fit["permutation"], [0, 2, 1])
        self.assertEqual(fit["signs"], [1, 1, -1])

    def test_three_points_do_not_determine_a_map(self):
        unreal, maya, _ = self._pairs()
        fit = reference.fit_linear_map(unreal[:3], maya[:3])
        self.assertFalse(fit["available"])
        self.assertIn("plane", fit["reason"])

    def test_two_points_do_not_determine_a_map(self):
        fit = reference.fit_linear_map([(1.0, 2.0, 3.0), (4.0, 5.0, 6.0)],
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
        fit = reference.fit_linear_map(moved, maya)
        self.assertTrue(fit["available"])
        for expected, actual in zip(reference.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=6)
        expected_error = reference.vector_length(reference.matrix3_apply(matrix, offset))
        close(self, fit["max_position_error_cm"], expected_error, places=3)
        close(self, fit["rms_position_error_cm"], expected_error, places=3)

    def test_a_moved_level_still_fits(self):
        # The same world offset on both sides is an exact relation: the fitted
        # map is recovered and the residual stays at machine precision.
        unreal, maya, matrix = self._pairs()
        offset = (500.0, -250.0, 75.0)
        moved_unreal = [tuple(point[axis] + offset[axis] for axis in range(3))
                        for point in unreal]
        moved_maya = [reference.matrix3_apply(matrix, point) for point in moved_unreal]
        fit = reference.fit_linear_map(moved_unreal, moved_maya)
        self.assertTrue(fit["available"])
        self.assertLess(fit["max_position_error_cm"], 1e-6)
        for expected, actual in zip(reference.matrix3_values(matrix), fit["matrix"]):
            close(self, expected, actual, places=6)

    def test_a_wrong_map_is_reported_by_its_residual(self):
        unreal, maya, _ = self._pairs()
        broken = [(x, y, z + 25.0) for x, y, z in maya]
        fit = reference.fit_linear_map(unreal, broken)
        self.assertTrue(fit["available"])
        close(self, fit["max_position_error_cm"], 25.0, places=6)


class OrientationMeasurabilityTest(unittest.TestCase):

    def test_distinct_extents_are_measurable(self):
        self.assertTrue(reference.orientation_measurable([10.0, 20.0, 30.0]))
        self.assertTrue(reference.orientation_measurable([10.0, 10.5, 30.0]))

    def test_symmetric_meshes_are_not_measurable(self):
        self.assertFalse(reference.orientation_measurable([20.0, 20.0, 20.0]))
        self.assertFalse(reference.orientation_measurable([20.0, 20.0, 30.0]))
        self.assertFalse(reference.orientation_measurable([0.0, 0.0, 0.0]))

    def test_missing_size_is_not_measurable(self):
        self.assertFalse(reference.orientation_measurable(None))
        self.assertFalse(reference.orientation_measurable([1.0, 2.0]))


class TextureScanTextTest(unittest.TestCase):

    def test_the_engine_fragment_control(self):
        scan = reference.scan_fbx_texture_records(ENGINE_FRAGMENT)
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["texture_references"], 2)
        self.assertEqual(scan["video_references"], 1)
        self.assertEqual(scan["files"], ["assets/textures/wall_brick.png",
                                         "textures/wall_brick.png",
                                         "handoff/assets/door.png",
                                         "assets/door.png",
                                         "Take_001.tak"])
        self.assertEqual(scan["texture_files"], ["assets/textures/wall_brick.png",
                                                 "textures/wall_brick.png",
                                                 "handoff/assets/door.png",
                                                 "assets/door.png"])
        self.assertEqual(scan["video_files"], ["Take_001.tak"])
        self.assertEqual(scan["image_files"], ["assets/textures/wall_brick.png",
                                               "textures/wall_brick.png",
                                               "handoff/assets/door.png",
                                               "assets/door.png"])
        self.assertEqual(scan["content_records"], 0)
        self.assertEqual(scan["embedded_media_records"], 0)
        self.assertEqual(scan["camera_records"], 0)
        self.assertEqual(scan["light_records"], 0)
        self.assertEqual(len(scan["records"]), 2)
        self.assertEqual(scan["records"][0]["line"], 21)
        self.assertEqual(scan["records"][0]["file_names"],
                         ["assets/textures/wall_brick.png", "textures/wall_brick.png"])
        self.assertEqual(scan["records"][1]["file_names"],
                         ["handoff/assets/door.png", "assets/door.png"])

    def test_a_nested_block_does_not_end_the_record(self):
        # The SDK writes a nested Properties70 block before the name lines; the
        # record ends only at the brace that closes it, so the names that follow
        # the nested block belong to the record.
        scan = reference.scan_fbx_texture_records(ENGINE_FRAGMENT)
        self.assertEqual(scan["records"][1]["header"],
                         'Texture: 2443664220208, "Texture::file1", "" {')
        self.assertEqual(scan["records"][1]["file_names"],
                         ["handoff/assets/door.png", "assets/door.png"])
        self.assertIn("assets/door.png", scan["texture_files"])

    def test_the_take_clip_is_not_a_texture(self):
        scan = reference.scan_fbx_texture_records(ENGINE_FRAGMENT)
        self.assertIn("Take_001.tak", scan["video_files"])
        self.assertNotIn("Take_001.tak", scan["texture_files"])
        self.assertEqual(scan["video_records"][0]["header"],
                         'Video: 12, "Video::Take_001", "Clip" {')

    def test_embedded_media_is_detected_on_the_following_line(self):
        # Measured layout: the SDK writes ``Content: ,`` and the base64 payload
        # on the next line.
        payload = "iVBORw0KGgoAAAANSUhEUg"
        text = ('Objects:  {\n'
                '\tVideo: 12, "Video::file1", "Clip" {\n'
                '\t\tType: "Clip"\n'
                '\t\tProperties70:  {\n'
                '\t\t\tP: "Path", "KString", "XRefUrl", "", "textures/probe.png"\n'
                '\t\t}\n'
                '\t\tFilename: "textures/probe.png"\n'
                '\t\tRelativeFilename: "probe.png"\n'
                '\t\tContent: ,\n'
                '\t\t "' + payload + '"\n'
                '\t}\n'
                '\tTexture: 13, "Texture::file1", "" {\n'
                '\t\tMedia: "Video::file1"\n'
                '\t}\n'
                '}\n')
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["content_records"], 1)
        self.assertEqual(scan["embedded_media_records"], 1)
        self.assertEqual(scan["embedded_media"][0]["kind"], "Video")
        self.assertEqual(scan["embedded_media"][0]["payload_characters"], len(payload))
        self.assertFalse(scan["embedded_media"][0]["same_line"])
        # The record the payload belongs to stays open and still ends normally.
        self.assertEqual(scan["texture_records"], 1)
        self.assertEqual(scan["texture_references"], 0)

    def test_embedded_media_on_the_same_line_is_detected(self):
        text = ('Objects:  {\n'
                '\tVideo: 12, "Video::file1", "Clip" {\n'
                '\t\tContent: ,"QUJDRA=="\n'
                '\t}\n'
                '}\n')
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["embedded_media_records"], 1)
        self.assertTrue(scan["embedded_media"][0]["same_line"])
        self.assertEqual(scan["embedded_media"][0]["payload_characters"], 8)

    def test_an_empty_content_record_carries_no_media(self):
        text = ('Objects:  {\n'
                '\tVideo: 12, "Video::file1", "Clip" {\n'
                '\t\tFilename: "probe.png"\n'
                '\t\tContent: ,\n'
                '\t}\n'
                '}\n')
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["content_records"], 1)
        self.assertEqual(scan["embedded_media_records"], 0)
        self.assertEqual(scan["video_files"], ["probe.png"])

    def test_camera_and_light_node_attributes_are_counted(self):
        text = ('Objects:  {\n'
                '\tNodeAttribute: 1, "NodeAttribute::probe_cam1", "Camera" {\n'
                '\t\tTypeFlags: "Camera"\n'
                '\t}\n'
                '\tNodeAttribute: 2, "NodeAttribute::", "Light" {\n'
                '\t\tTypeFlags: "Light"\n'
                '\t}\n'
                '\tNodeAttribute: 3, "NodeAttribute::box", "Mesh" {\n'
                '\t\tTypeFlags: "Mesh"\n'
                '\t}\n'
                '}\n')
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["camera_records"], 1)
        self.assertEqual(scan["light_records"], 1)
        self.assertEqual(scan["texture_records"], 0)

    def test_the_camera_and_light_heads_are_not_record_heads(self):
        # The FBX SDK writes cameras and lights as NodeAttribute records, so a
        # ``Camera:`` or ``Light:`` line head must not be counted as one.
        text = 'Objects:  {\n\tCamera: "Camera::x", "Camera" {\n\t}\n}\n'
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["camera_records"], 0)

    def test_the_definitions_template_and_comments_are_ignored(self):
        scan = reference.scan_fbx_texture_records(ENGINE_FRAGMENT)
        headers = [record["header"] for record in scan["records"]]
        self.assertEqual(len(headers), 2)
        for header in headers:
            self.assertTrue(header.startswith("Texture:"), header)
        self.assertNotIn("T_Commented_Out", " ".join(headers))

    def test_a_clean_fragment_records_nothing(self):
        scan = reference.scan_fbx_texture_records(CLEAN_FRAGMENT)
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
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["files"], ["kept/last_token.jpg"])

    def test_a_record_with_empty_names_is_still_a_record(self):
        text = 'Objects:  {\n\tTexture: "T_Empty", "" {\n\t\tFileName: ""\n\t}\n}\n'
        scan = reference.scan_fbx_texture_records(text)
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
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["texture_references"], 2)
        self.assertEqual(scan["files"], ["b.png", "a.png"])

    def test_closing_brace_ends_a_record(self):
        text = ('Objects:  {\n'
                '\tTexture: "T_A", "" {\n'
                '\t}\n'
                '\tFileName: "outside.png"\n'
                '}\n')
        scan = reference.scan_fbx_texture_records(text)
        self.assertEqual(scan["texture_records"], 1)
        self.assertEqual(scan["files"], [])


class ImageFileNameTest(unittest.TestCase):

    def test_image_suffixes(self):
        for name in ("a.png", "a.PNG", "b.bmp", "c.tga", "d.jpg", "e.jpeg", "f.exr",
                     "g.hdr", "h.dds", "i.fbm/tile.tga", "textures\\wall.tga"):
            self.assertTrue(reference.is_image_file_name(name), name)

    def test_non_image_names(self):
        for name in ("", None, "T_Wall", "wall.uasset", "Take_001.tak", "mesh.fbx"):
            self.assertFalse(reference.is_image_file_name(name), str(name))


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
        scan = reference.scan_fbx_file(str(path))
        self.assertTrue(scan["exists"] and scan["readable"])
        self.assertEqual(scan["format"], "ascii")
        self.assertFalse(scan["binary"])
        self.assertFalse(scan["media_heuristic"])
        self.assertEqual(scan["texture_records"], 2)
        self.assertEqual(scan["embedded_media_records"], 0)
        self.assertEqual(scan["camera_records"], 0)
        self.assertEqual(scan["light_records"], 0)
        self.assertEqual(scan["files"], ["assets/textures/wall_brick.png",
                                         "textures/wall_brick.png",
                                         "handoff/assets/door.png",
                                         "assets/door.png",
                                         "Take_001.tak"])
        self.assertGreater(scan["bytes"], 0)

    def test_the_image_files_beside_a_handoff_are_listed(self):
        self._write("watermark.png", "not a real png")
        self._write("handoff.fbx", CLEAN_FRAGMENT)
        self._write("notes.txt", "not an image")
        listed = reference.image_files_beside(str(self.directory / "handoff.fbx"))
        self.assertEqual(listed, ["watermark.png"])
        self.assertEqual(reference.image_files_beside(str(self.directory / "missing.fbx")),
                         ["watermark.png"])

    def test_a_clean_handoff_reports_nothing(self):
        path = self._write("clean.fbx", CLEAN_FRAGMENT)
        scan = reference.scan_fbx_file(str(path))
        self.assertEqual(scan["texture_records"], 0)
        self.assertEqual(scan["texture_references"], 0)
        self.assertEqual(scan["files"], [])

    def test_a_binary_magic_file_is_not_read_as_text(self):
        path = self.directory / "binary.fbx"
        path.write_bytes(reference.FBX_BINARY_MAGIC + b"\x00\x1a\x00\x14" + b"\x00" * 64)
        scan = reference.scan_fbx_file(str(path))
        self.assertEqual(scan["format"], "binary")
        self.assertTrue(scan["binary"])
        self.assertIn("heuristic", scan["note"])

    def test_a_non_fbx_file_is_reported_as_unknown(self):
        path = self._write("notes.txt", "this is not an fbx file\n")
        scan = reference.scan_fbx_file(str(path))
        self.assertEqual(scan["format"], "unknown")
        self.assertIn("not an FBX file", scan["note"])

    def test_a_missing_file_is_reported(self):
        scan = reference.scan_fbx_file(str(self.directory / "missing.fbx"))
        self.assertFalse(scan["exists"])
        self.assertFalse(scan["readable"])
        self.assertTrue(scan["error"])

    def test_sniffing_reads_the_header_not_the_extension(self):
        path = self._write("handoff.txt", ENGINE_FRAGMENT)
        sniffed = reference.sniff_fbx_file(str(path))
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
        manifest = reference.load_manifest(str(path))
        self.assertEqual(manifest["schema"], reference.MANIFEST_SCHEMA)

    def test_a_missing_manifest_is_refused(self):
        with self.assertRaises(reference.ManifestError) as caught:
            reference.load_manifest(str(self.directory / "missing.json"))
        self.assertEqual(caught.exception.category, "MANIFEST_UNREADABLE")

    def test_invalid_json_is_refused(self):
        path = self._write("broken.json", "{not json")
        with self.assertRaises(reference.ManifestError) as caught:
            reference.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_INVALID_JSON")

    def test_a_json_array_is_refused(self):
        path = self._write("array.json", [1, 2, 3])
        with self.assertRaises(reference.ManifestError) as caught:
            reference.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_INVALID_JSON")

    def test_another_schema_is_refused(self):
        manifest = self._manifest()
        manifest["schema"] = "mtou-scene-ref-manifest/2"
        path = self._write("schema.json", manifest)
        with self.assertRaises(reference.ManifestError) as caught:
            reference.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_SCHEMA_MISMATCH")
        self.assertIn("mtou-scene-ref-manifest/1", caught.exception.detail)

    def test_a_missing_objects_list_is_refused(self):
        manifest = self._manifest()
        del manifest["objects"]
        path = self._write("objects.json", manifest)
        with self.assertRaises(reference.ManifestError) as caught:
            reference.load_manifest(str(path))
        self.assertEqual(caught.exception.category, "MANIFEST_MISSING_OBJECTS")

    def test_only_objects_exported_true_are_required(self):
        manifest = self._manifest()
        exported = reference.exported_objects(manifest)
        self.assertEqual([entry["node_name"] for entry in exported], ["SM_A"])
        manifest["objects"][1]["exported"] = "yes"
        self.assertEqual(len(reference.exported_objects(manifest)), 1)

    def test_the_summary_reports_the_scope_and_the_scale(self):
        summary = reference.manifest_summary(self._manifest())
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
            reference.manifest_object_requirement(manifest["objects"][0])
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
            reference.manifest_object_requirement(entry)
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
            reference.manifest_object_requirement(entry)
        self.assertEqual(centroid, [1.0, -2.0, 25.0])
        self.assertEqual(offset, [0.0, 0.0, 50.0])
        self.assertEqual(problems, [])

    def test_a_manifest_without_a_centroid_is_not_a_problem(self):
        manifest = self._manifest()
        name, _, _, _, centroid, problems = reference.manifest_object_requirement(
            manifest["objects"][0])
        self.assertEqual(name, "SM_A")
        self.assertIsNone(centroid)
        self.assertEqual(problems, [])

    def test_requirement_reports_missing_fields(self):
        name, matrix, size, offset, centroid, problems = \
            reference.manifest_object_requirement({"id": "x", "exported": True})
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
            reference.manifest_object_requirement(
                {"node_name": "SM_C", "world_matrix": [1.0, 2.0, 3.0],
                 "world_bounds_cm": {"size": {"x": 1.0, "y": 1.0, "z": 1.0}}})
        self.assertIsNone(matrix)
        self.assertIsNone(offset)
        self.assertIsNone(centroid)
        self.assertEqual(len(problems), 2)
        self.assertTrue(any("world_matrix" in problem for problem in problems))
        self.assertTrue(any("min/max" in problem for problem in problems))



class ScopeCoverageTest(unittest.TestCase):
    """What a manifest says about the completeness of its own scope."""

    def _summary(self, world_partition=False, completeness=None, partition=None):
        scope = {"kind": "level_range", "persistent_level": "/Game/Map"}
        if completeness is not None:
            scope["completeness"] = completeness
        if partition is not None:
            scope["world_partition"] = partition
        return {"world": {"package": "/Game/Map", "world_partition": world_partition},
                "scope": scope}

    def test_the_summary_carries_the_coverage_fields(self):
        summary = reference.manifest_summary({
            "schema": reference.MANIFEST_SCHEMA,
            "world": {"package": "/Game/Open", "world_partition": True},
            "scope": {"kind": "level_range", "persistent_level": "/Game/Open",
                      "coverage": "partition_loaded_content",
                      "completeness": "not_confirmed",
                      "coverage_note": "only the loaded cells are in scope",
                      "world_partition": {
                          "detected": True, "inventory_available": True,
                          "actor_descriptors": 76, "loaded_actor_descriptors": 9,
                          "unloaded_actor_count": 3, "unloaded_hlod_count": 64,
                          "unloaded_actors": ["/Game/Open.Open:PersistentLevel.A"],
                          "inventory_truncated": False, "note": "loaded only"}},
            "objects": []})
        scope = summary["scope"]
        self.assertEqual(scope["coverage"], "partition_loaded_content")
        self.assertEqual(scope["completeness"], "not_confirmed")
        self.assertEqual(scope["coverage_note"], "only the loaded cells are in scope")
        partition = scope["world_partition"]
        self.assertTrue(partition["detected"])
        self.assertEqual(partition["actor_descriptors"], 76)
        self.assertEqual(partition["loaded_actor_descriptors"], 9)
        self.assertEqual(partition["unloaded_actor_count"], 3)
        self.assertEqual(partition["unloaded_hlod_count"], 64)
        self.assertEqual(partition["unloaded_actors"],
                         ["/Game/Open.Open:PersistentLevel.A"])
        self.assertFalse(partition["inventory_truncated"])

    def test_a_manifest_without_a_partition_block_reports_none(self):
        summary = reference.manifest_summary({
            "schema": reference.MANIFEST_SCHEMA, "world": {},
            "scope": {"kind": "level_range"}, "objects": []})
        self.assertIsNone(summary["scope"]["world_partition"])
        self.assertIsNone(summary["scope"]["completeness"])

    def test_a_confirmed_scope_raises_no_warning(self):
        self.assertIsNone(reference.scope_coverage_warning(
            self._summary(completeness="confirmed")))
        self.assertIsNone(reference.scope_coverage_warning(
            self._summary(world_partition=True, completeness="confirmed",
                          partition={"detected": True, "actor_descriptors": 5,
                                     "loaded_actor_descriptors": 5,
                                     "unloaded_actor_count": 0,
                                     "unloaded_hlod_count": 0})))

    def test_an_unconfirmed_partitioned_scope_is_warned_about(self):
        warning = reference.scope_coverage_warning(self._summary(
            world_partition=True, completeness="not_confirmed",
            partition={"detected": True, "actor_descriptors": 76,
                       "loaded_actor_descriptors": 9, "unloaded_actor_count": 3,
                       "unloaded_hlod_count": 64, "inventory_truncated": False}))
        self.assertIn("SCOPE_LOADED_ONLY", warning)
        self.assertIn("actor descriptors 76", warning)
        self.assertIn("spawned 9", warning)
        self.assertIn("authored actors not spawned 3", warning)
        self.assertIn("generated HLOD proxies not spawned 64", warning)
        self.assertNotIn("capped", warning)

    def test_a_capped_inventory_is_named_as_capped(self):
        warning = reference.scope_coverage_warning(self._summary(
            world_partition=True, completeness="not_confirmed",
            partition={"detected": True, "actor_descriptors": 900,
                       "loaded_actor_descriptors": 0, "unloaded_actor_count": 900,
                       "unloaded_hlod_count": 0, "inventory_truncated": True}))
        self.assertIn("capped", warning)

    def test_a_partitioned_manifest_without_the_block_is_still_warned_about(self):
        warning = reference.scope_coverage_warning(
            self._summary(world_partition=True, completeness="not_confirmed"))
        self.assertIn("SCOPE_LOADED_ONLY", warning)
        self.assertIn("no descriptor inventory", warning)

    def test_a_level_range_without_a_statement_is_not_warned_about(self):
        self.assertIsNone(reference.scope_coverage_warning(self._summary()))
        self.assertIsNone(reference.scope_coverage_warning({}))
        self.assertIsNone(reference.scope_coverage_warning(None))


class ExpectedNodeMatrixTest(unittest.TestCase):
    """The relation between an Unreal world matrix and the node matrix a file holds."""

    def setUp(self):
        self.frame = reference.matrix3(reference.ENGINE_HANDOFF_LOCAL_FRAME)
        self.engine = candidate_matrix(ENGINE)
        # A rotated, non-uniformly scaled, off-origin object: the frame factor
        # only shows up against the point map when the axes are not already
        # aligned with the map.
        rotation = math.radians(30.0)
        cosine, sine = math.cos(rotation), math.sin(rotation)
        rotated = (cosine, sine, 0.0, -sine, cosine, 0.0, 0.0, 0.0, 1.0)
        scales = reference.matrix3((2.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.25))
        linear = reference.matrix3_multiply(rotated, scales)
        self.world = reference.matrix4_from_linear_and_translation(
            linear, (1500.0, -800.0, 250.0))

    def test_the_expected_matrix_is_the_frame_composition(self):
        expected = reference.expected_node_matrix(self.engine, self.world)
        linear = reference.matrix3_multiply(
            reference.matrix3_multiply(self.frame, reference.matrix4_linear(self.world)),
            self.engine)
        close(self, reference.matrix3_determinant(reference.matrix4_linear(expected)),
              abs(reference.matrix3_determinant(reference.matrix4_linear(self.world))))
        for row in range(3):
            for column in range(3):
                close(self, expected[row * 4 + column], linear[row][column])
        position = reference.matrix4_translation(expected)
        for axis in range(3):
            close(self, position[axis],
                  reference.matrix3_apply(self.engine,
                                        reference.matrix4_translation(self.world))[axis])

    def test_an_unmeasured_frame_factor_is_unavailable(self):
        self.assertIsNone(reference.expected_node_matrix(self.engine, self.world,
                                                       frame=None))


class ComparisonTest(unittest.TestCase):
    """The comparison half: what a good, a wrong and a missing object report."""

    def _handoff_values(self, maya_matrix, maya_size, maya_offset, maya_centroid,
                        map_name=ENGINE, frame=reference.ENGINE_HANDOFF_LOCAL_FRAME,
                        map_matrix=None):
        """The Unreal values a handoff in ``map_name`` would have produced.

        The relation a file really uses is ``frame . L_ue . map`` with the
        position mapped by ``map``; inverting it here is what makes the
        synthetic scenes handoffs rather than arbitrary matrix pairs.
        """
        matrix = map_matrix if map_matrix is not None else candidate_matrix(map_name)
        inverse = reference.matrix3_inverse(matrix)
        frame_inverse = reference.matrix3_inverse(reference.matrix3(frame))
        unreal_matrix = reference.matrix4_from_linear_and_translation(
            reference.matrix3_multiply(
                reference.matrix3_multiply(frame_inverse,
                                         reference.matrix4_linear(maya_matrix)),
                inverse),
            reference.matrix3_apply(inverse, reference.matrix4_translation(maya_matrix)))
        return {
            "unreal_matrix": unreal_matrix,
            "unreal_size": reference.expected_axis_sizes(inverse, maya_size),
            "unreal_offset": reference.matrix3_apply(inverse, maya_offset),
            "unreal_centroid": reference.matrix3_apply(inverse, maya_centroid),
        }

    def _measurement(self, name, maya_matrix, maya_size, local_size, unreal_matrix=None,
                     unreal_size=None, entry_id=None, maya_offset=None,
                     maya_centroid=None, map_name=ENGINE):
        if maya_offset is None:
            maya_offset = (0.0, 0.0, 0.0)
        if maya_centroid is None:
            maya_centroid = reference.matrix4_translation(maya_matrix)
        derived = self._handoff_values(maya_matrix, maya_size, maya_offset, maya_centroid,
                                       map_name=map_name)
        if unreal_matrix is None:
            unreal_matrix = derived["unreal_matrix"]
        if unreal_size is None:
            unreal_size = derived["unreal_size"]
        unreal_offset = derived["unreal_offset"]
        unreal_centroid = derived["unreal_centroid"]
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

    def _scene_in_map_values(self, map_to_use, frame=None):
        """The same objects as a handoff whose file used an arbitrary map.

        Used for conventions whose node frame factor was never measured, which
        is exactly the case the orientation comparison reports as unavailable.
        """
        scene = []
        for entry in self._scene():
            entry = dict(entry)
            values = self._handoff_values(entry["maya_matrix"], entry["maya_size"],
                                          entry["maya_offset"], entry["maya_centroid"],
                                          map_name=None, map_matrix=map_to_use)
            entry["ue_matrix"] = values["unreal_matrix"]
            entry["ue_size"] = values["unreal_size"]
            entry["ue_offset"] = values["unreal_offset"]
            entry["ue_centroid"] = values["unreal_centroid"]
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

    def test_a_correct_handoff_matches_the_engine_candidate(self):
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
        check = result["transform_check"]
        self.assertEqual(check["candidate"], ENGINE)
        self.assertEqual(check["best"], ENGINE)
        self.assertEqual(check["decided_by"], "candidate")
        self.assertEqual(check["winner"]["name"], ENGINE)
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
        self.assertEqual(check["fit"]["permutation"], [0, 2, 1])
        self.assertEqual(check["fit"]["signs"], [1, 1, 1])

    def test_the_candidate_list_scores_every_axis_map(self):
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
        tc = result["transform_check"]
        scores = {entry["name"]: entry for entry in tc["candidates"]}
        self.assertEqual(len(scores), len(reference.AXIS_CANDIDATES))
        for name, entry in scores.items():
            self.assertEqual(entry["objects_compared"], len(self._scene()), name)
        self.assertLess(scores[ENGINE]["max_position_error_cm"], 1e-6)
        self.assertGreater(scores[IDENTITY]["max_position_error_cm"], 100.0)
        self.assertGreater(scores[CAMERA]["max_position_error_cm"], 100.0)
        # The orientation comparison is anchored to the file's own convention,
        # so it is a property of the handoff and not of the scored candidate.
        orientations = {entry["max_orientation_error_deg"] for entry in scores.values()}
        self.assertEqual(len(orientations), 1)
        self.assertLess(orientations.pop(), 1e-6)
        camera_contract = tc["camera_contract"]
        self.assertEqual(camera_contract["candidate"], CAMERA)
        self.assertGreater(camera_contract["max_position_error_cm"], 100.0)
        self.assertFalse(camera_contract["within_tolerance"])
        self.assertEqual(len(camera_contract["matrix"]), 9)
        self.assertTrue(camera_contract["note"])

    def test_a_scene_in_the_engine_map_names_the_engine_candidate(self):
        scene = self._scene()
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        self.assertIn("prototype", contract["note"])
        self.assertIn("does not correct", contract["note"])

    def test_the_classic_candidate_alone_is_not_the_engine_match(self):
        scene = self._scene()
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        scores = {entry["name"]: entry for entry in
                  result["transform_check"]["candidates"]}
        self.assertEqual(result["transform_check"]["best"], ENGINE)
        self.assertNotEqual(result["transform_check"]["best"], CLASSIC)
        self.assertGreater(scores[CLASSIC]["max_position_error_cm"], 100.0)

    def test_a_consistent_unknown_map_is_accepted_through_the_fit(self):
        # A signed permutation that is not one of the named candidates still
        # places every object exactly; the fit has to decide it.
        unknown = reference.matrix3((-1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0))
        scene = self._scene_in_map_values(unknown)
        # A convention the project never measured has no node frame factor, so
        # the orientation comparison reports itself unavailable instead of
        # comparing against the engine handoff's factor.
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene, frame=None)
        tc = result["transform_check"]
        self.assertEqual(tc["best"], reference.FITTED_CANDIDATE_NAME)
        self.assertEqual(tc["decided_by"], "fit")
        self.assertTrue(tc["matched"])
        self.assertEqual(tc["winner"]["source"], "fit")
        self.assertEqual(result["problems"], [])
        self.assertTrue(tc["fit"]["available"] and tc["fit"]["signed_permutation"])
        self.assertLess(tc["fit"]["max_position_error_cm"], 1e-6)
        self.assertFalse(tc["orientation_available"])
        for entry in result["objects"]:
            self.assertFalse(entry["orientation_checked"], entry["node_name"])
            self.assertLess(entry["position_error_cm"], 1e-6, entry["node_name"])
            self.assertLess(entry["size_error_cm"], 1e-6, entry["node_name"])
            self.assertEqual(entry["problems"], [])
        for name, score in {entry["name"]: entry for entry in tc["candidates"]}.items():
            self.assertGreater(score["max_position_error_cm"], 100.0, name)

    def test_the_fit_may_not_decide_when_it_is_not_a_signed_permutation(self):
        # A handoff whose positions follow a map with a scale in it: the fit can
        # explain them, but not as a signed permutation, so no map is reported
        # as the match and every object keeps its mismatch.
        scene = self._scene_in_map_values(
            reference.matrix3((1.1, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0)))
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene, frame=None)
        tc = result["transform_check"]
        self.assertNotEqual(tc["best"], reference.FITTED_CANDIDATE_NAME)
        self.assertFalse(tc["matched"])
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertTrue(result["problems"])

    def test_an_object_whitelists_its_measured_values(self):
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
        rock = [entry for entry in result["objects"] if entry["node_name"] == "SM_Rock"][0]
        self.assertFalse(rock["orientation_checked"])
        self.assertIsNone(rock["orientation_error_deg"])
        self.assertLess(rock["position_error_cm"], 1e-6)

    def test_a_wrong_position_is_reported_with_both_values(self):
        scene = self._scene()
        camera = candidate_matrix(ENGINE)
        # Move the manifest's Unreal position 25 cm along the object's maya -Z.
        offset = reference.matrix3_inverse(camera)
        delta = reference.matrix3_apply(offset, (0.0, 0.0, -25.0))
        position = reference.matrix4_translation(scene[0]["ue_matrix"])
        scene[0]["ue_matrix"] = reference.matrix4_from_linear_and_translation(
            reference.matrix4_linear(scene[0]["ue_matrix"]),
            tuple(position[axis] + delta[axis] for axis in range(3)))
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene())
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertFalse(tc["matched"])
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertNotEqual(tc["best"], reference.FITTED_CANDIDATE_NAME)
        for entry in tc["candidates"]:
            self.assertGreater(entry["max_centroid_error_cm"], 1.0, entry["name"])

    def test_a_manifest_without_centroids_reports_the_check_unavailable(self):
        scene = self._scene()
        for entry in scene:
            entry["ue_centroid"] = None
            entry["maya_centroid"] = None
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        problems = " ".join(result["problems"])
        self.assertIn("OFFSET_MISMATCH", problems)
        self.assertIn("SM_Wedge", problems)
        self.assertFalse(result["transform_check"]["matched"])
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Wedge"][0]
        close(self, entry["offset_error_cm"],
              reference.vector_length([2 * value for value in wedge["maya_offset"]]),
              places=6)
        self.assertEqual(entry["measured_offset_cm"]["x"], 50.0)
        self.assertLess(entry["position_error_cm"], 1e-6)
        self.assertLess(entry["size_error_cm"], 1e-6)

    def test_the_offset_is_checked_where_the_orientation_is_skipped(self):
        # The wedge's local extents are all 100, so its orientation cannot be
        # measured; the offset still can, and a mirror still shows up.
        scene = self._scene()
        wedge = [entry for entry in scene if entry["node_name"] == "SM_Wedge"][0]
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        entry = [item for item in result["objects"] if item["node_name"] == "SM_Wedge"][0]
        self.assertFalse(entry["orientation_checked"])
        self.assertIsNotNone(entry["offset_error_cm"])
        self.assertLess(entry["offset_error_cm"], 1e-6)
        wedge["maya_offset"] = [wedge["maya_offset"][0], -wedge["maya_offset"][1],
                                wedge["maya_offset"][2]]
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        tc = result["transform_check"]
        self.assertFalse(tc["matched"])
        self.assertGreater(tc["max_offset_error_cm"], 1.0)
        self.assertEqual(tc["decided_by"], "candidate")
        self.assertNotEqual(tc["best"], reference.FITTED_CANDIDATE_NAME)
        self.assertTrue(any("OFFSET_MISMATCH" in problem for problem in result["problems"]))

    def test_a_wrong_size_is_reported(self):
        scene = self._scene()
        scene[2]["maya_size"] = [400.0, 20.0, 360.0]
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        problems = " ".join(result["problems"])
        self.assertIn("SIZE_MISMATCH", problems)
        self.assertIn("SM_Wall", problems)
        close(self, result["objects"][2]["size_error_cm"], 60.0, places=6)

    def test_a_wrong_orientation_is_reported(self):
        scene = self._scene()
        # The wall's own axes are turned a further 90 degrees about Maya's up
        # axis: X goes to -Z and Z goes to X, while the position is unchanged.
        expected = reference.matrix4_linear(
            reference.expected_node_matrix(candidate_matrix(ENGINE), scene[2]["ue_matrix"]))
        rotated = [tuple(-value for value in expected[2]), expected[1], expected[0]]
        scene[2]["maya_matrix"] = reference.matrix4_from_linear_and_translation(
            reference.matrix3(rotated), reference.matrix4_translation(scene[2]["maya_matrix"]))
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
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
            position = reference.matrix4_translation(entry["ue_matrix"])
            entry["ue_matrix"] = reference.matrix4_from_linear_and_translation(
                reference.matrix4_linear(entry["ue_matrix"]),
                (position[0] + 5000.0, position[1], position[2]))
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        self.assertFalse(result["transform_check"]["matched"])
        self.assertEqual(len(result["problems"]), len(scene))
        for problem in result["problems"]:
            self.assertIn("POSITION_MISMATCH", problem)
        for entry in result["transform_check"]["candidates"]:
            self.assertGreater(entry["max_position_error_cm"], 1000.0, entry["name"])

    def test_a_handoff_without_a_measured_frame_reports_the_check_unavailable(self):
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, self._scene(),
                                              frame=None)
        check = result["transform_check"]
        self.assertFalse(check["orientation_available"])
        self.assertIsNone(check["node_frame_factor"])
        self.assertIn("unavailable", check["orientation_note"])
        self.assertTrue(check["matched"])
        for entry in result["objects"]:
            self.assertFalse(entry["orientation_checked"], entry["node_name"])

    def test_no_objects_compares_nothing(self):
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, [])
        check = result["transform_check"]
        self.assertFalse(check["matched"])
        self.assertIsNone(check["candidate"])
        self.assertEqual(check["objects_compared"], 0)
        self.assertEqual(result["objects"], [])
        self.assertEqual(result["problems"], [])
        self.assertFalse(check["fit"]["available"])

    def test_tolerances_can_be_tightened(self):
        scene = self._scene()
        position = reference.matrix4_translation(scene[0]["maya_matrix"])
        scene[0]["maya_matrix"] = reference.matrix4_from_linear_and_translation(
            reference.matrix4_linear(scene[0]["maya_matrix"]),
            (position[0] + 0.5, position[1], position[2]))
        relaxed = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        self.assertTrue(relaxed["transform_check"]["matched"])
        strict = reference.compare_measurements(reference.AXIS_CANDIDATES, scene,
                                              tolerances={"position_cm": 0.1})
        self.assertFalse(strict["transform_check"]["matched"])
        self.assertIn("POSITION_MISMATCH", " ".join(strict["problems"]))

    def test_two_objects_on_one_node_are_reported_as_shared(self):
        scene = self._scene()
        scene[1]["path"] = scene[0]["path"]
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        self.assertTrue(any("SHARED_NODE" in warning for warning in result["warnings"]))

    def test_manifest_field_problems_reach_the_report(self):
        scene = self._scene()
        scene[0]["problems"] = ["SM_Pillar: manifest object has no world_matrix"]
        result = reference.compare_measurements(reference.AXIS_CANDIDATES, scene)
        self.assertTrue(any("MANIFEST_FIELD" in problem for problem in result["problems"]))
def frozen_budget():
    """The frozen scene-reference budgets, as the panel passes them."""
    return {"bytes": reference.MAX_REFERENCE_BYTES,
            "objects": reference.MAX_REFERENCE_OBJECTS,
            "triangles": reference.MAX_REFERENCE_TRIANGLES}


class FakeReferenceNode(object):
    """One node of the fake scene: a transform with attributes and a UUID."""

    def __init__(self, namespace, short, node_type="transform"):
        self.namespace = namespace or ""
        self.short = short
        self.type = node_type
        self.parent = None
        self.children = []
        self.attributes = {}
        self.matrix = [1.0, 0.0, 0.0, 0.0,
                       0.0, 1.0, 0.0, 0.0,
                       0.0, 0.0, 1.0, 0.0,
                       0.0, 0.0, 0.0, 1.0]
        self.bounds = (0.0, 0.0, 0.0, 10.0, 20.0, 30.0)
        self.uuid = "b7" + "{0:032x}".format(id(self))

    @property
    def full_name(self):
        return "{0}:{1}".format(self.namespace, self.short) if self.namespace else self.short

    @property
    def long_name(self):
        parts = []
        node = self
        while node is not None:
            parts.append(node.full_name)
            node = node.parent
        return "|" + "|".join(reversed(parts))


class FakeReferenceScene(object):
    """An in-memory Maya scene: namespaces, transforms, attributes, UUIDs.

    The fake implements only the surface ``mtou_livelink.reference`` uses, but it
    implements it structurally: a rename moves no node and keeps its UUID, a
    namespace holds exactly the nodes whose name carries it, and a long path is
    the DAG parent chain a node really has. That is what makes the staged swap,
    the rollback and the interrupted-run recovery measurable without Maya.
    """

    def __init__(self, imported_nodes=("SM_Wall",)):
        self.namespaces = set([":"])
        self.current_namespace = ":"
        self.nodes = []
        self.imported_nodes = list(imported_nodes)
        self.imports = []
        self.scene_at_import = []
        self.fail_step = None
        self.cmds = _FakeCommands(self)
        self.om = _FakeOpenMaya()

    def add_node(self, namespace, short, node_type="transform", parent=None):
        """Create a node, deduplicating its short name the way Maya does."""
        taken = [node for node in self.nodes
                 if node.namespace == (namespace or "") and node.short == short]
        if taken:
            short = "{0}{1}".format(short, len(taken) + 1)
        node = FakeReferenceNode(namespace, short, node_type)
        if parent is not None:
            node.parent = parent
            parent.children.append(node)
        self.nodes.append(node)
        return node

    def find(self, path):
        """The node a long name, a full name, a short name or a UUID names."""
        for node in self.nodes:
            if path in (node.long_name, node.full_name, node.uuid):
                return node
        for node in self.nodes:
            if path == node.short:
                return node
        return None

    def namespace_members(self, namespace):
        return [node for node in self.nodes if node.namespace == (namespace or "")]

    def snapshots(self):
        """``(long name, uuid)`` of every node, for before/after comparisons."""
        return [(node.long_name, node.uuid) for node in self.nodes]

    def mark(self, node, value):
        node.attributes["mtouSceneRefContainer"] = value


class _FakeOpenMaya(object):
    """The ``maya.api.OpenMaya`` surface; the tests never read a mesh."""

    class _Unavailable(object):
        def __getattr__(self, name):
            raise AssertionError("the fake scene has no geometry to read: "
                                 "{0}".format(name))

    class MSelectionList(_Unavailable):
        pass

    class MFnMesh(_Unavailable):
        pass

    class MSpace(object):
        kWorld = 4

    MVector = _Unavailable


class _FakeCommands(object):
    """The ``maya.cmds`` surface of one :class:`FakeReferenceScene`."""

    def __init__(self, scene):
        self.scene = scene

    def find(self, path):
        return self.scene.find(path)

    # ------------------------------------------------------------- session

    def about(self, **flags):
        return "2024" if flags.get("version") else False

    def currentUnit(self, **flags):
        if flags.get("linear"):
            return "cm"
        if flags.get("time"):
            return "film"
        return None

    def upAxis(self, **flags):
        return "y"

    def loadPlugin(self, name, **flags):
        return name

    def pluginInfo(self, name, **flags):
        return "2020.3.4"

    def playbackOptions(self, **flags):
        if flags.get("query"):
            return 1.0 if flags.get("minTime") else 120.0
        return None

    def currentTime(self, **flags):
        return 1.0 if flags.get("query") else None

    # --------------------------------------------------------- namespaces

    def namespaceInfo(self, *args, **flags):
        if flags.get("currentNamespace"):
            return self.scene.current_namespace
        if flags.get("listOnlyNamespaces"):
            return sorted(self.scene.namespaces)
        namespace = args[0] if args else ""
        nodes = [node for node in self.scene.nodes
                 if node.namespace == namespace
                 or (flags.get("recurse") and node.namespace.startswith(namespace + ":"))]
        return [node.long_name for node in nodes]

    def namespace(self, **flags):
        scene = self.scene
        if flags.get("exists") is not None:
            return flags["exists"] in scene.namespaces
        if "add" in flags:
            scene.namespaces.add(flags["add"])
            return None
        if "set" in flags:
            scene.current_namespace = flags["set"]
            return None
        if "rename" in flags:
            old, new = flags["rename"]
            if old not in scene.namespaces or new in scene.namespaces:
                raise RuntimeError("cannot rename namespace {0} to {1}".format(old, new))
            scene.namespaces.discard(old)
            scene.namespaces.add(new)
            for node in scene.namespace_members(old):
                node.namespace = new
            return None
        if "removeNamespace" in flags:
            old = flags["removeNamespace"]
            if flags.get("deleteNamespaceContent"):
                for node in list(scene.namespace_members(old)):
                    self._delete(node)
            scene.namespaces.discard(old)
            if scene.current_namespace == old:
                scene.current_namespace = ":"
            return None
        raise AssertionError("unsupported namespace flags: {0}".format(sorted(flags)))

    def _delete(self, node):
        if node.parent is not None:
            node.parent.children = [child for child in node.parent.children
                                    if child is not node]
        for child in list(node.children):
            self._delete(child)
        self.scene.nodes = [entry for entry in self.scene.nodes if entry is not node]

    # ------------------------------------------------------------- queries

    def ls(self, *args, **flags):
        nodes = list(self.scene.nodes)
        pattern = args[0] if args else None
        if pattern and pattern.endswith(":*"):
            prefix = pattern[:-2]
            nodes = [node for node in nodes
                     if node.full_name.startswith(prefix + ":")]
        elif pattern and pattern != "*":
            nodes = [node for node in nodes
                     if pattern in (node.long_name, node.full_name, node.uuid,
                                    node.short)]
        node_type = flags.get("type")
        if node_type:
            nodes = [node for node in nodes if node.type == node_type]
        if flags.get("uuid"):
            return [node.uuid for node in nodes]
        if flags.get("long"):
            return [node.long_name for node in nodes]
        return [node.short for node in nodes]

    def objExists(self, name):
        return self.find(name) is not None

    def nodeType(self, path):
        node = self.find(path)
        return node.type if node is not None else None

    def attributeQuery(self, attribute, **flags):
        node = self.find(flags.get("node"))
        exists = bool(node is not None and attribute in node.attributes)
        if flags.get("exists") is True:
            return exists
        if flags.get("exists") is False:
            return not exists
        raise AssertionError("unsupported attributeQuery flags")

    def getAttr(self, name):
        path, _, attribute = name.rpartition(".")
        node = self.find(path)
        return node.attributes.get(attribute) if node is not None else None

    def setAttr(self, name, *values, **flags):
        path, _, attribute = name.rpartition(".")
        node = self.find(path)
        if node is None:
            raise RuntimeError("no node named {0}".format(path))
        node.attributes[attribute] = list(values) if len(values) > 1 else values[0]
        return None

    def addAttr(self, node_path, **flags):
        node = self.find(node_path)
        if node is None:
            raise RuntimeError("no node named {0}".format(node_path))
        node.attributes.setdefault(flags.get("longName"), None)
        return None

    def listConnections(self, name, **flags):
        del name, flags
        return []

    def isConnected(self, first, second):
        del first, second
        return False

    def listRelatives(self, path, **flags):
        found = []
        node = self.find(path)
        if node is None:
            return found
        if flags.get("parent") and node.parent is not None:
            found.append(node.parent.long_name if flags.get("fullPath")
                         else node.parent.short)
        if flags.get("children"):
            for child in node.children:
                found.append(child.long_name if flags.get("fullPath")
                             else child.short)
        if flags.get("allDescendents"):
            pending = list(node.children)
            while pending:
                child = pending.pop(0)
                found.append(child.long_name if flags.get("fullPath") else child.short)
                pending.extend(child.children)
        return found

    def xform(self, path, **flags):
        node = self.find(path)
        if node is None:
            raise RuntimeError("no node named {0}".format(path))
        if flags.get("query"):
            return list(node.matrix)
        if "matrix" in flags:
            node.matrix = [float(value) for value in flags["matrix"]]
        return None

    def exactWorldBoundingBox(self, *targets):
        for target in targets:
            node = self.find(target)
            if node is not None and node.bounds is not None:
                return list(node.bounds)
        return [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]

    def createNode(self, node_type, **flags):
        node = self.scene.add_node(self.scene.current_namespace,
                                   flags.get("name") or node_type, node_type)
        return node.full_name

    def parent(self, child, parent, **flags):
        del flags
        node = self.find(child)
        target = self.find(parent)
        if node is None or target is None:
            raise RuntimeError("cannot parent {0} under {1}".format(child, parent))
        if node.parent is not None:
            node.parent.children = [entry for entry in node.parent.children
                                    if entry is not node]
        node.parent = target
        target.children.append(node)
        return node.long_name

    def rename(self, old, new):
        node = self.find(old)
        if node is None:
            raise RuntimeError("no node named {0}".format(old))
        node.short = new.split(":", 1)[-1]
        return node.long_name

    def delete(self, *paths):
        for path in paths:
            node = self.find(path)
            if node is not None:
                self._delete(node)
        return None

    def shadingNode(self, node_type, **flags):
        return self.createNode(node_type, **flags)

    def sets(self, **flags):
        del flags
        return "MtoU_UE_SceneRef_GraySG"

    def connectAttr(self, first, second, **flags):
        del first, second, flags
        return None

    def file(self, *args, **flags):
        """Import one handoff into the current namespace, as ``fbxmaya`` does.

        The fake records the scene as the importer sees it at import time, which
        is what proves the previous reference was untouched while the new one was
        being measured.
        """
        scene = self.scene
        scene.scene_at_import.append(scene.snapshots())
        namespace = flags.get("ns")
        created = []
        for name in scene.imported_nodes:
            node = scene.add_node(namespace, name)
            created.append(node.full_name)
            # fbxmaya creates every mesh as a transform with its shape below.
            created.append(scene.add_node(
                namespace, name + "Shape", "mesh", parent=node).full_name)
        scene.imports.append({"namespaces": set(scene.namespaces),
                              "nodes": len(scene.nodes)})
        if scene.fail_step == "import":
            raise RuntimeError("the fake FBX plugin refused the file")
        return created


class FakeBindings(object):
    """Bind the fake scene's ``cmds``/``om`` into the module under test."""

    def __init__(self, scene):
        self.scene = scene
        self._cmds = None
        self._om = None

    def __enter__(self):
        self._cmds = reference.cmds
        self._om = reference.om
        reference.cmds = self.scene.cmds
        reference.om = self.scene.om
        return self.scene

    def __exit__(self, *exception):
        reference.cmds = self._cmds
        reference.om = self._om
        return False


def mark_for(name):
    """The ownership mark this tool writes for the namespace ``name``."""
    return "{0} {1}".format(reference.REFERENCE_MARK_VALUE, name)


def record_inventory(scene, group):
    """Record on ``group`` every node its import created, as the tool does."""
    nodes = scene.namespace_members(group.namespace)
    pending = list(group.children)
    while pending:
        node = pending.pop(0)
        nodes.append(node)
        pending.extend(node.children)
    group.attributes["mtouSceneRefNodes"] = " ".join(
        sorted(set(node.uuid for node in nodes + [group])))


def owned_scene(scene, container=None, node_name=None):
    """A container this tool owns: one marked group, optionally with content."""
    container = container or reference.REFERENCE_CONTAINER
    scene.add_node("", "Some", "transform")
    scene.namespaces.add(container)
    scene.current_namespace = container
    group = scene.add_node(container, container, "transform")
    scene.current_namespace = ":"
    scene.mark(group, mark_for(container))
    content = None
    if node_name is not None:
        content = scene.add_node(container, node_name, "transform", parent=group)
    record_inventory(scene, group)
    return group, content


def staging_scene(scene, staging_name):
    """A staging namespace an interrupted run left behind, carrying its mark."""
    scene.namespaces.add(staging_name)
    scene.current_namespace = staging_name
    group = scene.add_node(staging_name, staging_name, "transform")
    scene.current_namespace = ":"
    scene.mark(group, mark_for(staging_name))
    record_inventory(scene, group)
    return group


class HandoffFixture(object):
    """One synthetic handoff directory: a manifest, an FBX file and its digest."""

    def __init__(self, directory, triangles=120, anchor=None):
        self.directory = pathlib.Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        manifest = {
            "schema": reference.MANIFEST_SCHEMA,
            "world": {"name": "Lvl", "up_axis": "Z", "linear_unit": "cm"},
            "conventions": {"export_axis_option": "bForceFrontXAxis=false",
                            "engine_to_maya_point_map":
                                reference.ENGINE_HANDOFF_CANDIDATE},
            "scope": {"level": "/Game/Lvl", "completeness": "confirmed"},
            "counts": {"objects": 1, "triangles": triangles, "bytes": 2048},
            "files": {"geometry": "Lvl.fbx", "geometry_bytes": 2048,
                      "image_data": False},
        }
        if anchor is not None:
            manifest["world"]["anchor"] = anchor
        self.manifest_text = json.dumps(manifest, indent=2)
        (self.directory / "Lvl.manifest.json").write_bytes(
            self.manifest_text.encode("utf-8"))
        payload = ("; FBX 7.7.0 project file\n"
                   "Objects:  {\n"
                   "\tModel: 77, \"Model::SM_Wall\", \"Mesh\" {\n"
                   "\t}\n"
                   "}\n")
        (self.directory / "Lvl.fbx").write_bytes(payload.encode("utf-8"))
        self.digest = hashlib.md5(
            (self.manifest_text + hashlib.md5(
                (self.directory / "Lvl.fbx").read_bytes()).hexdigest()).encode("utf-8")
        ).hexdigest()
        self.handoff = {"transfer_id": "tx-ref-1", "directory": str(self.directory),
                        "digest": self.digest, "space_revision": 3,
                        "scope": {"level": "/Game/Lvl", "sublevels": []},
                        "counts": {"objects": 1, "triangles": triangles,
                                   "bytes": 2048}}


def import_fixture(scene, fixture, budget=None):
    """Import ``fixture`` into ``scene`` with the module's Maya names bound."""
    with FakeBindings(scene):
        return reference.import_handoff(fixture.handoff, budget or frozen_budget())


class HandoffValidationTest(unittest.TestCase):
    """What :func:`reference.inspect` accepts and refuses, without Maya."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_validate_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def test_a_good_handoff_is_accepted_with_its_report(self):
        fixture = HandoffFixture(self.directory / "handoff")
        report = reference.inspect(fixture.handoff, frozen_budget())
        self.assertTrue(report["accepted"], report["problems"])
        self.assertEqual("the handoff passed validation", report["detail"])
        self.assertEqual({"objects": 1, "triangles": 120,
                          "bytes": (fixture.directory / "Lvl.manifest.json").stat().st_size
                          + (fixture.directory / "Lvl.fbx").stat().st_size},
                         report["counts"])
        self.assertEqual("/Game/Lvl", report["scope"]["level"])
        self.assertTrue(report["digest"]["matched"])
        self.assertEqual("manifest+geometry_md5-md5", report["digest"]["algorithm"])

    def test_a_product_manifest_with_chinese_names_is_accepted(self):
        # The exporter writes UTF-8 without a BOM and hashes those bytes followed
        # by the geometry bytes; a non-ANSI actor label must still match.
        fixture = HandoffFixture(self.directory / "handoff")
        manifest = json.loads(fixture.manifest_text)
        manifest["objects"] = [{"node_name": "测试石块", "actor": "测试石块",
                                "exported": True}]
        manifest_bytes = json.dumps(manifest, ensure_ascii=False).encode("utf-8")
        (fixture.directory / "Lvl.manifest.json").write_bytes(manifest_bytes)
        digest = hashlib.md5(
            manifest_bytes + (fixture.directory / "Lvl.fbx").read_bytes()).hexdigest()
        report = reference.inspect(dict(fixture.handoff, digest=digest),
                                   frozen_budget())
        self.assertTrue(report["accepted"], report["problems"])
        self.assertTrue(report["digest"]["matched"])

    def test_a_handoff_without_a_space_revision_is_stale(self):
        fixture = HandoffFixture(self.directory / "handoff")
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(dict(fixture.handoff, space_revision=None),
                              frozen_budget())
        self.assertEqual("REFERENCE_STALE", caught.exception.code)

    def test_a_handoff_of_an_older_space_is_stale(self):
        fixture = HandoffFixture(self.directory / "handoff")
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(dict(fixture.handoff, session_space_revision=11),
                              frozen_budget())
        self.assertEqual("REFERENCE_STALE", caught.exception.code)

    def test_a_changed_directory_is_a_digest_mismatch(self):
        fixture = HandoffFixture(self.directory / "handoff")
        (fixture.directory / "Lvl.fbx").write_bytes(b"; FBX 7.7.0 changed\n")
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(fixture.handoff, frozen_budget())
        self.assertEqual("REFERENCE_DIGEST_MISMATCH", caught.exception.code)

    def test_an_empty_digest_is_a_digest_mismatch(self):
        fixture = HandoffFixture(self.directory / "handoff")
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(dict(fixture.handoff, digest=""), frozen_budget())
        self.assertEqual("REFERENCE_DIGEST_MISMATCH", caught.exception.code)

    def test_image_data_in_the_directory_is_refused(self):
        fixture = HandoffFixture(self.directory / "handoff")
        (fixture.directory / "baked.png").write_bytes(b"not an image")
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(fixture.handoff, frozen_budget())
        self.assertEqual("REFERENCE_IMAGE_DATA", caught.exception.code)
        self.assertIn("baked.png", caught.exception.detail)

    def test_an_over_budget_handoff_is_refused(self):
        fixture = HandoffFixture(self.directory / "handoff", triangles=900000)
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.inspect(fixture.handoff,
                              dict(frozen_budget(), triangles=200000))
        self.assertEqual("REFERENCE_OVER_BUDGET", caught.exception.code)

    def test_the_corpus_sha256_spelling_of_the_same_content_matches(self):
        fixture = HandoffFixture(self.directory / "handoff")
        payload = (fixture.manifest_text + hashlib.md5(
            (fixture.directory / "Lvl.fbx").read_bytes()).hexdigest()).encode("utf-8")
        report = reference.inspect(
            dict(fixture.handoff, digest="sha256:" + hashlib.sha256(payload).hexdigest()),
            frozen_budget())
        self.assertTrue(report["accepted"], report["problems"])

    def test_a_missing_directory_is_reported_not_raised(self):
        fixture = HandoffFixture(self.directory / "handoff")
        report = reference.inspect(dict(fixture.handoff,
                                        directory=str(self.directory / "gone")),
                                   frozen_budget())
        self.assertFalse(report["accepted"])
        self.assertIn("HANDOFF_DIRECTORY_ABSENT", report["problems"][0])

    def test_the_default_budgets_are_the_frozen_ones(self):
        self.assertEqual({"bytes": reference.MAX_REFERENCE_BYTES,
                          "objects": reference.MAX_REFERENCE_OBJECTS,
                          "triangles": reference.MAX_REFERENCE_TRIANGLES},
                         frozen_budget())


class StagedImportTest(unittest.TestCase):
    """The staged import: measure in a free namespace, then take the name."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_import_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def test_a_first_import_stages_and_takes_the_container_name(self):
        scene = FakeReferenceScene()
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        self.assertEqual("finalise", report["phase"])
        self.assertTrue(report["container"]["swapped"])
        container = reference.REFERENCE_CONTAINER
        self.assertIn(container, scene.namespaces)
        self.assertEqual("|{0}:{1}".format(container, container),
                         report["container"]["group_path"])
        group = scene.find(report["container"]["group_path"])
        self.assertEqual("|{0}:{0}".format(container), group.long_name)
        self.assertEqual(mark_for(container),
                         group.attributes["mtouSceneRefContainer"])
        self.assertEqual(["SM_Wall"], [node.short for node in group.children])
        self.assertNotIn(container + reference.STAGING_SUFFIX, scene.namespaces)
        self.assertEqual(1, report["counts"]["objects"])
        self.assertEqual(120, report["counts"]["triangles"])
        self.assertTrue(report["counts"]["bytes"] > 0)
        self.assertFalse(report["rolled_back"])
        self.assertFalse(report["discarded"])
        self.assertEqual([], report["residual"]["nodes"])

    def test_a_manifest_without_objects_imports_and_reports_the_check_unavailable(self):
        scene = FakeReferenceScene()
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        self.assertTrue(any("MANIFEST_OBJECTS_ABSENT" in warning
                            for warning in report["warnings"]))

    def test_the_previous_reference_is_untouched_while_the_new_one_is_measured(self):
        scene = FakeReferenceScene()
        group, content = owned_scene(scene, node_name="SM_Old")
        old_uuid = group.uuid
        before = scene.snapshots()
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        # The import itself saw the previous reference in place, unchanged, and no
        # staging namespace had touched it yet.
        (seen,) = scene.scene_at_import
        before_paths = {path for path, _uuid in before}
        for entry in before:
            self.assertIn(entry, seen)
        added = sorted(path for path, _uuid in seen if path not in before_paths)
        container = reference.REFERENCE_CONTAINER
        staging = container + reference.STAGING_SUFFIX
        self.assertEqual(["|{0}:{0}".format(staging)], added)
        # The takeover moved the staged group into the container name and deleted
        # the retired copy, so the very node found before is gone.
        self.assertIsNone(scene.find(old_uuid))
        taken = scene.find("|{0}:{0}".format(container))
        self.assertIsNot(group, taken)
        self.assertEqual("SM_Wall", taken.children[0].short)
        self.assertNotIn(container + reference.RETIRING_SUFFIX, scene.namespaces)

    def test_a_repeat_import_replaces_only_the_tool_owned_container(self):
        scene = FakeReferenceScene()
        owned_scene(scene, node_name="SM_Old")
        first = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(first["accepted"], first["problems"])
        other = scene.add_node("", "ProductionObject", "transform")
        scene.namespaces.add("Production")
        kept = scene.add_node("Production", "Kept", "transform")
        second = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(second["accepted"], second["problems"])
        container = reference.REFERENCE_CONTAINER
        self.assertIs(other, scene.find("|ProductionObject"))
        self.assertIs(kept, scene.find("|Production:Kept"))
        self.assertEqual("|{0}:{0}".format(container),
                         second["container"]["group_path"])
        group = scene.find("|{0}:{0}".format(container))
        self.assertEqual(["SM_Wall"], [node.short for node in group.children])
        self.assertNotIn(container + reference.RETIRING_SUFFIX, scene.namespaces)
        self.assertNotIn(container + reference.STAGING_SUFFIX, scene.namespaces)
        # The group, the imported transform and its mesh shape.
        self.assertEqual(3, len(scene.namespace_members(container)))

    def test_the_staging_namespace_is_the_one_the_report_names(self):
        scene = FakeReferenceScene()
        owned_scene(scene, node_name="SM_Old")
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        container = reference.REFERENCE_CONTAINER
        staging = container + reference.STAGING_SUFFIX
        self.assertEqual(staging, report["update"]["staging_namespace"])
        self.assertNotIn(staging, scene.namespaces)
        self.assertEqual("|{0}:{0}".format(container),
                         scene.find("{0}:{0}".format(container)).long_name)


def ue_anchor(location, roll, pitch, yaw):
    """The manifest's ``world.anchor`` for an Unreal placement."""
    return {"location_cm": list(location),
            "rotation_deg": {"roll": roll, "pitch": pitch, "yaw": yaw}}


def ue_axes(roll, pitch, yaw):
    """Forward, right and up of an Unreal FRotator, from first principles."""
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return ([cp * cy, cp * sy, sp],
            [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
            [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp])


#: The D003 acceptance fixture's anchor: (60, 40, 0), roll 3, pitch -4, yaw 37.
D003_ANCHOR = ue_anchor((60.0, 40.0, 0.0), 3.0, -4.0, 37.0)
D003_AXES = ue_axes(3.0, -4.0, 37.0)


def to_anchor_local(anchor, point):
    """``A^-1 . p`` in Unreal axes: the anchor's axes dotted with ``p - t``."""
    shifted = [point[index] - anchor["location_cm"][index] for index in range(3)]
    return [sum(axis[index] * shifted[index] for index in range(3))
            for axis in D003_AXES]


def row_vector_times(matrix, point):
    """A point carried by a row-major, translation-in-fourth-row Maya matrix."""
    return [sum(point[row] * matrix[row * 4 + column] for row in range(3))
            + matrix[12 + column] for column in range(3)]


class AnchorTest(unittest.TestCase):
    """The reference follows the character and the camera into the anchor frame."""

    def test_a_manifest_without_an_anchor_names_none(self):
        self.assertEqual((None, None), reference.parse_anchor(None))

    def test_a_rigid_right_handed_anchor_is_accepted(self):
        anchor, problem = reference.parse_anchor(D003_ANCHOR)
        self.assertIsNone(problem)
        self.assertEqual((60.0, 40.0, 0.0), anchor["location_cm"])
        for axis, expected in zip(anchor["axes"], D003_AXES):
            for index in range(3):
                self.assertAlmostEqual(expected[index], axis[index], places=9)

    def test_a_malformed_anchor_names_its_problem(self):
        bad = {
            "not an object": [1, 2, 3],
            "missing rotation": {"location_cm": [0, 0, 0]},
            "missing location": {"rotation_deg": D003_ANCHOR["rotation_deg"]},
            "short location": dict(D003_ANCHOR, location_cm=[1.0, 0.0]),
            "not a number": dict(D003_ANCHOR, location_cm=["a", 0, 0]),
            "non finite": dict(D003_ANCHOR, location_cm=[float("nan"), 0, 0]),
            "partial rotation": dict(D003_ANCHOR, rotation_deg={"yaw": 1.0}),
            "non finite rotation": dict(
                D003_ANCHOR, rotation_deg={"roll": 0, "pitch": 0, "yaw": float("inf")}),
        }
        for name, value in bad.items():
            anchor, problem = reference.parse_anchor(value)
            self.assertIsNone(anchor, name)
            self.assertTrue(problem, name)

    def test_the_identity_anchor_leaves_the_container_alone(self):
        anchor, _problem = reference.parse_anchor(
            ue_anchor((0.0, 0.0, 0.0), 0.0, 0.0, 0.0))
        matrix = reference.anchor_container_matrix(anchor)
        for index, value in enumerate(
                [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]):
            self.assertAlmostEqual(value, matrix[index], places=9)

    def test_the_container_takes_every_reference_point_into_the_anchor_frame(self):
        anchor, _problem = reference.parse_anchor(D003_ANCHOR)
        matrix = reference.anchor_container_matrix(anchor)
        for point in ((0.0, 0.0, 0.0), (60.0, 40.0, 0.0), (500.0, -120.0, 35.0),
                      (-80.0, 260.0, 910.0)):
            imported = camera_module.ue_to_maya_point(*point)
            placed = row_vector_times(matrix, imported)
            expected = camera_module.ue_to_maya_point(
                *to_anchor_local(D003_ANCHOR, point))
            for index in range(3):
                self.assertAlmostEqual(expected[index], placed[index], places=6)

    def test_the_reference_stands_where_the_linked_camera_sees_it(self):
        # Unreal's camera is sent relative to the anchor and mapped through C,
        # so a prop 300 cm in front of the camera in world space must be 300 cm
        # in front of it after the container is placed too.
        anchor, _problem = reference.parse_anchor(D003_ANCHOR)
        matrix = reference.anchor_container_matrix(anchor)
        camera_world = (260.0, -40.0, 170.0)
        forward = (math.cos(math.radians(20.0)), math.sin(math.radians(20.0)), 0.0)
        prop_world = tuple(camera_world[index] + 300.0 * forward[index]
                           for index in range(3))
        camera_maya = camera_module.ue_to_maya_point(
            *to_anchor_local(D003_ANCHOR, camera_world))
        prop_maya = row_vector_times(
            matrix, camera_module.ue_to_maya_point(*prop_world))
        distance = math.sqrt(sum((prop_maya[index] - camera_maya[index]) ** 2
                                 for index in range(3)))
        self.assertAlmostEqual(300.0, distance, places=6)
        # The character stands at the anchor's origin: Maya (0, 0, 0).
        origin = row_vector_times(
            matrix, camera_module.ue_to_maya_point(60.0, 40.0, 0.0))
        for value in origin:
            self.assertAlmostEqual(0.0, value, places=6)


class AnchorDifferenceTest(unittest.TestCase):
    """A reference fits the linkage anchor within the common-anchor tolerances."""

    def test_the_same_placement_fits(self):
        self.assertIsNone(reference.anchor_difference(D003_ANCHOR, D003_ANCHOR))

    def test_a_shift_within_the_tolerance_fits(self):
        near = ue_anchor((60.005, 40.0, 0.0), 3.0, -4.0, 37.0)
        self.assertIsNone(reference.anchor_difference(D003_ANCHOR, near))

    def test_a_moved_target_is_named_with_both_placements(self):
        moved = ue_anchor((60.5, 40.0, 0.0), 3.0, -4.0, 37.0)
        problem = reference.anchor_difference(D003_ANCHOR, moved)
        self.assertIn("(60, 40, 0)", problem)
        self.assertIn("(60.5, 40, 0)", problem)
        self.assertIn("0.500 cm", problem)

    def test_a_turned_target_is_named(self):
        turned = ue_anchor((60.0, 40.0, 0.0), 3.0, -4.0, 37.05)
        self.assertIn("0.050 deg", reference.anchor_difference(D003_ANCHOR, turned))

    def test_an_unusable_anchor_on_either_side_does_not_fit(self):
        self.assertIn("records no usable anchor",
                      reference.anchor_difference({"location_cm": [0]}, D003_ANCHOR))
        self.assertIn("names no usable anchor",
                      reference.anchor_difference(D003_ANCHOR, None))


class AnchorImportTest(unittest.TestCase):
    """The staged import places the container once the comparison passed."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_anchor_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def imported(self, **fixture_extra):
        fixture = HandoffFixture(self.directory / "handoff", **fixture_extra)
        scene = FakeReferenceScene()
        report = import_fixture(scene, fixture)
        return scene, report

    def group_matrix(self, scene, report):
        return scene.find(report["container"]["group_path"]).matrix

    def test_the_container_is_placed_in_the_anchor_frame(self):
        scene, report = self.imported(anchor=D003_ANCHOR)
        self.assertTrue(report["accepted"], report["problems"])
        anchor, _problem = reference.parse_anchor(D003_ANCHOR)
        expected = reference.anchor_container_matrix(anchor)
        for index, value in enumerate(self.group_matrix(scene, report)):
            self.assertAlmostEqual(expected[index], value, places=9)
        self.assertEqual("manifest", report["anchor"]["source"])
        self.assertTrue(report["anchor"]["applied"])
        self.assertFalse(any(entry.startswith("ANCHOR_ABSENT")
                             for entry in report["warnings"]))

    def test_a_manifest_without_an_anchor_imports_at_the_origin_and_says_so(self):
        scene, report = self.imported()
        self.assertTrue(report["accepted"], report["problems"])
        self.assertEqual([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
                         [int(value) for value in self.group_matrix(scene, report)])
        self.assertEqual("absent", report["anchor"]["source"])
        self.assertFalse(report["anchor"]["applied"])
        self.assertTrue(any(entry.startswith("ANCHOR_ABSENT")
                            for entry in report["warnings"]))

    def recorded_anchor(self, scene, report):
        group = scene.find(report["container"]["group_path"])
        return json.loads(group.attributes[reference.REFERENCE_ANCHOR_ATTRIBUTE])

    def test_the_container_records_the_anchor_it_was_placed_for(self):
        scene, report = self.imported(anchor=D003_ANCHOR)
        self.assertTrue(report["accepted"], report["problems"])
        self.assertIsNone(reference.anchor_difference(
            self.recorded_anchor(scene, report), D003_ANCHOR))
        with FakeBindings(scene):
            self.assertIsNone(reference.stale_reference(D003_ANCHOR))
            moved = reference.stale_reference(
                ue_anchor((160.0, 40.0, 0.0), 3.0, -4.0, 37.0))
        self.assertIn("100.000 cm", moved)

    def test_a_reference_without_an_anchor_records_the_origin_frame(self):
        scene, report = self.imported()
        self.assertEqual(reference.IDENTITY_ANCHOR, self.recorded_anchor(scene, report))
        with FakeBindings(scene):
            self.assertIsNone(reference.stale_reference(
                ue_anchor((0.0, 0.0, 0.0), 0.0, 0.0, 0.0)))
            self.assertTrue(reference.stale_reference(D003_ANCHOR))

    def test_a_reference_imported_by_an_earlier_version_is_stale(self):
        scene, report = self.imported(anchor=D003_ANCHOR)
        group = scene.find(report["container"]["group_path"])
        del group.attributes[reference.REFERENCE_ANCHOR_ATTRIBUTE]
        with FakeBindings(scene):
            problem = reference.stale_reference(D003_ANCHOR)
        self.assertIn("records no anchor", problem)

    def test_no_reference_is_never_stale(self):
        with FakeBindings(FakeReferenceScene()):
            self.assertIsNone(reference.stale_reference(D003_ANCHOR))

    def test_a_malformed_anchor_refuses_the_import_and_changes_nothing(self):
        scene, report = self.imported(anchor=dict(D003_ANCHOR, location_cm=[1.0]))
        self.assertFalse(report["accepted"])
        self.assertEqual("refused", report["phase"])
        self.assertTrue(any("ANCHOR_INVALID" in entry for entry in report["problems"]))
        self.assertNotIn(reference.REFERENCE_CONTAINER, scene.namespaces)

    def test_a_dry_run_places_nothing(self):
        fixture = HandoffFixture(self.directory / "handoff", anchor=D003_ANCHOR)
        scene = FakeReferenceScene()
        with FakeBindings(scene):
            container = reference.ReferenceImporter(
                fixture.handoff, frozen_budget(), dry_run=True)
            report = container.run()
        self.assertFalse(report["anchor"]["applied"])
        self.assertNotIn(reference.REFERENCE_CONTAINER, scene.namespaces)


class SwapRollbackTest(unittest.TestCase):
    """A takeover that fails must delete its staging and put the previous back."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_rollback_")
        self.scene = FakeReferenceScene()
        owned_scene(self.scene, node_name="SM_Old")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def test_an_import_without_geometry_keeps_the_previous_reference(self):
        # fbxmaya reads an unreadable file as an empty scene.
        scene = FakeReferenceScene()
        _group, old = owned_scene(scene, node_name="SM_Old")
        scene.imported_nodes = []
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertFalse(report["accepted"])
        self.assertTrue(any("FBX_IMPORT_EMPTY" in entry for entry in report["problems"]),
                        report["problems"])
        self.assertIs(old, scene.find(old.long_name))
        self.assertNotIn(reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX,
                         scene.namespaces)

    def test_a_failed_read_back_rolls_the_swap_back(self):
        fixture = HandoffFixture(self.directory / "handoff")
        old = self.scene.find("|{0}:{0}".format(reference.REFERENCE_CONTAINER))
        old_uuid = old.uuid
        original = reference.ReferenceImporter._rehome_paths

        def broken(self):
            raise RuntimeError("the injected final-path read back failed")

        reference.ReferenceImporter._rehome_paths = broken
        try:
            report = import_fixture(self.scene, fixture)
        finally:
            reference.ReferenceImporter._rehome_paths = original
        container = reference.REFERENCE_CONTAINER
        self.assertFalse(report["accepted"])
        self.assertTrue(report["rolled_back"])
        self.assertTrue(report["discarded"])
        self.assertIn("STAGED_SWAP_FAILED", report["problems"][0])
        self.assertIn("group_renamed", report["detail"])
        restored = self.scene.find("|{0}:{0}".format(container))
        self.assertEqual(old_uuid, restored.uuid)
        self.assertEqual("SM_Old", restored.children[0].short)
        self.assertNotIn(container + reference.STAGING_SUFFIX, self.scene.namespaces)
        self.assertNotIn(container + reference.RETIRING_SUFFIX, self.scene.namespaces)
        self.assertEqual(2, len(self.scene.namespace_members(container)))
        rollback = report["update"]["rollback"]
        self.assertTrue(rollback["previous_reference_restored"])
        self.assertTrue(rollback["container_namespace_removed"])
        self.assertIn("SM_Wall", " ".join(rollback["container_nodes_removed"]))
        self.assertEqual(["SM_Old"], [node.short for node in restored.children])

    def test_a_failure_before_the_retire_keeps_the_reference_in_place(self):
        fixture = HandoffFixture(self.directory / "handoff")
        old = self.scene.find("|{0}:{0}".format(reference.REFERENCE_CONTAINER))
        original = reference._retire_container

        def broken(container, previous_state):
            raise RuntimeError("the injected retire failed")

        reference._retire_container = broken
        try:
            report = import_fixture(self.scene, fixture)
        finally:
            reference._retire_container = original
        self.assertFalse(report["accepted"])
        self.assertFalse(report["rolled_back"])
        self.assertIn("STAGED_SWAP_FAILED", report["problems"][0])
        restored = self.scene.find("|{0}:{0}".format(reference.REFERENCE_CONTAINER))
        self.assertIs(old, restored)
        self.assertEqual("SM_Old", restored.children[0].short)
        self.assertNotIn(reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX,
                         self.scene.namespaces)


class ForeignNodeRefusalTest(unittest.TestCase):
    """A name this tool cannot prove it owns is never deleted or renamed."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_foreign_")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def _run(self, scene):
        return import_fixture(scene, HandoffFixture(self.directory / "handoff"))

    def test_a_foreign_node_under_the_container_name_is_refused(self):
        scene = FakeReferenceScene()
        scene.namespaces.add(reference.REFERENCE_CONTAINER)
        foreign = scene.add_node(reference.REFERENCE_CONTAINER, "ProductionObject")
        before = scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._run(scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn("ProductionObject", caught.exception.detail)
        self.assertEqual(before, scene.snapshots())
        self.assertIs(foreign, scene.find("|{0}:ProductionObject".format(
            reference.REFERENCE_CONTAINER)))

    def test_a_scene_node_deep_under_the_container_refuses_the_update(self):
        # A person parented a production object under a reference mesh: it is in
        # the scene's own namespace, so deleting the old container would take it.
        scene = FakeReferenceScene()
        _group, mesh = owned_scene(scene, node_name="SM_Old")
        prop = scene.add_node("", "HeroProp", parent=mesh)
        before = scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._run(scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn("HeroProp", caught.exception.detail)
        self.assertEqual(before, scene.snapshots())
        self.assertIs(prop, scene.find(prop.long_name))

    def test_a_foreign_root_node_of_the_container_name_is_refused(self):
        scene = FakeReferenceScene()
        foreign = scene.add_node("", reference.REFERENCE_CONTAINER)
        before = scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._run(scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertEqual(before, scene.snapshots())
        self.assertIs(foreign, scene.find("|" + reference.REFERENCE_CONTAINER))

    def test_an_unmarked_namespace_of_the_container_name_is_refused(self):
        scene = FakeReferenceScene()
        container = reference.REFERENCE_CONTAINER
        scene.namespaces.add(container)
        before = scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._run(scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn(reference.REFERENCE_MARK_ATTRIBUTE, caught.exception.detail)
        self.assertEqual(before, scene.snapshots())


class RecoveryTest(unittest.TestCase):
    """What an interrupted run leaves behind, and what the next run does."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_recover_")
        self.container = reference.ReferenceContainer(
            container=reference.REFERENCE_CONTAINER, space_revision=3,
            budget=frozen_budget())

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def _recover(self, scene):
        with FakeBindings(scene):
            return self.container.recover()

    def test_a_stale_staging_namespace_is_swept_before_the_next_import(self):
        scene = FakeReferenceScene()
        staging_scene(scene, reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX)
        other = scene.add_node("", "Kept", "transform")
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        staging = reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX
        self.assertEqual([staging], report["update"]["stale_staging_namespaces"])
        self.assertTrue(report["update"]["stale_staging_removed"])
        self.assertNotIn(staging, scene.namespaces)
        self.assertIs(other, scene.find("|Kept"))
        self.assertTrue(any("STALE_STAGING_REMOVED" in warning
                            for warning in report["warnings"]))

    def test_a_staging_namespace_with_foreign_nodes_is_left_alone(self):
        scene = FakeReferenceScene()
        staging = reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX
        staging_scene(scene, staging)
        kept = scene.add_node(staging, "ForeignNode")
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        recovery = report["update"]["recovery"]
        self.assertEqual([], recovery["stale_staging_namespaces"])
        self.assertEqual(1, len(recovery["partial_cleanups"]))
        self.assertIs(kept, scene.find("|{0}:ForeignNode".format(staging)))

    def test_a_staging_namespace_with_unrecorded_nodes_is_kept_whole(self):
        # A DG node, or a node in a child namespace, would go with the namespace.
        for namespace, node_type in (("", "lambert"), (":Props", "transform")):
            scene = FakeReferenceScene()
            staging = reference.REFERENCE_CONTAINER + reference.STAGING_SUFFIX
            group = staging_scene(scene, staging)
            scene.namespaces.add(staging + namespace)
            kept = scene.add_node(staging + namespace, "ArtistNode", node_type)
            report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
            self.assertTrue(report["accepted"], report["problems"])
            recovery = report["update"]["recovery"]
            self.assertEqual([], recovery["stale_staging_namespaces"])
            self.assertEqual(1, len(recovery["partial_cleanups"]))
            self.assertIn("ArtistNode", " ".join(recovery["events"]))
            self.assertIs(kept, scene.find(kept.uuid))
            self.assertIs(group, scene.find(group.uuid))

    def test_an_interrupted_takeover_is_finished_on_the_next_run(self):
        scene = FakeReferenceScene()
        container = reference.REFERENCE_CONTAINER
        scene.namespaces.add(container)
        scene.current_namespace = container
        staged = scene.add_node(container, "SM_Wall")
        scene.current_namespace = ":"
        scene.mark(staged, mark_for(container + reference.STAGING_SUFFIX))
        record_inventory(scene, staged)
        report = import_fixture(scene, HandoffFixture(self.directory / "handoff"))
        self.assertTrue(report["accepted"], report["problems"])
        self.assertTrue(report["update"]["recovery"]
                        ["finished_interrupted_container"])
        group = scene.find("|{0}:{0}".format(container))
        self.assertEqual(container, group.short)
        self.assertEqual(mark_for(container),
                         group.attributes["mtouSceneRefContainer"])

    def test_a_reference_left_in_a_retiring_name_is_put_back(self):
        scene = FakeReferenceScene()
        container = reference.REFERENCE_CONTAINER
        retiring = container + reference.RETIRING_SUFFIX
        scene.namespaces.add(retiring)
        scene.current_namespace = retiring
        group = scene.add_node(retiring, container)
        scene.current_namespace = ":"
        scene.mark(group, mark_for(container))
        content = scene.add_node(retiring, "SM_Old", parent=group)
        record_inventory(scene, group)
        report = self._recover(scene)
        self.assertTrue(report["accepted"])
        self.assertEqual(retiring, report["restored_previous_reference"])
        self.assertIs(group, scene.find("|{0}:{0}".format(container)))
        self.assertIs(content, group.children[0])
        self.assertIn("RESTORED_PREVIOUS_REFERENCE", " ".join(report["events"]))

    def test_a_retiring_reference_with_foreign_nodes_is_refused(self):
        scene = FakeReferenceScene()
        container = reference.REFERENCE_CONTAINER
        retiring = container + reference.RETIRING_SUFFIX
        scene.namespaces.add(retiring)
        scene.current_namespace = retiring
        group = scene.add_node(retiring, container)
        scene.mark(group, mark_for(container))
        scene.add_node(retiring, "ProductionObject")
        scene.current_namespace = ":"
        before = scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._recover(scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn("ProductionObject", caught.exception.detail)
        self.assertEqual(before, scene.snapshots())

    def test_a_refused_recovery_leaves_the_previous_reference_recoverable(self):
        scene = FakeReferenceScene()
        container = reference.REFERENCE_CONTAINER
        retiring = container + reference.RETIRING_SUFFIX
        # A finished container whose takeover already deleted the previous
        # reference, and a retiring copy of it left in the scene.
        owned_scene(scene)
        scene.namespaces.add(retiring)
        scene.current_namespace = retiring
        group = scene.add_node(retiring, container)
        scene.current_namespace = ":"
        scene.mark(group, mark_for(container))
        record_inventory(scene, group)
        original = reference._remove_namespace

        def broken(namespace):
            return False, "the host refused to delete {0!r}".format(namespace)

        reference._remove_namespace = broken
        try:
            with self.assertRaises(reference.ReferenceRefused) as caught:
                self._recover(scene)
        finally:
            reference._remove_namespace = original
        self.assertEqual("RECOVERY_REFUSED", caught.exception.code)
        self.assertIn(retiring, caught.exception.detail)
        self.assertIs(group, scene.find("|{0}:{1}".format(retiring, container)))
        self.assertIn(retiring, scene.namespaces)


class ContainerRemovalTest(unittest.TestCase):
    """Removal deletes only the container this tool owns."""

    def setUp(self):
        self.scene = FakeReferenceScene()
        self.container = reference.ReferenceContainer(
            container=reference.REFERENCE_CONTAINER, space_revision=3,
            budget=frozen_budget())

    def _remove(self, scene):
        with FakeBindings(scene):
            return self.container.remove()

    def test_the_owned_container_is_removed(self):
        owned_scene(self.scene, node_name="SM_Old")
        other = self.scene.add_node("", "ProductionObject", "transform")
        report = self._remove(self.scene)
        self.assertTrue(report["removed"])
        self.assertEqual(2, report["nodes_removed"])
        self.assertTrue(report["namespace_removed"])
        self.assertEqual("removed the reference container 'MtoU_UE_SceneRef' with 2 "
                         "node(s)", report["detail"])
        self.assertIs(other, self.scene.find("|ProductionObject"))
        self.assertNotIn(reference.REFERENCE_CONTAINER, self.scene.namespaces)
        self.assertEqual([], report["residual"]["nodes"])

    def test_an_empty_scene_reports_nothing_to_remove(self):
        report = self._remove(self.scene)
        self.assertFalse(report["removed"])
        self.assertIn("no tool-owned reference container", report["detail"])

    def test_a_name_this_tool_does_not_own_is_refused(self):
        foreign = self.scene.add_node("", reference.REFERENCE_CONTAINER)
        before = self.scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._remove(self.scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn(reference.REFERENCE_CONTAINER, caught.exception.detail)
        self.assertEqual(before, self.scene.snapshots())
        self.assertIs(foreign, self.scene.find("|" + reference.REFERENCE_CONTAINER))

    def test_a_scene_node_deep_under_the_container_refuses_the_removal(self):
        _group, mesh = owned_scene(self.scene, node_name="SM_Old")
        prop = self.scene.add_node("", "HeroProp", parent=mesh)
        before = self.scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._remove(self.scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn("HeroProp", caught.exception.detail)
        self.assertEqual(before, self.scene.snapshots())
        self.assertIs(prop, self.scene.find(prop.long_name))

    def test_a_node_created_in_the_container_namespace_refuses_the_removal(self):
        # The namespace does not prove a node came from the import: a node a
        # person created there, under the reference or beside it, is kept.
        container = reference.REFERENCE_CONTAINER
        group, mesh = owned_scene(self.scene, node_name="SM_Old")
        prop = self.scene.add_node(container, "ArtistProp", parent=group)
        shader = self.scene.add_node(container, "ArtistShader", "lambert")
        self.scene.namespaces.add(container + ":Props")
        child = self.scene.add_node(container + ":Props", "Crate", parent=mesh)
        before = self.scene.snapshots()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            self._remove(self.scene)
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        for name in ("ArtistProp", "ArtistShader", "Crate"):
            self.assertIn(name, caught.exception.detail)
        self.assertEqual(before, self.scene.snapshots())
        for node in (prop, shader, child):
            self.assertIs(node, self.scene.find(node.uuid))

    def test_a_refused_deletion_is_reported_not_raised(self):
        owned_scene(self.scene, node_name="SM_Old")
        original = reference._remove_namespace

        def broken(namespace):
            return False, "the host refused to delete {0!r}".format(namespace)

        reference._remove_namespace = broken
        try:
            report = self._remove(self.scene)
        finally:
            reference._remove_namespace = original
        self.assertFalse(report["accepted"])
        self.assertFalse(report["removed"])
        self.assertIn("CONTAINER_REMOVAL_FAILED", report["problems"][0])
        self.assertEqual(2, len(self.scene.namespace_members(
            reference.REFERENCE_CONTAINER)))


class ReportFieldTest(unittest.TestCase):
    """The report fields the panel and the protocol message read."""

    def setUp(self):
        self.directory = scratch_directory("mtou_ref_report_")
        self.scene = FakeReferenceScene()
        owned_scene(self.scene, node_name="SM_Old")

    def tearDown(self):
        shutil.rmtree(str(self.directory), ignore_errors=True)

    def test_the_import_report_carries_every_field_the_panel_renders(self):
        report = import_fixture(self.scene,
                                HandoffFixture(self.directory / "handoff"))
        self.assertIsInstance(report["accepted"], bool)
        self.assertTrue(report["accepted"])
        self.assertIsInstance(report["detail"], str)
        self.assertTrue(report["detail"])
        self.assertEqual(sorted(report["counts"]), ["bytes", "objects", "triangles"])
        self.assertEqual(reference.REFERENCE_CONTAINER, report["container"]["name"])
        self.assertIn("group_path", report["container"])
        self.assertIn("discarded", report)
        self.assertIn("rolled_back", report)
        self.assertIsNotNone(report["residual"])

    def test_a_refused_handoff_raises_with_a_frozen_code_and_detail(self):
        fixture = HandoffFixture(self.directory / "handoff")
        (fixture.directory / "baked.png").write_bytes(b"png")
        with FakeBindings(self.scene):
            with self.assertRaises(reference.ReferenceRefused) as caught:
                reference.import_handoff(fixture.handoff, frozen_budget())
        self.assertEqual("REFERENCE_IMAGE_DATA", caught.exception.code)
        self.assertTrue(caught.exception.detail)
        self.assertEqual(2, len(self.scene.namespace_members(
            reference.REFERENCE_CONTAINER)))
