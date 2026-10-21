"""Unit tests for the MtoU LiveLink camera module (port + host adapter).

The pure mapping tests are ported from
``prototypes/camera-sync/maya/MtoUCameraSyncPrototype/tests/
test_camera_sync_mapping.py``; the host adapter tests use a fake ``cmds``/``om``
so no Maya session is needed, plus one real mayapy host check.

    python -m unittest test_mtou_camera
"""

import copy
import math
import pathlib
import sys
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import camera as mtou_camera  # noqa: E402


def close(test, first, second, places=9, message=""):
    test.assertAlmostEqual(first, second, places=places, msg=message)


def camera_payload(**overrides):
    """One v10 ``camera_time`` camera payload, with both basis and rotation."""
    payload = {
        "name": "CineCameraActor",
        "location": {"x": 100.0, "y": 200.0, "z": 300.0},
        "right": {"x": 0.0, "y": 1.0, "z": 0.0},
        "up": {"x": 0.0, "y": 0.0, "z": 1.0},
        "forward": {"x": 1.0, "y": 0.0, "z": 0.0},
        "rotation": {"roll": 0.0, "pitch": 0.0, "yaw": 0.0},
        "focal_length_mm": 50.0,
        "sensor_width_mm": 24.89,
        "sensor_height_mm": 10.41,
        "sensor_horizontal_offset_mm": 0.5,
        "sensor_vertical_offset_mm": -0.25,
        "aspect_axis_constraint": "MaintainXFOV",
        "constrain_aspect_ratio": False,
        "f_stop": 2.8,
        "focus_distance_cm": 500.0,
        "depth_of_field": True,
        "squeeze_factor": 1.0,
        "near_clip_cm": 10.0,
        "far_clip_cm": None,
    }
    payload.update(overrides)
    return payload


def gate_payload(**overrides):
    """One v10 ``camera_time`` resolution gate, with the restore mirror."""
    payload = {
        "output_resolution": {"x": 1920, "y": 1080},
        "aperture_resolution": {"x": 1920, "y": 1080},
        "pixel_aspect_ratio": 1.0,
        "width": 1920,
        "height": 1080,
    }
    payload.update(overrides)
    return payload


class AxisConversionTest(unittest.TestCase):

    def test_known_vectors(self):
        # The v10 common space: Maya (x, y, z) is Unreal (x, z, y), the same
        # mapping the character stream uses.
        self.assertEqual(mtou_camera.ue_to_maya_point(1, 0, 0), (1.0, 0.0, 0.0))
        self.assertEqual(mtou_camera.ue_to_maya_point(0, 1, 0), (0.0, 0.0, 1.0))
        self.assertEqual(mtou_camera.ue_to_maya_point(0, 0, 1), (0.0, 1.0, 0.0))
        self.assertEqual(mtou_camera.ue_to_maya_point(10.0, -20.0, 30.0),
                         (10.0, 30.0, -20.0))

    def test_round_trip(self):
        for point in ((0, 0, 0), (1, 2, 3), (-4.5, 6.25, -7.125), (100.0, 0.0, 0.0)):
            maya = mtou_camera.ue_to_maya_point(*point)
            back = mtou_camera.maya_to_ue_point(*maya)
            for first, second in zip(point, back):
                close(self, float(first), second)

    def test_conversion_is_a_reflection(self):
        columns = [mtou_camera.ue_to_maya_point(*vector)
                   for vector in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
        determinant = (
            columns[0][0] * (columns[1][1] * columns[2][2] - columns[1][2] * columns[2][1])
            - columns[0][1] * (columns[1][0] * columns[2][2] - columns[1][2] * columns[2][0])
            + columns[0][2] * (columns[1][0] * columns[2][1] - columns[1][1] * columns[2][0]))
        close(self, determinant, -1.0)


class BasisVectorsTest(unittest.TestCase):

    def test_identity(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 0.0, 0.0))
        self.assertEqual(forward, (1.0, 0.0, 0.0))
        self.assertEqual(right, (0.0, 1.0, 0.0))
        self.assertEqual(up, (0.0, 0.0, 1.0))

    def test_yaw_90(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 0.0, 90.0))
        close(self, forward[0], 0.0)
        close(self, forward[1], 1.0)
        close(self, forward[2], 0.0)
        close(self, right[0], -1.0)
        close(self, up[2], 1.0)

    def test_pitch_90_looks_up(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 90.0, 0.0))
        close(self, forward[2], 1.0)
        close(self, up[0], -1.0)

    def test_roll_90_matches_unreal_matrix(self):
        right, up, forward = mtou_camera.ue_basis_vectors((90.0, 0.0, 0.0))
        close(self, right[2], -1.0)
        close(self, up[1], 1.0)
        close(self, forward[0], 1.0)

    def test_basis_is_orthonormal_and_right_handed(self):
        for rotation in ((0, 0, 0), (10, 20, 30), (-45, 33, 170), (0, -89, 0),
                         (75, -15, -120), (5, 0, 359)):
            right, up, forward = mtou_camera.ue_basis_vectors(rotation)
            for first, second in ((right, up), (right, forward), (up, forward)):
                dot = sum(a * b for a, b in zip(first, second))
                close(self, dot, 0.0, places=9, message=str(rotation))
            for vector in (right, up, forward):
                close(self, math.sqrt(sum(v * v for v in vector)), 1.0, places=9)
            cross = (right[1] * up[2] - right[2] * up[1],
                     right[2] * up[0] - right[0] * up[2],
                     right[0] * up[1] - right[1] * up[0])
            for first, second in zip(cross, forward):
                close(self, first, second, places=9, message=str(rotation))


class WorldMatrixTest(unittest.TestCase):

    def test_identity_basis_gives_identity_matrix(self):
        matrix = mtou_camera.ue_basis_to_maya_matrix(
            (1, 0, 0), (0, 0, 1), (0, -1, 0), (0, 0, 0))
        expected = (1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 0.0, 0.0, 1.0)
        for first, second in zip(matrix, expected):
            close(self, first, second)

    def test_translation_is_the_last_row(self):
        matrix = mtou_camera.ue_basis_to_maya_matrix(
            (1, 0, 0), (0, 0, 1), (0, -1, 0), (100, 200, 300))
        self.assertEqual(matrix[12:16], (100.0, 300.0, 200.0, 1.0))
        self.assertEqual(matrix[3], 0.0)

    def test_z_axis_is_negative_mapped_forward(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 40.0, 25.0))
        matrix = mtou_camera.ue_basis_to_maya_matrix(right, up, forward, (0, 0, 0))
        mapped_forward = mtou_camera.ue_to_maya_point(*forward)
        for index in range(3):
            close(self, matrix[8 + index], -mapped_forward[index])

    def test_camera_looks_along_mapped_forward(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 12.0, 35.0))
        matrix = mtou_camera.ue_basis_to_maya_matrix(right, up, forward, (0, 0, 0))
        marker = tuple(component * 500.0 for component in forward)
        ndc = mtou_camera.project_point_ndc(
            mtou_camera.ue_to_maya_point(*marker), matrix, 50.0, (0.5, 0.25), 1, (1920, 1080))
        close(self, ndc[0], 0.0, places=9)
        close(self, ndc[1], 0.0, places=9)

    def test_camera_space_point_uses_matrix_rows(self):
        right, up, forward = mtou_camera.ue_basis_vectors((0.0, 0.0, 90.0))
        matrix = mtou_camera.ue_basis_to_maya_matrix(right, up, forward, (10, 20, 30))
        marker = (10.0 + 100.0 * forward[0], 20.0 + 100.0 * forward[1],
                  30.0 + 100.0 * forward[2])
        local = mtou_camera.camera_space_point(
            mtou_camera.ue_to_maya_point(*marker), matrix)
        close(self, local[0], 0.0, places=9)
        close(self, local[1], 0.0, places=9)
        close(self, local[2], -100.0, places=9)


class FilmBackTest(unittest.TestCase):

    def test_aperture_inches(self):
        horizontal, vertical = mtou_camera.film_aperture_inches(25.4, 12.7)
        close(self, horizontal, 1.0)
        close(self, vertical, 0.5)
        horizontal, vertical = mtou_camera.film_aperture_inches(24.89, 10.41)
        close(self, horizontal, 24.89 / 25.4)
        close(self, vertical, 10.41 / 25.4)

    def test_offset_inches_preserves_sign(self):
        self.assertEqual(mtou_camera.film_offset_inches(0.0, 0.0), (0.0, 0.0))
        close(self, mtou_camera.film_offset_inches(25.4, -12.7)[0], 1.0)
        close(self, mtou_camera.film_offset_inches(25.4, -12.7)[1], -0.5)

    def test_film_fit_selection(self):
        horizontal = mtou_camera.FILM_FIT_HORIZONTAL
        vertical = mtou_camera.FILM_FIT_VERTICAL
        fill = mtou_camera.FILM_FIT_FILL
        self.assertEqual(mtou_camera.film_fit("MaintainXFOV", True, 1.3333, 1.7778), horizontal)
        self.assertEqual(mtou_camera.film_fit("MaintainYFOV", True, 1.3333, 1.7778), vertical)
        self.assertEqual(mtou_camera.film_fit(0, False, 1.3333, 1.7778), horizontal)
        self.assertEqual(mtou_camera.film_fit(1, False, 1.3333, 1.7778), vertical)
        self.assertEqual(mtou_camera.film_fit("MaintainMajorAxisFOV", False, 1.3333, 1.7778),
                         horizontal)
        self.assertEqual(mtou_camera.film_fit("MaintainMajorAxisFOV", False, 1.3333, 0.5625),
                         vertical)
        self.assertEqual(mtou_camera.film_fit("MaintainMajorAxisFOV", True, 0.75, 1.7778),
                         vertical)
        self.assertEqual(mtou_camera.film_fit(None, False, 1.3333, 1.7778), fill)
        self.assertEqual(mtou_camera.film_fit(2, True, 1.3333, 1.7778), horizontal)

    def test_film_fit_rejects_unknown_value(self):
        with self.assertRaises(mtou_camera.PayloadError) as caught:
            mtou_camera.film_fit("MaintainZFOV", False, 1.0, 1.0)
        self.assertEqual(caught.exception.category, mtou_camera.ERR_INVALID_FIELD)
        with self.assertRaises(mtou_camera.PayloadError):
            mtou_camera.film_fit(7, False, 1.0, 1.0)

    def test_film_fit_reason_names_the_rule(self):
        reason = mtou_camera.film_fit_reason("MaintainXFOV", False, 1.3333, 1.7778)
        self.assertIn("Horizontal", reason)
        self.assertIn("horizontal aperture", reason)

    def test_gate_extent_horizontal_keeps_the_horizontal_aperture(self):
        width, height = mtou_camera.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080))
        close(self, width, 0.5)
        close(self, height, 0.5 * 1080 / 1920)
        close(self, width / height, 1920 / 1080)

    def test_gate_extent_vertical_keeps_vertical_aperture(self):
        width, height = mtou_camera.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080))
        close(self, height, 0.25)
        close(self, width, 0.25 * 1920 / 1080)

    def test_gate_extent_fill_inscribes_the_image_in_the_film_gate(self):
        width, height = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080))
        close(self, height, 0.25)
        close(self, width, 0.25 * 1920 / 1080)
        self.assertLessEqual(width, 0.5)
        width, height = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (2500, 1000))
        close(self, width, 0.5)
        close(self, height, 0.5 / 2.5)
        self.assertLessEqual(height, 0.25)

    def test_gate_extent_applies_squeeze_to_the_horizontal_aperture(self):
        plain = mtou_camera.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080))
        squeezed = mtou_camera.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080), 2.0)
        close(self, squeezed[0], plain[0] * 2.0)
        close(self, squeezed[1], plain[1] * 2.0)
        plain = mtou_camera.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080))
        squeezed = mtou_camera.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080), 2.0)
        close(self, squeezed[1], 0.25)
        close(self, squeezed[0], plain[0])
        plain = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080))
        squeezed = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080), 2.0)
        self.assertEqual(plain, (0.25 * 1920 / 1080, 0.25))
        self.assertEqual(squeezed, plain)
        squeezed = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (5000, 1000), 2.0)
        self.assertEqual(squeezed, (1.0, 1.0 / 5.0))
        squeezed = mtou_camera.film_gate_extent_inches((0.5, 0.25), 0, (3000, 1000), 2.0)
        self.assertEqual(squeezed, (0.75, 0.25))

    def test_gate_extent_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            mtou_camera.film_gate_extent_inches((0.5, 0.25), 1, (0, 1080))
        with self.assertRaises(ValueError):
            mtou_camera.film_gate_extent_inches((0.5, 0.25), 9, (1920, 1080))


class TimeModelTest(unittest.TestCase):

    def test_equal_rates_keep_frame_numbers(self):
        time_value = mtou_camera.maya_time_for_frame(
            120.0, 100.0, {"numerator": 30, "denominator": 1}, 30.0, 20.0)
        close(self, time_value, 40.0)
        time_value = mtou_camera.maya_time_for_frame(
            120.0, 100.0, {"numerator": 30, "denominator": 1}, 30.0, 100.0)
        close(self, time_value, 120.0)

    def test_unequal_rates_preserve_the_offset_from_range_start(self):
        # 30 display frames at 30 Hz are 1 second after the range start, which
        # is 24 frames in a 24 fps Maya scene.
        time_value = mtou_camera.maya_time_for_frame(
            130.0, 100.0, {"numerator": 30, "denominator": 1}, 24.0, 1.0)
        close(self, time_value, 25.0)
        # Two display frames are 1/15 s, which is 1.6 frames at 24 fps.
        time_value = mtou_camera.maya_time_for_frame(
            102.0, 100.0, {"numerator": 30, "denominator": 1}, 24.0, 1.0)
        close(self, time_value, 2.6)

    def test_exact_rational_rates_are_not_rounded(self):
        # 23.976 is 24000/1001, never 24: 1000 display frames after the range
        # start are 1000 * 24 / (24000/1001) Maya frames.
        rate = {"numerator": 24000, "denominator": 1001}
        time_value = mtou_camera.maya_time_for_frame(1010.0, 10.0, rate, 24.0, 0.0)
        close(self, time_value, 1000.0 * 24.0 / (24000.0 / 1001.0))
        self.assertNotEqual(round(time_value, 4), 1000.0)

    def test_sub_frame_result_is_preserved(self):
        time_value = mtou_camera.maya_time_for_frame(
            101.0, 100.0, {"numerator": 24, "denominator": 1}, 30.0, 1.0)
        close(self, time_value, 2.25)
        close(self, mtou_camera.applied_maya_time(time_value, True), 2.25)

    def test_fractional_display_frame_is_preserved(self):
        time_value = mtou_camera.maya_time_for_frame(
            101.5, 100.0, {"numerator": 24, "denominator": 1}, 30.0, 1.0)
        close(self, time_value, 2.875)
        time_value = mtou_camera.maya_time_for_frame(
            101.0, 100.0, {"numerator": 1, "denominator": 1}, 30.0, 0.0)
        close(self, time_value, 30.0)

    def test_non_integer_target_frame(self):
        rate = {"numerator": 24000, "denominator": 1001}
        time_value = mtou_camera.maya_time_for_frame(10.0, 10.0, rate, 24.0, 1.0)
        close(self, time_value, 1.0)
        time_value = mtou_camera.maya_time_for_frame(1010.0, 10.0, rate, 24.0, 0.0)
        close(self, time_value, 1000.0 * 24.0 / (24000.0 / 1001.0))

    def test_negative_start_frames_are_supported(self):
        time_value = mtou_camera.maya_time_for_frame(
            -5.0, -5.0, {"numerator": 30, "denominator": 1}, 30.0, -5.0)
        close(self, time_value, -5.0)

    def test_explicit_origin_does_not_depend_on_connection_frame(self):
        time_value = mtou_camera.maya_time_for_frame(
            1030.5, 1001.0, {"numerator": 30, "denominator": 1}, 24.0, 1001.0)
        close(self, time_value, 1024.6)

    def test_rejects_bad_rates(self):
        with self.assertRaises(mtou_camera.PayloadError):
            mtou_camera.maya_time_for_frame(
                1.0, 0.0, {"numerator": 30, "denominator": 0}, 30.0, 0.0)
        with self.assertRaises(ValueError):
            mtou_camera.maya_time_for_frame(1.0, 0.0, 30.0, 0.0, 0.0)


class ProjectionTest(unittest.TestCase):
    """Hand-computed expectations for Maya's film-aperture projection."""

    EXPECTED = 0.7874015748031495

    def setUp(self):
        self.matrix = mtou_camera.ue_basis_to_maya_matrix(
            (1, 0, 0), (0, 0, 1), (0, -1, 0), (0, 0, 0))
        self.aperture = (0.5, 0.25)

    def test_hand_computed_point(self):
        # Maya camera space (10, 5, -100) cm; extent (0.5, 0.25) inches.
        ndc = mtou_camera.project_point_ndc(
            (10.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], self.EXPECTED)
        close(self, ndc[1], self.EXPECTED)

    def test_optical_axis_maps_to_the_gate_centre(self):
        ndc = mtou_camera.project_point_ndc(
            (0.0, 0.0, -250.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], 0.0)
        close(self, ndc[1], 0.0)

    def test_horizontal_fit_ignores_the_vertical_aperture(self):
        ndc = mtou_camera.project_point_ndc(
            (10.0, 5.0, -100.0), self.matrix, 50.0, (0.5, 0.5), 1, (1000, 500))
        close(self, ndc[1], self.EXPECTED)

    def test_aperture_edge_is_exactly_one(self):
        offset = 0.25 * 100.0 / (50.0 / 25.4)
        ndc = mtou_camera.project_point_ndc(
            (offset, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], 1.0)
        ndc = mtou_camera.project_point_ndc(
            (0.0, offset / 2.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[1], 1.0)

    def test_film_offset_shifts_the_frame(self):
        ndc = mtou_camera.project_point_ndc(
            (0.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            film_offset=(0.1, 0.0))
        close(self, ndc[0], -0.4)
        ndc = mtou_camera.project_point_ndc(
            (0.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            film_offset=(0.0, -0.025))
        close(self, ndc[1], 0.2)

    def test_film_offset_shifts_content_the_other_way(self):
        ndc = mtou_camera.project_point_ndc(
            (0.1 * 100.0 / (50.0 / 25.4), 0.0, -100.0), self.matrix, 50.0,
            self.aperture, 1, (1000, 500), film_offset=(0.1, 0.0))
        close(self, ndc[0], 0.0)

    def test_vertical_fit_keeps_the_vertical_aperture(self):
        horizontal = mtou_camera.project_point_ndc(
            (0.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 1, (500, 500))
        vertical = mtou_camera.project_point_ndc(
            (0.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 2, (500, 500))
        close(self, horizontal[1], 0.0984251968503937 / (0.5 / 2.0))
        close(self, vertical[1], 0.0984251968503937 / 0.125)
        close(self, vertical[1], self.EXPECTED)

    def test_squeeze_widens_the_horizontal_extent(self):
        plain = mtou_camera.project_point_ndc(
            (10.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        squeezed = mtou_camera.project_point_ndc(
            (10.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            lens_squeeze_ratio=2.0)
        close(self, squeezed[0], plain[0] / 2.0)

    def test_point_behind_the_camera_is_rejected(self):
        with self.assertRaises(ValueError):
            mtou_camera.project_point_ndc(
                (0.0, 0.0, 10.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))


class AccessorsTest(unittest.TestCase):

    def test_camera_field_tolerates_the_tolerated_spellings(self):
        camera = {"focal_length_mm": 35.0}
        self.assertEqual(35.0, mtou_camera.camera_field(camera, "focal_length_mm"))
        self.assertIsNone(mtou_camera.camera_field(camera, "f_stop"))
        self.assertEqual(2.8, mtou_camera.camera_field(
            {"aperture": 2.8}, "f_stop"))
        self.assertEqual(2.8, mtou_camera.camera_field(
            {"current_aperture": 2.8}, "f_stop"))

    def test_require_number_rejects_bools_strings_and_non_finite(self):
        for value in (True, False, "50", None, float("nan"), float("inf")):
            with self.assertRaises(mtou_camera.PayloadError) as caught:
                mtou_camera.require_number(value, "camera.focal_length_mm")
            self.assertEqual(caught.exception.category, mtou_camera.ERR_INVALID_FIELD)
        self.assertEqual(35.0, mtou_camera.require_number(35, "camera.focal_length_mm"))

    def test_require_mapping_and_sequence(self):
        with self.assertRaises(mtou_camera.PayloadError) as caught:
            mtou_camera.require_mapping([1, 2], "camera")
        self.assertIn("must be an object", caught.exception.detail)
        with self.assertRaises(mtou_camera.PayloadError):
            mtou_camera.require_sequence({"x": 1}, "markers")
        self.assertEqual([1.0, 2.0], mtou_camera.require_sequence([1, 2], "markers"))

    def test_require_vector_accepts_objects_and_sequences(self):
        self.assertEqual((1.0, 2.0, 3.0), mtou_camera.require_vector(
            {"x": 1, "y": 2, "z": 3}, "camera.location"))
        self.assertEqual((1.0, 2.0, 3.0), mtou_camera.require_vector(
            [1, 2, 3], "camera.location"))
        for broken in ({"x": 1, "y": 2}, [1, 2], [1, 2, 3, 4], {"x": "a", "y": 1, "z": 2}):
            with self.assertRaises(mtou_camera.PayloadError):
                mtou_camera.require_vector(broken, "camera.location")

    def test_normalize_aspect_constraint_spellings(self):
        self.assertIsNone(mtou_camera.normalize_aspect_constraint(None))
        self.assertEqual("maintain_xfov", mtou_camera.normalize_aspect_constraint(0))
        self.assertEqual("maintain_yfov", mtou_camera.normalize_aspect_constraint("MaintainYFOV"))
        self.assertEqual("maintain_major_axis_fov",
                         mtou_camera.normalize_aspect_constraint("maintainmajoraxisfov"))
        for broken in (7, "MaintainZFOV", True, 1.5):
            with self.assertRaises(mtou_camera.PayloadError):
                mtou_camera.normalize_aspect_constraint(broken)


class ValidateCameraTimeTest(unittest.TestCase):

    def _payload(self, **overrides):
        payload = {
            "type": "camera_time",
            "session_id": 5,
            "set_revision": 1,
            "mode_revision": 1,
            "space_revision": 1,
            "eval_id": "seq@10+0/cam#c1",
            "eval_serial": 1,
            "transport_serial": 1,
            "frame_serial": 1,
            "time": {"display_frame": 10.0,
                     "display_rate": {"numerator": 24, "denominator": 1},
                     "playback_range": {"start": 1, "end": 240}},
            "camera": camera_payload(),
            "gate": gate_payload(),
        }
        payload.update(overrides)
        return payload

    def test_accepts_the_complete_payload_and_ignores_extra_keys(self):
        payload = self._payload(extra={"a": 1},
                                camera=dict(camera_payload(), lens={"min_fstop": 1.4}),
                                cut={"stage": "root", "name": "shotA"})
        self.assertEqual(payload, mtou_camera.validate_camera_time(payload))

    def test_accepts_the_camera_shape_the_unreal_side_sends(self):
        payload = self._payload(camera={
            "location": [0.0, 120.0, 240.0],
            "right": [1.0, 0.0, 0.0],
            "up": [0.0, 0.0, 1.0],
            "forward": [0.0, 1.0, 0.0],
            "focal_length_mm": 35.0,
            "sensor_width_mm": 36.0,
            "sensor_height_mm": 20.25,
            "aspect_axis_constraint": "MaintainXFOV",
            "constrain_aspect_ratio": False,
            "aperture": 2.8,
            "focus_distance_cm": 240.0,
            "focus_method": "Manual",
            "squeeze_factor": 1.0,
            "near_clip_cm": 10.0,
        })
        self.assertEqual(payload, mtou_camera.validate_camera_time(payload))
        # focus_method Manual without an explicit depth_of_field turns DOF on.
        _, notes = mtou_camera.camera_attributes(
            payload["camera"], gate_payload())
        self.assertTrue(notes["intended"]["depth_of_field"])

    def test_missing_fields_are_rejected_with_their_names(self):
        for field in ("session_id", "set_revision", "mode_revision", "space_revision",
                      "eval_id", "eval_serial", "transport_serial", "frame_serial",
                      "time", "camera", "gate"):
            payload = self._payload()
            del payload[field]
            with self.assertRaises(mtou_camera.PayloadError) as caught:
                mtou_camera.validate_camera_time(payload)
            self.assertEqual(caught.exception.category, mtou_camera.ERR_MISSING_FIELD)
            self.assertIn(field, caught.exception.detail)

    def test_empty_eval_id_is_rejected(self):
        with self.assertRaises(mtou_camera.PayloadError) as caught:
            mtou_camera.validate_camera_time(self._payload(eval_id=""))
        self.assertEqual(caught.exception.category, mtou_camera.ERR_INVALID_FIELD)
        self.assertIn("eval_id", caught.exception.detail)

    def test_non_finite_numbers_are_rejected(self):
        for field in ("session_id", "eval_serial", "transport_serial", "frame_serial"):
            payload = self._payload()
            payload[field] = float("inf")
            with self.assertRaises(mtou_camera.PayloadError) as caught:
                mtou_camera.validate_camera_time(payload)
            self.assertEqual(caught.exception.category, mtou_camera.ERR_INVALID_FIELD)
        for field in ("session_id", "eval_serial"):
            payload = self._payload()
            payload[field] = "5"
            with self.assertRaises(mtou_camera.PayloadError) as caught:
                mtou_camera.validate_camera_time(payload)
            self.assertEqual(caught.exception.category, mtou_camera.ERR_INVALID_FIELD)

    def test_wrong_shapes_are_rejected(self):
        cases = {
            "time is not a mapping": {"time": [1, 2]},
            "camera is not a mapping": {"camera": "CineCameraActor"},
            "gate is not a mapping": {"gate": 1920},
            "time has no display frame": {"time": {"display_rate": {"numerator": 24,
                                                                   "denominator": 1}}},
            "gate declares one half": {"gate": {"width": 1920}},
            "gate has no extent": {"gate": {"pixel_aspect_ratio": 1.0}},
            "unknown message type": {"type": "mode_state"},
        }
        for label, patch in cases.items():
            with self.subTest(case=label):
                with self.assertRaises(mtou_camera.PayloadError):
                    mtou_camera.validate_camera_time(self._payload(**patch))

    def test_camera_payload_rejections(self):
        cases = {
            "missing focal length": camera_payload(focal_length_mm=None),
            "string focal length": camera_payload(focal_length_mm="50"),
            "nan sensor width": camera_payload(sensor_width_mm=float("nan")),
            "no location": camera_payload(location=None),
            "incomplete basis": dict(camera_payload(), up=None),
            "no basis and no rotation": camera_payload(right=None, up=None, forward=None,
                                                       rotation=None),
            "bad bool flag": camera_payload(depth_of_field="yes"),
        }
        for label, camera in cases.items():
            with self.subTest(case=label):
                with self.assertRaises(mtou_camera.PayloadError):
                    mtou_camera.validate_camera_time(self._payload(camera=camera))

    def test_bad_resolution_and_rates_are_rejected(self):
        with self.assertRaises(mtou_camera.PayloadError):
            mtou_camera.validate_camera_time(self._payload(
                gate=gate_payload(output_resolution={"x": 0, "y": 1080})))
        with self.assertRaises(mtou_camera.PayloadError):
            mtou_camera.validate_camera_time(self._payload(
                time={"display_frame": 10.0,
                      "display_rate": {"numerator": 30, "denominator": 0}}))


class CameraAttributesTest(unittest.TestCase):

    def test_units_and_film_fit(self):
        attributes, notes = mtou_camera.camera_attributes(
            camera_payload(), gate_payload())
        close(self, attributes["focalLength"], 50.0)
        close(self, attributes["horizontalFilmAperture"], 24.89 / 25.4)
        close(self, attributes["verticalFilmAperture"], 10.41 / 25.4)
        close(self, attributes["horizontalFilmOffset"], 0.5 / 25.4)
        close(self, attributes["verticalFilmOffset"], -0.25 / 25.4)
        self.assertEqual(attributes["filmFit"], mtou_camera.FILM_FIT_HORIZONTAL)
        close(self, attributes["fStop"], 2.8)
        close(self, attributes["focusDistance"], 500.0)
        self.assertTrue(attributes["depthOfField"])
        close(self, attributes["nearClipPlane"], 10.0)
        close(self, attributes["lensSqueezeRatio"], 1.0)
        self.assertIn("MaintainXFOV", notes["film_fit_reason"])
        self.assertEqual("Horizontal", notes["film_fit_name"])
        # The intended notes are what the read-back is compared against.
        close(self, notes["intended"]["focal_length_mm"], 50.0)
        close(self, notes["intended"]["horizontal_film_aperture_in"], 24.89 / 25.4)

    def test_focal_length_alias_and_sequence_basis(self):
        attributes, _ = mtou_camera.camera_attributes(
            {"location": [0, 0, 0], "forward": [1, 0, 0], "focal_length_mm": 35.0,
             "sensor_width_mm": 36.0, "sensor_height_mm": 20.25}, gate_payload())
        close(self, attributes["focalLength"], 35.0)

    def test_depth_of_field_comes_from_focus_method(self):
        def camera_with(method):
            camera = camera_payload()
            del camera["depth_of_field"]
            camera["focus_method"] = method
            return camera

        _, disabled = mtou_camera.camera_attributes(
            camera_with("Disable"), gate_payload())
        self.assertFalse(disabled["intended"]["depth_of_field"])
        _, manual = mtou_camera.camera_attributes(
            camera_with("Manual"), gate_payload())
        self.assertTrue(manual["intended"]["depth_of_field"])
        _, tracking = mtou_camera.camera_attributes(
            camera_with("Tracking"), gate_payload())
        self.assertTrue(tracking["intended"]["depth_of_field"])
        # An explicit flag always wins over the method, and neither is a
        # rejection when the payload simply declares no depth of field.
        _, explicit = mtou_camera.camera_attributes(
            camera_payload(focus_method="Disable"), gate_payload())
        self.assertTrue(explicit["intended"]["depth_of_field"])
        _, undeclared = mtou_camera.camera_attributes(
            camera_payload(depth_of_field=None), gate_payload())
        self.assertIsNone(undeclared["intended"]["depth_of_field"])

    def test_far_clip_falls_back_to_the_documented_default(self):
        _, notes = mtou_camera.camera_attributes(camera_payload(), gate_payload())
        close(self, notes["far_clip_cm"], mtou_camera.DEFAULT_FAR_CLIP_CM)
        self.assertTrue(notes["far_clip_substituted"])
        self.assertEqual("module_default", notes["far_clip_source"])
        _, finite = mtou_camera.camera_attributes(
            camera_payload(far_clip_cm=12345.0), gate_payload())
        close(self, finite["far_clip_cm"], 12345.0)
        self.assertFalse(finite["far_clip_substituted"])
        self.assertEqual("camera", finite["far_clip_source"])

    def test_sensor_aspect_and_rendered_aspect_are_read(self):
        _, notes = mtou_camera.camera_attributes(
            camera_payload(sensor_aspect_ratio=2.39, rendered_aspect_ratio=1.85,
                           aspect_axis_constraint="MaintainMajorAxisFOV"),
            gate_payload())
        close(self, notes["sensor_aspect"], 2.39)
        close(self, notes["output_aspect"], 1.85)
        self.assertEqual(mtou_camera.FILM_FIT_HORIZONTAL, notes["film_fit"])

    def test_gate_aliases_and_pixel_aspect(self):
        for gate in ({"width": 1280, "height": 720, "pixel_aspect_ratio": 1.0},
                     {"output_width": 1280, "output_height": 720},
                     {"output_resolution": {"x": 1280, "y": 720}},
                     {"resolution": {"x": 1280, "y": 720}}):
            _, notes = mtou_camera.camera_attributes(camera_payload(), gate)
            close(self, notes["output_aspect"], 1280.0 / 720.0)
        _, notes = mtou_camera.camera_attributes(
            camera_payload(), {"output_resolution": {"x": 1920, "y": 1080},
                               "pixel_aspect_ratio": 2.0})
        close(self, notes["output_aspect"], 1920.0 * 2.0 / 1080.0)

    def test_optional_attributes_are_omitted_when_not_declared(self):
        camera = camera_payload()
        for field in ("f_stop", "focus_distance_cm", "depth_of_field"):
            del camera[field]
        attributes, _ = mtou_camera.camera_attributes(camera, gate_payload())
        for attribute in ("fStop", "focusDistance", "depthOfField"):
            self.assertNotIn(attribute, attributes)

    def test_a_camera_without_focus_keeps_maya_focus_distance(self):
        # A production CineCamera with focus disabled reports a focus distance
        # of 0; the camera still maps, only the focus distance is not written.
        camera = camera_payload(focus_distance_cm=0.0, depth_of_field=False)
        attributes, notes = mtou_camera.camera_attributes(camera, gate_payload())
        self.assertNotIn("focusDistance", attributes)
        self.assertFalse(attributes["depthOfField"])
        self.assertIsNone(notes["intended"]["focus_distance_cm"])

    def test_invalid_values_are_rejected(self):
        for camera in (camera_payload(focal_length_mm=0.0),
                       camera_payload(sensor_width_mm=-1.0),
                       camera_payload(f_stop=0.0),
                       camera_payload(squeeze_factor=0.0),
                       camera_payload(focus_distance_cm=-5.0)):
            with self.assertRaises(mtou_camera.PayloadError):
                mtou_camera.camera_attributes(camera, gate_payload())


class FakeOpenMaya(object):
    """The ``om`` subset :class:`LinkCamera` reads the world matrix through."""

    class MDagPath(object):
        def __init__(self, cmds, name):
            self._cmds = cmds
            self._name = name

        def inclusiveMatrix(self):
            return FakeWorldMatrix(self._cmds._matrix)

    class MSelectionListImpl(object):
        def __init__(self, cmds):
            self._cmds = cmds

        def add(self, name):
            self._cmds.last_selection = name
            return self

        def getDagPath(self, unused_index):
            return FakeOpenMaya.MDagPath(self._cmds, self._cmds.last_selection)

    def __init__(self, cmds):
        self._cmds = cmds
        outer = self

        class MSelectionList(outer.MSelectionListImpl):
            def __init__(self):
                outer.MSelectionListImpl.__init__(self, outer._cmds)

        self.MSelectionList = MSelectionList


#: Resolution-gate plugs and their index in ``FakeCmds.gate``.
_GATE_PLUGS = {
    "defaultResolution.width": 0,
    "defaultResolution.height": 1,
    "defaultResolution.pixelAspect": 2,
    "defaultResolution.deviceAspectRatio": 3,
}


class FakeWorldMatrix(object):
    def __init__(self, values):
        self._values = list(values)

    def __iter__(self):
        return iter(self._values)


class FakeCmds(object):
    """A minimal fake ``cmds`` recording every write it receives.

    One node store holds the tool camera, one panel set and the resolution
    gate, so the refusals, the apply, the read-back and the restore can be
    asserted without a Maya session.
    """

    def __init__(self, panels=(), unit="cm", locked=(), driven=(), camera_shapes=None,
                 start_time=10.0, start_gate=(960, 540, 1.0, 1.7777777777777777)):
        self.panels = list(panels)
        self.unit = unit
        self.locked = set(locked)
        self.driven = set(driven)
        self.camera_shapes = camera_shapes or {}
        self.nodes = {}
        self.panel_cameras = {}
        # Panels whose camera switch Maya refuses, for the restore residual.
        self.failing_panels = set()
        self.gate = list(start_gate)
        self.time = start_time
        self.undo_state = True
        self.undo_depth = 0
        self.undo_log = []
        self._chunks = []
        self.writes = []
        self.next_id = 0
        self._matrix = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]

    # ------------------------------------------------------------- queries

    def currentUnit(self, query=False, linear=None, **kwargs):
        if query and linear:
            return self.unit
        raise AssertionError("unexpected currentUnit call")

    def _require_node(self, name):
        # Like Maya: a model panel is a UI object, unknown to node queries.
        if name.split(".")[0] in self.panels:
            raise RuntimeError("No object matches name: " + name)

    def objExists(self, name):
        if name.split(".")[0] in self.panels:
            return False
        if name in self.nodes:
            return True
        return name in self.camera_shapes or any(
            name in shapes for shapes in self.camera_shapes.values())

    def getAttr(self, plug, **kwargs):
        return self._get_attr(plug, kwargs)

    def _get_attr(self, plug, kwargs):
        self._require_node(plug)
        if kwargs.get("lock"):
            return plug in self.locked
        if kwargs.get("settable"):
            return plug not in self.driven
        if kwargs.get("type"):
            return self._attr_type(plug)
        if plug in self.nodes:
            return self.nodes[plug]
        if plug in _GATE_PLUGS:
            index = _GATE_PLUGS[plug]
            return self.gate[index]
        # An attribute of a camera that has never been written returns Maya's
        # own default, which is what the read-back has to report too.
        if plug.endswith(".filmFit"):
            return 0
        if plug.endswith(".displayResolution") or plug.endswith(".displayGateMask"):
            return False
        if plug.endswith(".depthOfField"):
            return False
        if plug.endswith(".horizontalFilmAperture"):
            return 1.41732
        if plug.endswith(".verticalFilmAperture"):
            return 0.94488
        if plug.endswith(".focalLength"):
            return 35.0
        if plug.endswith(".fStop"):
            return 5.6
        if plug.endswith(".focusDistance"):
            return 5.0
        if plug.endswith(".lensSqueezeRatio"):
            return 1.0
        if plug.endswith(".horizontalFilmOffset"):
            return 0.0
        if plug.endswith(".verticalFilmOffset"):
            return 0.0
        if plug.endswith(".nearClipPlane"):
            return 0.1
        if plug.endswith(".farClipPlane"):
            return 10000.0
        raise AssertionError("no attribute {0}".format(plug))

    def _attr_type(self, plug):
        if plug == "defaultResolution.width":
            return "long"
        if plug == "defaultResolution.deviceAspectRatio":
            return "float"
        value = self.nodes.get(plug)
        if isinstance(value, bool):
            return "bool"
        if isinstance(value, float) and value.is_integer():
            return "double"
        return "double"

    def attributeQuery(self, attribute, node=None, listParent=False):
        self._require_node(node or "")
        if listParent:
            if attribute in ("translateX", "translateY", "translateZ"):
                return ["translate"]
            if attribute in ("rotateX", "rotateY", "rotateZ"):
                return ["rotate"]
            if attribute in ("scaleX", "scaleY", "scaleZ"):
                return ["scale"]
            if attribute in ("shearXY", "shearXZ", "shearYZ"):
                return ["shear"]
            return None
        raise AssertionError("unexpected attributeQuery call")

    def listConnections(self, plug, source=False, destination=False, **kwargs):
        self._require_node(plug)
        if source and not destination and plug in self.driven:
            return ["driverCurve"]
        return []

    def lockNode(self, node, query=False, lock=False):
        self._require_node(node)
        if query:
            return [False]
        return None

    def getPanel(self, type=None):
        if type in ("modelPanel", "modelEditor"):
            return list(self.panels)
        raise AssertionError("unexpected getPanel call")

    def nodeType(self, name):
        self._require_node(name)
        return "transform"

    def xform(self, name, query=False, worldSpace=False, matrix=None):
        if query:
            return list(self._matrix)
        if worldSpace and matrix is not None:
            self._matrix = list(matrix)
            self.writes.append(("xform", name, list(matrix)))
            return None
        raise AssertionError("unexpected xform call")

    def modelPanel(self, panel, edit=False, query=False, camera=None, exists=False):
        if query and exists:
            return panel in self.panels and panel.startswith("modelPanel")
        return self._panel_call(panel, edit, query, camera)

    def modelEditor(self, panel, edit=False, query=False, camera=None, exists=False):
        if query and exists:
            return panel in self.panels and not panel.startswith("modelPanel")
        return self._panel_call(panel, edit, query, camera)

    def _panel_call(self, panel, edit, query, camera):
        if panel not in self.panels:
            raise RuntimeError("Object not found: " + panel)
        if edit and panel in self.failing_panels:
            raise RuntimeError("Cannot set the camera of " + panel)
        if query and camera:
            return self.panel_cameras.get(panel, "persp")
        if edit and camera:
            self.panel_cameras[panel] = camera
            self.writes.append(("panel", panel, camera))
            return None
        raise AssertionError("unexpected panel call")

    def undoInfo(self, query=False, state=True, openChunk=False, closeChunk=False,
                 stateWithoutFlush=None):
        if stateWithoutFlush is not None:
            self.undo_state = bool(stateWithoutFlush)
            return self.undo_state
        if query and state:
            return self.undo_state
        if openChunk:
            self.undo_depth += 1
            self.undo_log.append("open")
            self._chunks.append(("open", self._snapshot()))
            return None
        if closeChunk:
            self.undo_depth = max(0, self.undo_depth - 1)
            self.undo_log.append("close")
            return None
        raise AssertionError("unexpected undoInfo call")

    def _snapshot(self):
        return (copy.deepcopy(self.nodes), copy.deepcopy(self.panel_cameras),
                list(self.gate), self.time, list(self._matrix),
                copy.deepcopy(self.camera_shapes))

    def currentTime(self, value=None, edit=False, query=False, **kwargs):
        if query:
            return self.time
        if edit:
            self.time = float(value)
            self.writes.append(("currentTime", self.time))
            return self.time
        raise AssertionError("unexpected currentTime call")

    def playbackOptions(self, query=False, minTime=False, maxTime=False):
        if query:
            return (1.0, 120.0) if minTime else 120.0
        raise AssertionError("unexpected playbackOptions call")

    # -------------------------------------------------------------- writes

    def setAttr(self, plug, value):
        if plug in self.locked:
            raise AssertionError("{0} is locked".format(plug))
        if plug in self.driven:
            raise AssertionError("{0} is driven".format(plug))
        if plug in _GATE_PLUGS:
            self.gate[_GATE_PLUGS[plug]] = value
            self.writes.append(("setAttr", plug, value))
            return None
        self.nodes[plug] = value
        self.writes.append(("setAttr", plug, value))
        return None

    def undo(self):
        """Undo everything since the open chunk opened.

        The fake keeps a deep snapshot at every ``undoInfo(openChunk=True)`` and
        restores it, which is what Maya's undo of one chunk does.
        """
        while self._chunks:
            label, snapshot = self._chunks.pop()
            if label == "open":
                self._restore(snapshot)
                break
        return None

    def _restore(self, snapshot):
        (self.nodes, self.panel_cameras, self.gate, self.time, self._matrix,
         self.camera_shapes) = (copy.deepcopy(snapshot[0]),
                                copy.deepcopy(snapshot[1]),
                                list(snapshot[2]), snapshot[3], list(snapshot[4]),
                                copy.deepcopy(snapshot[5]))
        self.writes = [write for write in self.writes if write[0] == "openChunk"]

    def createNode(self, node_type, name=None, parent=None, **kwargs):
        self.next_id += 1
        created = name if name is not None else "{0}{1}".format(node_type, self.next_id)
        self.nodes[created] = None
        if node_type == "camera":
            # ``camera_shapes`` maps a transform to the shape it carries; the
            # caller creates the transform first and parents the camera shape
            # under it, exactly as Maya's own example does.
            if parent and parent in self.nodes:
                self.camera_shapes.setdefault(parent, []).append(created)
            else:
                self.camera_shapes.setdefault(created, []).append(created)
        self.writes.append(("createNode", node_type, created))
        return created

    def listRelatives(self, name, shapes=False, type=None, parent=False,
                      fullPath=False):
        if shapes and type == "camera":
            return list(self._shapes_of(name))
        if parent and fullPath:
            return []
        raise AssertionError("unexpected listRelatives call")

    def _shapes_of(self, name):
        """The camera shapes parented under ``name``; a shape lists itself."""
        found = []
        for transform, shapes in self.camera_shapes.items():
            if not isinstance(shapes, (list, tuple)):
                shapes = [shapes]
            if transform == name:
                found.extend(shapes)
            elif name in shapes:
                found.append(name)
        return [shape for index, shape in enumerate(found) if shape not in found[:index]]

    def delete(self, name):
        if name not in self.nodes:
            raise AssertionError("no node {0}".format(name))
        for shape in self._shapes_of(name):
            self.nodes.pop(shape, None)
        # Deleting a transform deletes the shapes it carries.
        self.camera_shapes.pop(name, None)
        for transform in list(self.camera_shapes):
            shapes = [shape for shape in self.camera_shapes[transform]
                      if shape != name]
            if shapes:
                self.camera_shapes[transform] = shapes
            else:
                del self.camera_shapes[transform]
        self.nodes.pop(name, None)
        self.writes.append(("delete", name))
        return None


class FakeCmdsWithUuid(FakeCmds):
    """FakeCmds whose nodes carry UUIDs, as Maya 2016+ reports them."""

    def __init__(self, **kwargs):
        FakeCmds.__init__(self, **kwargs)
        self.uuids = {}
        self._uuid_counter = 0

    def uuid_of(self, name):
        if name not in self.uuids:
            self._uuid_counter += 1
            self.uuids[name] = "uuid-{0}".format(self._uuid_counter)
        return self.uuids[name]

    def replace_node(self, name):
        """Another scene's node now carries the same name."""
        self.uuids.pop(name, None)
        self.uuid_of(name)

    def ls(self, target, uuid=False, long=False):
        if uuid:
            return [self.uuid_of(target)] if self.objExists(target) else []
        return [name for name, value in self.uuids.items()
                if value == target and self.objExists(name)]

    def createNode(self, node_type, name=None, parent=None, **kwargs):
        created = FakeCmds.createNode(self, node_type, name, parent, **kwargs)
        self.replace_node(created)
        return created


class LinkCameraHostTestBase(unittest.TestCase):
    """Patches the module-level ``cmds``/``om`` and builds a fake scene."""

    def build_cmds(self, **kwargs):
        return FakeCmds(**kwargs)

    def setUp(self):
        self.cmds = self.build_cmds()
        self.om = FakeOpenMaya(self.cmds)
        patcher = mock.patch.multiple(mtou_camera, cmds=self.cmds, om=self.om)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.camera = mtou_camera.LinkCamera()

    def set_matrix(self, values):
        self.cmds._matrix = list(values)

    def node(self, name):
        return self.cmds.nodes.get(name)

    def assert_nothing_written(self):
        # A borrowed camera that already carries the name is a fixture, not a
        # write; what matters is that this adapter created and wrote nothing.
        self.assertEqual([], self.cmds.writes)
        self.assertEqual([960, 540, 1.0, 1.7777777777777777], self.cmds.gate)
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME,
                         [write for write in self.cmds.writes
                          if write[0] == "createNode"])


class LinkCameraApplyTest(LinkCameraHostTestBase):

    def test_apply_creates_one_camera_and_writes_the_mapped_values(self):
        report, values = self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME, report["camera"])
        self.assertTrue(report["created"])
        shape = mtou_camera.LINK_CAMERA_NAME + "Shape"
        self.assertEqual(("createNode", "transform", mtou_camera.LINK_CAMERA_NAME),
                         self.cmds.writes[0])
        self.assertEqual(("createNode", "camera", shape), self.cmds.writes[1])
        self.assertEqual(50.0, self.cmds.nodes[shape + ".focalLength"])
        close(self, self.cmds.nodes[shape + ".horizontalFilmAperture"], 24.89 / 25.4)
        close(self, self.cmds.nodes[shape + ".verticalFilmAperture"], 10.41 / 25.4)
        close(self, self.cmds.nodes[shape + ".horizontalFilmOffset"], 0.5 / 25.4)
        close(self, self.cmds.nodes[shape + ".verticalFilmOffset"], -0.25 / 25.4)
        self.assertEqual(mtou_camera.FILM_FIT_HORIZONTAL,
                         self.cmds.nodes[shape + ".filmFit"])
        self.assertEqual(2.8, self.cmds.nodes[shape + ".fStop"])
        self.assertEqual(500.0, self.cmds.nodes[shape + ".focusDistance"])
        self.assertTrue(self.cmds.nodes[shape + ".depthOfField"])
        self.assertEqual(10.0, self.cmds.nodes[shape + ".nearClipPlane"])
        self.assertEqual(mtou_camera.DEFAULT_FAR_CLIP_CM,
                         self.cmds.nodes[shape + ".farClipPlane"])
        self.assertTrue(self.cmds.nodes[shape + ".displayResolution"])
        self.assertTrue(self.cmds.nodes[shape + ".displayGateMask"])
        # The world matrix is the mapped Unreal basis and location: rows are
        # right, up, -forward, and the translation is the last row.
        self.assertEqual([0.0, 0.0, 1.0, 0.0,
                          0.0, 1.0, 0.0, 0.0,
                          -1.0, 0.0, 0.0, 0.0,
                          100.0, 300.0, 200.0, 1.0], self.cmds._matrix)
        # The resolution gate follows the aperture pixels and the device aspect.
        self.assertEqual((1920, 1080), (self.cmds.gate[0], self.cmds.gate[1]))
        close(self, self.cmds.gate[3], 1920.0 / 1080.0)
        self.assertEqual(mtou_camera.DEFAULT_FAR_CLIP_CM, values["far_clip_cm"])
        self.assertTrue(values["far_clip_substituted"])
        self.assertEqual(report["notes"], self.camera.last_notes)

    def test_apply_writes_the_aperture_pixels_of_a_gate_inside_a_larger_output(self):
        gate = gate_payload(output_resolution={"x": 1920, "y": 1080},
                            aperture_resolution={"x": 1280, "y": 540})
        report, _ = self.camera.apply(camera_payload(), gate)
        self.assertEqual((1280, 540), (self.cmds.gate[0], self.cmds.gate[1]))
        close(self, self.cmds.gate[3], 1280.0 / 540.0)
        self.assertEqual({"output_width": 1280, "output_height": 540,
                          "pixel_aspect_ratio": 1.0}, report["gate"])

    def test_apply_folds_the_gate_pixel_aspect_into_the_device_aspect(self):
        _, _ = self.camera.apply(camera_payload(), gate_payload(pixel_aspect_ratio=2.0))
        close(self, self.cmds.gate[3], 1920.0 * 2.0 / 1080.0)
        self.assertEqual(2.0, self.cmds.gate[2])

    def test_apply_reuses_a_created_camera_and_creates_no_second_node(self):
        self.camera.apply(camera_payload(), gate_payload())
        created = [write for write in self.cmds.writes if write[0] == "createNode"]
        report, _ = self.camera.apply(camera_payload(focal_length_mm=85.0), gate_payload())
        self.assertFalse(report["created"])
        self.assertEqual(created, [write for write in self.cmds.writes
                                   if write[0] == "createNode"])
        self.assertEqual(85.0, self.cmds.nodes[
            mtou_camera.LINK_CAMERA_NAME + "Shape.focalLength"])

    def test_apply_refuses_a_foreign_node_under_the_tool_name(self):
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_NAME_CONFLICT,
                         caught.exception.category)
        self.assertEqual([], self.cmds.writes)

    def test_apply_borrows_a_camera_that_already_carries_the_tool_name(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        report, _ = self.camera.apply(camera_payload(), gate_payload())
        self.assertFalse(report["created"])
        self.assertEqual(shape, report["shape"])

    def test_readback_reports_the_host_values(self):
        self.set_matrix([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                         0.0, 0.0, 1.0, 0.0, 5.0, 6.0, 7.0, 1.0])
        self.camera.apply(camera_payload(), gate_payload())
        values = self.camera.readback()
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME, values["camera"])
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME + "Shape", values["shape"])
        expected = [0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                    -1.0, 0.0, 0.0, 0.0, 100.0, 300.0, 200.0, 1.0]
        self.assertEqual(len(expected), len(values["world_matrix"]))
        for first, second in zip(expected, values["world_matrix"]):
            close(self, first, second)
        close(self, values["focal_length_mm"], 50.0)
        close(self, values["horizontal_film_aperture_in"], 24.89 / 25.4)
        close(self, values["vertical_film_aperture_in"], 10.41 / 25.4)
        close(self, values["horizontal_film_offset_in"], 0.5 / 25.4)
        close(self, values["vertical_film_offset_in"], -0.25 / 25.4)
        self.assertEqual(mtou_camera.FILM_FIT_HORIZONTAL, values["film_fit"])
        self.assertEqual("Horizontal", values["film_fit_name"])
        self.assertEqual(2.8, values["f_stop"])
        self.assertEqual(500.0, values["focus_distance_cm"])
        self.assertTrue(values["depth_of_field"])
        self.assertEqual(10.0, values["near_clip_cm"])
        self.assertEqual(mtou_camera.DEFAULT_FAR_CLIP_CM, values["far_clip_cm"])
        self.assertEqual((1920, 1080, 1.0, 1920.0 / 1080.0),
                         (values["gate"]["width"], values["gate"]["height"],
                          values["gate"]["pixel_aspect_ratio"],
                          values["gate"]["device_aspect_ratio"]))

    def test_readback_without_an_apply_is_refused(self):
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.readback()
        self.assertEqual(mtou_camera.CATEGORY_NO_CAMERA_YET, caught.exception.category)

    def test_readback_reports_a_payload_without_optional_attributes(self):
        camera = camera_payload()
        for field in ("f_stop", "focus_distance_cm", "depth_of_field"):
            del camera[field]
        camera["focus_method"] = "Manual"
        self.camera.apply(camera, gate_payload())
        values = self.camera.readback()
        self.assertTrue(values["depth_of_field"])

    def test_apply_rejects_a_payload_before_touching_the_scene(self):
        with self.assertRaises(mtou_camera.PayloadError):
            self.camera.apply(camera_payload(focal_length_mm=None), gate_payload())
        self.assert_nothing_written()


class LinkCameraStateTest(LinkCameraHostTestBase):

    def test_capture_state_records_panels_gate_undo_and_camera(self):
        self.cmds.panels = ["modelPanel4"]
        self.cmds.panel_cameras["modelPanel4"] = "persp"
        state = self.camera.capture_state()
        self.assertEqual("MtoU_LinkCamera", state["name"])
        self.assertEqual({"modelPanel4": "persp"}, state["panels"])
        self.assertEqual([960, 540, 1.0, 1.7777777777777777],
                         [state["resolution"][plug]
                          for plug in mtou_camera.RESOLUTION_PLUGS])
        self.assertTrue(state["undo"])
        self.assertIsNone(state["camera"])
        self.assertEqual([], self.cmds.writes)

    def test_capture_state_records_a_borrowed_camera(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.cmds.nodes[shape + ".focalLength"] = 24.0
        state = self.camera.capture_state()
        self.assertEqual(shape, state["camera"]["shape"])
        self.assertEqual(24.0, state["camera"]["attributes"]["focalLength"])
        self.assertEqual(self.cmds._matrix, state["camera"]["world_matrix"])

    def test_restore_state_puts_back_panels_gate_undo_and_deletes_our_camera(self):
        self.cmds.panel_cameras["modelPanel4"] = "persp"
        self.cmds.panels = ["modelPanel4"]
        state = self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME,
                         self.cmds.panel_cameras["modelPanel4"])
        residual = self.camera.restore_state(state)
        self.assertEqual([], residual)
        self.assertEqual("persp", self.cmds.panel_cameras["modelPanel4"])
        self.assertEqual([960, 540, 1.0, 1.7777777777777777], self.cmds.gate)
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME + "Shape", self.cmds.nodes)
        self.assertTrue(self.cmds.undo_state)
        # A second call changes nothing and raises nothing.
        self.assertEqual([], self.camera.restore_state(state))
        self.assertEqual([], self.camera.restore_state(None))
        self.assertEqual([], self.camera.restore_state(None))

    def test_restore_state_without_a_capture_only_deletes_our_camera(self):
        self.camera.apply(camera_payload(), gate_payload())
        residual = self.camera.restore_state(None)
        self.assertEqual([], residual)
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)

    def test_restore_state_without_anything_does_nothing(self):
        self.assertEqual([], self.camera.restore_state(None))

    def test_apply_keeps_ownership_of_the_camera_it_created(self):
        """Regression: a second capture after apply must not disown the camera."""
        report, _ = self.camera.apply(camera_payload(), gate_payload())
        self.assertTrue(report["created"])
        self.assertTrue(self.camera._created)
        # A capture taken while the tool camera exists still records the node,
        # but it does not make the adapter believe it borrowed it.
        state = self.camera.capture_state()
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME, state["camera"]["transform"])
        self.assertTrue(self.camera._created)
        residual = self.camera.restore_state(state)
        self.assertEqual([], residual)
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)

    def test_restore_state_restores_a_borrowed_camera_and_keeps_the_node(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.cmds.nodes[shape + ".focalLength"] = 24.0
        self.cmds.nodes[shape + ".fStop"] = 1.4
        self.set_matrix([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                         0.0, 0.0, 1.0, 0.0, 11.0, 12.0, 13.0, 1.0])
        state = self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(50.0, self.cmds.nodes[shape + ".focalLength"])
        residual = self.camera.restore_state(state)
        self.assertEqual([], residual)
        self.assertEqual(24.0, self.cmds.nodes[shape + ".focalLength"])
        self.assertEqual(1.4, self.cmds.nodes[shape + ".fStop"])
        self.assertEqual([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                          0.0, 0.0, 1.0, 0.0, 11.0, 12.0, 13.0, 1.0],
                         self.cmds._matrix)
        self.assertIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)

    def test_restore_state_reports_a_resource_it_cannot_put_back(self):
        state = self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.cmds.locked.add("defaultResolution.width")
        residual = self.camera.restore_state(state)
        self.assertEqual(1, len(residual))
        self.assertIn("defaultResolution.width", residual[0])
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)

    def test_restore_state_reports_a_panel_it_cannot_put_back(self):
        self.cmds.panels = ["modelPanel4"]
        self.cmds.panel_cameras["modelPanel4"] = "persp"
        state = self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.cmds.failing_panels.add("modelPanel4")
        residual = self.camera.restore_state(state)
        self.assertEqual(1, len(residual))
        self.assertIn("modelPanel4", residual[0])

    def test_restore_state_puts_back_an_open_panel_and_skips_a_closed_one(self):
        self.cmds.panels = ["modelPanel4", "modelPanel1"]
        self.cmds.panel_cameras["modelPanel4"] = "persp"
        self.cmds.panel_cameras["modelPanel1"] = "side"
        state = self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.cmds.panels.remove("modelPanel1")
        self.assertEqual([], self.camera.restore_state(state))
        self.assertEqual("persp", self.cmds.panel_cameras["modelPanel4"])

    def test_apply_undoes_a_failed_read_back_comparison(self):
        shape = mtou_camera.LINK_CAMERA_NAME + "Shape"

        def losing_verify(instance, values, attributes, notes, matrix):
            raise mtou_camera.CameraSyncRefused(
                mtou_camera.CATEGORY_HOST_STATE_MISMATCH, "focalLength mismatch")

        with mock.patch.object(mtou_camera.LinkCamera, "_verify", losing_verify):
            with self.assertRaises(mtou_camera.CameraSyncRefused):
                self.camera.apply(camera_payload(), gate_payload())
        # The undo removed every write the chunk made: the camera shape's
        # mapped attributes and the resolution gate.
        self.assertNotIn(shape + ".focalLength", self.cmds.nodes)
        self.assertNotIn(shape + ".filmFit", self.cmds.nodes)
        self.assertEqual([960, 540, 1.0, 1.7777777777777777], self.cmds.gate)
        # The failed apply left an empty tool camera behind only as the nodes
        # Maya would keep; restore_state deletes them, which is what the caller
        # does on a refused target.
        self.assertEqual([], self.camera.restore_state(self.camera._state))


class LinkCameraIdentityTest(LinkCameraHostTestBase):
    """Restore acts on node identity, never on a name another node now carries."""

    def build_cmds(self, **kwargs):
        return FakeCmdsWithUuid(**kwargs)

    def test_restore_deletes_the_created_camera_by_identity(self):
        self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual([], self.camera.restore_state(None))
        self.assertNotIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)

    def test_restore_keeps_a_foreign_camera_that_reuses_the_name(self):
        self.camera.apply(camera_payload(), gate_payload())
        # The open scene changed: its own camera carries the tool name.
        self.cmds.replace_node(mtou_camera.LINK_CAMERA_NAME)
        residual = self.camera.restore_state(None)
        self.assertEqual([], residual)
        self.assertIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)
        self.assertNotIn(("delete", mtou_camera.LINK_CAMERA_NAME), self.cmds.writes)
        self.assertFalse(self.camera._created)

    def test_restore_does_not_write_a_borrowed_snapshot_into_another_node(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.cmds.nodes[shape + ".focalLength"] = 24.0
        state = self.camera.capture_state()
        self.cmds.replace_node(mtou_camera.LINK_CAMERA_NAME)
        self.cmds.nodes[shape + ".focalLength"] = 85.0
        self.cmds.writes[:] = []
        self.assertEqual([], self.camera.restore_state(state))
        self.assertEqual(85.0, self.cmds.nodes[shape + ".focalLength"])
        self.assertFalse(any(write[0] == "xform" for write in self.cmds.writes))

    def test_apply_after_capture_refuses_a_node_that_took_the_name(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        # The borrowed camera was deleted and another one took its name.
        self.cmds.replace_node(mtou_camera.LINK_CAMERA_NAME)
        self.cmds.writes[:] = []
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(focal_length_mm=60.0), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_REPLACED, caught.exception.category)
        self.assertEqual([], self.cmds.writes)

    def test_apply_after_capture_refuses_a_replaced_shape(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.camera.capture_state()
        self.camera.apply(camera_payload(), gate_payload())
        self.cmds.replace_node(shape)
        self.cmds.writes[:] = []
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(focal_length_mm=60.0), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_REPLACED, caught.exception.category)
        self.assertEqual([], self.cmds.writes)

    def test_a_deleted_created_camera_is_created_again(self):
        self.camera.apply(camera_payload(), gate_payload())
        self.cmds.delete(mtou_camera.LINK_CAMERA_NAME)
        report, unused_values = self.camera.apply(camera_payload(), gate_payload())
        self.assertTrue(self.camera._created)
        self.assertIn(mtou_camera.LINK_CAMERA_NAME, self.cmds.nodes)


class LinkCameraRefusalTest(LinkCameraHostTestBase):

    def test_scene_unit_is_refused_before_anything_is_written(self):
        self.cmds.unit = "m"
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_UNSUPPORTED_SCENE_UNIT,
                         caught.exception.category)
        self.assertIn("centimetres", caught.exception.detail)
        self.assert_nothing_written()

    def test_locked_camera_attribute_is_refused(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.cmds.locked.add(shape + ".focalLength")
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_LOCKED_ATTRIBUTE, caught.exception.category)
        self.assertIn("focalLength", caught.exception.detail)
        self.assert_nothing_written()

    def test_driven_camera_attribute_is_refused(self):
        shape = "BorrowedShape"
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = shape
        self.cmds.driven.add(shape + ".focalLength")
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_INPUT_DRIVEN, caught.exception.category)
        self.assertIn("driver", caught.exception.detail)
        self.assert_nothing_written()

    def test_locked_transform_attribute_is_refused(self):
        self.cmds.nodes[mtou_camera.LINK_CAMERA_NAME] = None
        self.cmds.camera_shapes[mtou_camera.LINK_CAMERA_NAME] = "BorrowedShape"
        self.cmds.locked.add(mtou_camera.LINK_CAMERA_NAME + ".translateX")
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_LOCKED_ATTRIBUTE, caught.exception.category)
        self.assertIn("translateX", caught.exception.detail)
        self.assert_nothing_written()

    def test_locked_gate_attribute_is_refused(self):
        self.cmds.locked.add("defaultResolution.width")
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_LOCKED_ATTRIBUTE, caught.exception.category)
        self.assertIn("defaultResolution.width", caught.exception.detail)
        self.assert_nothing_written()

    def test_driven_gate_attribute_is_refused(self):
        self.cmds.driven.add("defaultResolution.deviceAspectRatio")
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_INPUT_DRIVEN, caught.exception.category)
        self.assert_nothing_written()

    def test_panels_are_switched_through_panel_commands_only(self):
        # Real Maya knows no node named after a model panel; nodeType, getAttr
        # and objExists all fail on one, so none of them may be used for it.
        self.cmds.panels = ["modelPanel1", "modelEditor1"]
        self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME, self.cmds.panel_cameras["modelPanel1"])
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME, self.cmds.panel_cameras["modelEditor1"])

    def test_apply_switches_every_model_panel_to_the_tool_camera(self):
        self.cmds.panels = ["modelPanel4", "modelEditor1"]
        self.cmds.panel_cameras["modelEditor1"] = "otherCam"
        self.camera.apply(camera_payload(), gate_payload())
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME,
                         self.cmds.panel_cameras["modelPanel4"])
        self.assertEqual(mtou_camera.LINK_CAMERA_NAME,
                         self.cmds.panel_cameras["modelEditor1"])


class LinkCameraTimeTest(LinkCameraHostTestBase):

    def test_applied_time_keeps_fractional_frames(self):
        self.assertEqual(2.875, self.camera.applied_time(2.875))
        self.assertEqual(-5.0, self.camera.applied_time(-5.0))

    def test_set_time_moves_the_host_and_reports_the_value(self):
        applied = self.camera.set_time(12.375)
        self.assertEqual(12.375, applied)
        self.assertEqual(12.375, self.cmds.time)
        self.assertEqual(("currentTime", 12.375), self.cmds.writes[-1])

    def test_set_time_refuses_a_non_finite_frame(self):
        for frame in (float("nan"), float("inf"), "10", True, None):
            with self.assertRaises(mtou_camera.PayloadError):
                self.camera.set_time(frame)
        self.assertEqual(10.0, self.cmds.time)


if __name__ == "__main__":
    unittest.main()
