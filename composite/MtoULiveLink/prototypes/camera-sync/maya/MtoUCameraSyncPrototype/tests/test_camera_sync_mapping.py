"""Pure-Python checks for the camera sync mapping.

Runs under plain CPython 3 and under mayapy; nothing here imports Maya.

    python -m unittest discover -s <this directory> -t <package root>
"""
import math
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import mtou_camera_sync_mapping as mapping  # noqa: E402


def close(test, first, second, places=9, message=""):
    test.assertAlmostEqual(first, second, places=places, msg=message)


class AxisConversionTest(unittest.TestCase):

    def test_known_vectors(self):
        # Unreal forward +X, right +Y, up +Z land on Maya -Z, +X, +Y.
        close(self, mapping.ue_to_maya_point(1, 0, 0)[2], -1.0)
        self.assertEqual(mapping.ue_to_maya_point(1, 0, 0), (0.0, 0.0, -1.0))
        self.assertEqual(mapping.ue_to_maya_point(0, 1, 0), (1.0, 0.0, 0.0))
        self.assertEqual(mapping.ue_to_maya_point(0, 0, 1), (0.0, 1.0, 0.0))
        self.assertEqual(mapping.ue_to_maya_point(10.0, -20.0, 30.0),
                         (-20.0, 30.0, -10.0))

    def test_round_trip(self):
        for point in ((0, 0, 0), (1, 2, 3), (-4.5, 6.25, -7.125), (100.0, 0.0, 0.0)):
            maya = mapping.ue_to_maya_point(*point)
            back = mapping.maya_to_ue_point(*maya)
            for first, second in zip(point, back):
                close(self, float(first), second)

    def test_conversion_is_a_reflection(self):
        # Converting a left-handed basis into a right-handed one flips handedness.
        columns = [mapping.ue_to_maya_point(*vector)
                   for vector in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
        determinant = (
            columns[0][0] * (columns[1][1] * columns[2][2] - columns[1][2] * columns[2][1])
            - columns[0][1] * (columns[1][0] * columns[2][2] - columns[1][2] * columns[2][0])
            + columns[0][2] * (columns[1][0] * columns[2][1] - columns[1][1] * columns[2][0]))
        close(self, determinant, -1.0)


class BasisVectorsTest(unittest.TestCase):

    def test_identity(self):
        right, up, forward = mapping.ue_basis_vectors((0.0, 0.0, 0.0))
        self.assertEqual(forward, (1.0, 0.0, 0.0))
        self.assertEqual(right, (0.0, 1.0, 0.0))
        self.assertEqual(up, (0.0, 0.0, 1.0))

    def test_yaw_90(self):
        # Unreal yaw +90 turns forward from +X towards +Y.
        right, up, forward = mapping.ue_basis_vectors((0.0, 0.0, 90.0))
        close(self, forward[0], 0.0)
        close(self, forward[1], 1.0)
        close(self, forward[2], 0.0)
        close(self, right[0], -1.0)
        close(self, up[2], 1.0)

    def test_pitch_90_looks_up(self):
        right, up, forward = mapping.ue_basis_vectors((0.0, 90.0, 0.0))
        close(self, forward[2], 1.0)
        close(self, up[0], -1.0)

    def test_roll_90_matches_unreal_matrix(self):
        # Unreal's FRotationMatrix for roll +90 about the forward axis.
        right, up, forward = mapping.ue_basis_vectors((90.0, 0.0, 0.0))
        close(self, right[2], -1.0)
        close(self, up[1], 1.0)
        close(self, forward[0], 1.0)

    def test_basis_is_orthonormal_and_right_handed(self):
        for rotation in ((0, 0, 0), (10, 20, 30), (-45, 33, 170), (0, -89, 0),
                         (75, -15, -120), (5, 0, 359)):
            right, up, forward = mapping.ue_basis_vectors(rotation)
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
        matrix = mapping.ue_basis_to_maya_matrix(
            (0, 1, 0), (0, 0, 1), (1, 0, 0), (0, 0, 0))
        expected = (1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 0.0, 0.0, 1.0)
        for first, second in zip(matrix, expected):
            close(self, first, second)

    def test_translation_is_the_last_row(self):
        matrix = mapping.ue_basis_to_maya_matrix(
            (0, 1, 0), (0, 0, 1), (1, 0, 0), (100, 200, 300))
        self.assertEqual(matrix[12:16], (200.0, 300.0, -100.0, 1.0))
        self.assertEqual(matrix[3], 0.0)

    def test_z_axis_is_negative_mapped_forward(self):
        right, up, forward = mapping.ue_basis_vectors((0.0, 40.0, 25.0))
        matrix = mapping.ue_basis_to_maya_matrix(right, up, forward, (0, 0, 0))
        mapped_forward = mapping.ue_to_maya_point(*forward)
        for index in range(3):
            close(self, matrix[8 + index], -mapped_forward[index])

    def test_camera_looks_along_mapped_forward(self):
        # A camera at the Unreal origin whose forward points at the marker must
        # put that marker at the centre of the frame.
        right, up, forward = mapping.ue_basis_vectors((0.0, 12.0, 35.0))
        matrix = mapping.ue_basis_to_maya_matrix(right, up, forward, (0, 0, 0))
        marker = tuple(component * 500.0 for component in forward)
        ndc = mapping.project_point_ndc(
            mapping.ue_to_maya_point(*marker), matrix, 50.0, (0.5, 0.25), 1, (1920, 1080))
        close(self, ndc[0], 0.0, places=9)
        close(self, ndc[1], 0.0, places=9)

    def test_camera_space_point_uses_matrix_rows(self):
        right, up, forward = mapping.ue_basis_vectors((0.0, 0.0, 90.0))
        matrix = mapping.ue_basis_to_maya_matrix(right, up, forward, (10, 20, 30))
        # A point 100 cm along Unreal forward is 100 cm in front of the camera.
        marker = (10.0 + 100.0 * forward[0], 20.0 + 100.0 * forward[1],
                  30.0 + 100.0 * forward[2])
        local = mapping.camera_space_point(mapping.ue_to_maya_point(*marker), matrix)
        close(self, local[0], 0.0, places=9)
        close(self, local[1], 0.0, places=9)
        close(self, local[2], -100.0, places=9)


class FilmBackTest(unittest.TestCase):

    def test_aperture_inches(self):
        horizontal, vertical = mapping.film_aperture_inches(25.4, 12.7)
        close(self, horizontal, 1.0)
        close(self, vertical, 0.5)
        horizontal, vertical = mapping.film_aperture_inches(24.89, 10.41)
        close(self, horizontal, 24.89 / 25.4)
        close(self, vertical, 10.41 / 25.4)

    def test_offset_inches_preserves_sign(self):
        self.assertEqual(mapping.film_offset_inches(0.0, 0.0), (0.0, 0.0))
        close(self, mapping.film_offset_inches(25.4, -12.7)[0], 1.0)
        close(self, mapping.film_offset_inches(25.4, -12.7)[1], -0.5)

    def test_film_fit_selection(self):
        horizontal = mapping.FILM_FIT_HORIZONTAL
        vertical = mapping.FILM_FIT_VERTICAL
        fill = mapping.FILM_FIT_FILL
        self.assertEqual(mapping.film_fit("MaintainXFOV", True, 1.3333, 1.7778), horizontal)
        self.assertEqual(mapping.film_fit("MaintainYFOV", True, 1.3333, 1.7778), vertical)
        self.assertEqual(mapping.film_fit(0, False, 1.3333, 1.7778), horizontal)
        self.assertEqual(mapping.film_fit(1, False, 1.3333, 1.7778), vertical)
        # MaintainMajorAxisFOV keeps the larger axis of the rendered aspect.
        self.assertEqual(mapping.film_fit("MaintainMajorAxisFOV", False, 1.3333, 1.7778),
                         horizontal)
        self.assertEqual(mapping.film_fit("MaintainMajorAxisFOV", False, 1.3333, 0.5625),
                         vertical)
        # Constraining the aspect ratio makes the sensor aspect the rendered one.
        self.assertEqual(mapping.film_fit("MaintainMajorAxisFOV", True, 0.75, 1.7778),
                         vertical)
        # No declared constraint uses the whole sensor.
        self.assertEqual(mapping.film_fit(None, False, 1.3333, 1.7778), fill)
        self.assertEqual(mapping.film_fit(2, True, 1.3333, 1.7778), horizontal)

    def test_film_fit_rejects_unknown_value(self):
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.film_fit("MaintainZFOV", False, 1.0, 1.0)
        self.assertEqual(caught.exception.category, mapping.ERR_INVALID_FIELD)
        with self.assertRaises(mapping.PayloadError):
            mapping.film_fit(7, False, 1.0, 1.0)

    def test_film_fit_reason_names_the_rule(self):
        reason = mapping.film_fit_reason("MaintainXFOV", False, 1.3333, 1.7778)
        self.assertIn("Horizontal", reason)
        self.assertIn("horizontal aperture", reason)

    def test_gate_extent_horizontal_keeps_the_horizontal_aperture(self):
        width, height = mapping.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080))
        close(self, width, 0.5)
        close(self, height, 0.5 * 1080 / 1920)
        close(self, width / height, 1920 / 1080)

    def test_gate_extent_vertical_keeps_vertical_aperture(self):
        width, height = mapping.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080))
        close(self, height, 0.25)
        close(self, width, 0.25 * 1920 / 1080)

    def test_gate_extent_fill_inscribes_the_image_in_the_film_gate(self):
        # Gate aspect 1.7778 against a 2.0 sensor: the image is narrower than the
        # film gate, so the vertical aperture is exact. Confirmed against
        # rendered images by tests/maya_render_marker_check.py.
        width, height = mapping.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080))
        close(self, height, 0.25)
        close(self, width, 0.25 * 1920 / 1080)
        self.assertLessEqual(width, 0.5)
        # Gate aspect 2.5 against a 2.0 sensor: the image is wider than the film
        # gate, so the horizontal aperture is exact.
        width, height = mapping.film_gate_extent_inches((0.5, 0.25), 0, (2500, 1000))
        close(self, width, 0.5)
        close(self, height, 0.5 / 2.5)
        self.assertLessEqual(height, 0.25)

    def test_gate_extent_applies_squeeze_to_the_horizontal_aperture(self):
        # Horizontal fit follows the (widened) horizontal aperture on both axes.
        plain = mapping.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080))
        squeezed = mapping.film_gate_extent_inches((0.5, 0.25), 1, (1920, 1080), 2.0)
        close(self, squeezed[0], plain[0] * 2.0)
        close(self, squeezed[1], plain[1] * 2.0)
        # Vertical fit keeps the vertical aperture, so the squeeze cannot move it.
        plain = mapping.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080))
        squeezed = mapping.film_gate_extent_inches((0.5, 0.25), 2, (1920, 1080), 2.0)
        close(self, squeezed[1], 0.25)
        close(self, squeezed[0], plain[0])
        # Fill compares the squeezed horizontal aperture with the gate aspect:
        # at 2.0 with a gate aspect of 1.7778 the image is still narrower than
        # the film gate, so the vertical aperture stays exact.
        plain = mapping.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080))
        squeezed = mapping.film_gate_extent_inches((0.5, 0.25), 0, (1920, 1080), 2.0)
        self.assertEqual(plain, (0.25 * 1920 / 1080, 0.25))
        self.assertEqual(squeezed, plain)
        # With a gate wider than the squeezed film gate the horizontal aperture
        # becomes the exact one: 4.0 in against a 5.0 gate.
        squeezed = mapping.film_gate_extent_inches((0.5, 0.25), 0, (5000, 1000), 2.0)
        self.assertEqual(squeezed, (1.0, 1.0 / 5.0))
        squeezed = mapping.film_gate_extent_inches((0.5, 0.25), 0, (3000, 1000), 2.0)
        self.assertEqual(squeezed, (0.75, 0.25))

    def test_gate_extent_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            mapping.film_gate_extent_inches((0.5, 0.25), 1, (0, 1080))
        with self.assertRaises(ValueError):
            mapping.film_gate_extent_inches((0.5, 0.25), 9, (1920, 1080))


class TimeModelTest(unittest.TestCase):

    def test_equal_rates_keep_frame_numbers(self):
        time_value, quantized = mapping.maya_time_for_frame(
            120.0, 100.0, {"numerator": 30, "denominator": 1}, 30.0, 20.0)
        self.assertFalse(quantized)
        close(self, time_value, 40.0)
        # Maya started on the same frame as the range start.
        time_value, _ = mapping.maya_time_for_frame(
            120.0, 100.0, {"numerator": 30, "denominator": 1}, 30.0, 100.0)
        close(self, time_value, 120.0)

    def test_unequal_rates_preserve_the_offset_from_range_start(self):
        # 30 display frames at 30 Hz are 1 second after the range start, which is
        # 24 frames in a 24 fps Maya scene.
        time_value, quantized = mapping.maya_time_for_frame(
            130.0, 100.0, {"numerator": 30, "denominator": 1}, 24.0, 1.0)
        self.assertFalse(quantized)
        close(self, time_value, 25.0)
        # Two display frames are 1/15 s, which is 1.6 frames at 24 fps.
        time_value, quantized = mapping.maya_time_for_frame(
            102.0, 100.0, {"numerator": 30, "denominator": 1}, 24.0, 1.0)
        self.assertTrue(quantized)
        close(self, time_value, 2.6)

    def test_sub_frame_result_is_preserved(self):
        time_value, subframe = mapping.maya_time_for_frame(
            101.0, 100.0, {"numerator": 24, "denominator": 1}, 30.0, 1.0)
        self.assertTrue(subframe)
        close(self, time_value, 2.25)
        close(self, mapping.applied_maya_time(time_value, subframe), 2.25)

    def test_fractional_display_frame_is_preserved(self):
        time_value, subframe = mapping.maya_time_for_frame(
            101.5, 100.0, {"numerator": 24, "denominator": 1}, 30.0, 1.0)
        close(self, time_value, 2.875)
        close(self, mapping.applied_maya_time(time_value, subframe), 2.875)
        time_value, quantized = mapping.maya_time_for_frame(
            101.0, 100.0, {"numerator": 1, "denominator": 1}, 30.0, 0.0)
        close(self, time_value, 30.0)
        self.assertFalse(quantized)

    def test_non_integer_target_frame(self):
        # A 23.976 display rate against a 24 fps scene keeps the wall-clock offset.
        rate = {"numerator": 24000, "denominator": 1001}
        time_value, quantized = mapping.maya_time_for_frame(
            10.0, 10.0, rate, 24.0, 1.0)
        close(self, time_value, 1.0)
        self.assertFalse(quantized)
        time_value, quantized = mapping.maya_time_for_frame(
            1010.0, 10.0, rate, 24.0, 0.0)
        close(self, time_value, 1000.0 * 24.0 / (24000.0 / 1001.0))
        self.assertTrue(quantized)

    def test_negative_start_frames_are_supported(self):
        time_value, quantized = mapping.maya_time_for_frame(
            -5.0, -5.0, {"numerator": 30, "denominator": 1}, 30.0, -5.0)
        close(self, time_value, -5.0)
        self.assertFalse(quantized)

    def test_explicit_origin_does_not_depend_on_connection_frame(self):
        time_value, _ = mapping.maya_time_for_frame(
            1030.5, 1001.0, {"numerator": 30, "denominator": 1},
            24.0, 1001.0)
        close(self, time_value, 1024.6)

    def test_rejects_bad_rates(self):
        with self.assertRaises(mapping.PayloadError):
            mapping.maya_time_for_frame(1.0, 0.0, {"numerator": 30, "denominator": 0}, 30.0, 0.0)
        with self.assertRaises(ValueError):
            mapping.maya_time_for_frame(1.0, 0.0, 30.0, 0.0, 0.0)


class ProjectionTest(unittest.TestCase):
    """Hand-computed expectations for Maya's film-aperture projection."""

    # Camera at the Maya origin, identity rotation: aperture (0.5, 0.25) inches,
    # 50 mm focal length, resolution 1000x500 (gate aspect 2.0), Horizontal fit.
    # extent = (0.5, 0.25) inches, so half extents are 0.25 and 0.125 inches.
    # A point 10 cm right, 5 cm up, 100 cm in front:
    #   u = 5.0 cm * 10 cm / 100 cm = 0.5 cm = 0.1968504 in
    #   ndc_x = 0.1968504 / 0.25 = 0.7874016
    #   v = 5.0 cm * 5 cm / 100 cm = 0.25 cm = 0.0984252 in
    #   ndc_y = 0.0984252 / 0.125 = 0.7874016
    EXPECTED = 0.7874015748031495

    def setUp(self):
        self.matrix = mapping.ue_basis_to_maya_matrix(
            (0, 1, 0), (0, 0, 1), (1, 0, 0), (0, 0, 0))
        self.aperture = (0.5, 0.25)

    def test_hand_computed_point(self):
        # Maya camera space (10, 5, -100) cm.
        ndc = mapping.project_point_ndc(
            (10.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], self.EXPECTED)
        close(self, ndc[1], self.EXPECTED)

    def test_optical_axis_maps_to_the_gate_centre(self):
        ndc = mapping.project_point_ndc(
            (0.0, 0.0, -250.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], 0.0)
        close(self, ndc[1], 0.0)

    def test_horizontal_fit_ignores_the_vertical_aperture(self):
        # Doubling the vertical aperture cannot change a Horizontal fit.
        ndc = mapping.project_point_ndc(
            (10.0, 5.0, -100.0), self.matrix, 50.0, (0.5, 0.5), 1, (1000, 500))
        close(self, ndc[1], self.EXPECTED)

    def test_aperture_edge_is_exactly_one(self):
        # The half aperture at 100 cm is 0.25 in => 0.25 * 100 / (50/25.4) cm.
        offset = 0.25 * 100.0 / (50.0 / 25.4)
        ndc = mapping.project_point_ndc(
            (offset, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[0], 1.0)
        ndc = mapping.project_point_ndc(
            (0.0, offset / 2.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        close(self, ndc[1], 1.0)

    def test_film_offset_shifts_the_frame(self):
        # ox = 0.1 in over a 0.25 in half extent is -0.4 in NDC for a centred point.
        ndc = mapping.project_point_ndc(
            (0.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            film_offset=(0.1, 0.0))
        close(self, ndc[0], -0.4)
        ndc = mapping.project_point_ndc(
            (0.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            film_offset=(0.0, -0.025))
        close(self, ndc[1], 0.2)

    def test_film_offset_shifts_content_the_other_way(self):
        # A point that lands on the optical axis of a shifted gate reads zero.
        ndc = mapping.project_point_ndc(
            (0.1 * 100.0 / (50.0 / 25.4), 0.0, -100.0), self.matrix, 50.0,
            self.aperture, 1, (1000, 500), film_offset=(0.1, 0.0))
        close(self, ndc[0], 0.0)

    def test_vertical_fit_keeps_the_vertical_aperture(self):
        # Gate aspect 2.0 and aperture aspect 2.0 agree, so both fits coincide
        # only for the extent; use a 1:1 gate to separate them.
        horizontal = mapping.project_point_ndc(
            (0.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 1, (500, 500))
        vertical = mapping.project_point_ndc(
            (0.0, 5.0, -100.0), self.matrix, 50.0, self.aperture, 2, (500, 500))
        close(self, horizontal[1], 0.0984251968503937 / (0.5 / 2.0))
        close(self, vertical[1], 0.0984251968503937 / 0.125)
        close(self, vertical[1], self.EXPECTED)

    def test_squeeze_widens_the_horizontal_extent(self):
        plain = mapping.project_point_ndc(
            (10.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))
        squeezed = mapping.project_point_ndc(
            (10.0, 0.0, -100.0), self.matrix, 50.0, self.aperture, 1, (1000, 500),
            lens_squeeze_ratio=2.0)
        close(self, squeezed[0], plain[0] / 2.0)

    def test_point_behind_the_camera_is_rejected(self):
        with self.assertRaises(ValueError):
            mapping.project_point_ndc(
                (0.0, 0.0, 10.0), self.matrix, 50.0, self.aperture, 1, (1000, 500))

    def test_ndc_to_pixel_matches_the_raster_convention(self):
        close(self, mapping.ndc_to_pixel((0.0, 0.0), (1920, 1080))[0], 959.5)
        close(self, mapping.ndc_to_pixel((0.0, 0.0), (1920, 1080))[1], 539.5)
        close(self, mapping.ndc_to_pixel((-1.0, 1.0), (1920, 1080))[0], -0.5)
        close(self, mapping.ndc_to_pixel((-1.0, 1.0), (1920, 1080))[1], -0.5)
        close(self, mapping.ndc_to_pixel((1.0, -1.0), (1920, 1080))[0], 1919.5)
        close(self, mapping.ndc_to_pixel((1.0, -1.0), (1920, 1080))[1], 1079.5)


def session_payload(**overrides):
    payload = {
        "type": "session",
        "protocol": "MtoUCameraSync",
        "version": mapping.PROTOCOL_VERSION,
        "port": 54330,
        "time_authority": "unreal",
        "sequence": "MtoU_CameraSync",
        "display_rate": {"numerator": 30, "denominator": 1},
        "tick_resolution": {"numerator": 24000, "denominator": 1001},
        "playback_range": {"start": 100.0, "end": 200.0},
        "output_resolution": {"x": 1920, "y": 1080},
        "far_clip_fallback_cm": 100000.0,
        "camera_cut": {"camera": "CineCameraActor", "stage": "root"},
    }
    payload.update(overrides)
    return payload


def camera_payload(**overrides):
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


def frame_payload(**overrides):
    payload = {
        "type": "frame",
        "session": "MtoU_CameraSync",
        "sequence": 1,
        "frame_serial": 7,
        "eval_serial": 3,
        "eval_identity": "MtoU_CameraSync@24000/CineCameraActor",
        "time": {"display_frame": 130.0, "seconds": 1.0, "source_frame": 130,
                 "tick": 24000, "display_rate": {"numerator": 30, "denominator": 1}},
        "camera_cut": {"camera": "CineCameraActor", "stage": "root"},
        "output_resolution": {"x": 1920, "y": 1080},
        "camera": camera_payload(),
        "view": {"location": {"x": 0.0, "y": 0.0, "z": 0.0}},
        "projection": {"view_proj": [0.0] * 16, "view_rect": {"x": 0, "y": 0}},
        "markers": [
            {"name": "MarkerA", "location": {"x": 500.0, "y": 0.0, "z": 0.0},
             "ndc": {"x": 0.0, "y": 0.0}, "pixel": {"x": 960.0, "y": 540.0},
             "inside": True},
        ],
    }
    payload.update(overrides)
    return payload


class ValidationTest(unittest.TestCase):

    def test_accepts_the_full_payload_and_ignores_extra_keys(self):
        session = mapping.validate_message(session_payload(extra_key={"a": 1}))
        self.assertEqual(session["protocol"], "MtoUCameraSync")
        frame = mapping.validate_message(frame_payload(
            unknown_top_level=[1, 2],
            camera=camera_payload(dof={"b_override_fstop": True}, lens={"min_fstop": 1.4})))
        self.assertEqual(frame["frame_serial"], 7)

    def test_unknown_type_is_rejected_with_a_stable_category(self):
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message({"type": "pause"})
        self.assertEqual(caught.exception.category, mapping.ERR_UNKNOWN_TYPE)

    def test_missing_type_is_rejected(self):
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message({"session": "x"})
        self.assertEqual(caught.exception.category, mapping.ERR_MISSING_FIELD)

    def test_missing_top_level_field_is_rejected(self):
        payload = frame_payload()
        del payload["projection"]
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(payload)
        self.assertEqual(caught.exception.category, mapping.ERR_MISSING_FIELD)
        self.assertIn("projection", caught.exception.detail)

    def test_frame_serial_is_required(self):
        payload = frame_payload()
        del payload["frame_serial"]
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(payload)
        self.assertIn("frame_serial", caught.exception.detail)

    def test_evaluation_identity_is_required(self):
        for field in ("eval_serial", "eval_identity"):
            payload = frame_payload()
            del payload[field]
            with self.assertRaises(mapping.PayloadError) as caught:
                mapping.validate_message(payload)
            self.assertEqual(caught.exception.category, mapping.ERR_MISSING_FIELD)
            self.assertIn(field, caught.exception.detail)
        self.assertEqual(
            mapping.validate_message(frame_payload())["eval_identity"],
            "MtoU_CameraSync@24000/CineCameraActor")

    def test_evaluation_identity_must_be_a_non_empty_string(self):
        for value in ("", 7, None, ["id"]):
            with self.assertRaises(mapping.PayloadError) as caught:
                mapping.validate_message(frame_payload(eval_identity=value))
            self.assertEqual(caught.exception.category, mapping.ERR_INVALID_FIELD)
            self.assertIn("eval_identity", caught.exception.detail)

    def test_missing_camera_field_is_rejected(self):
        for field in ("focal_length_mm", "sensor_width_mm", "sensor_height_mm"):
            payload = frame_payload(camera=camera_payload())
            del payload["camera"][field]
            with self.assertRaises(mapping.PayloadError) as caught:
                mapping.validate_message(payload)
            self.assertEqual(caught.exception.category, mapping.ERR_MISSING_FIELD)
            self.assertIn(field, caught.exception.detail)

    def test_camera_needs_a_basis_or_a_rotation(self):
        # Both are supplied by the Unreal publisher; either alone is enough.
        camera = camera_payload()
        for field in ("right", "up", "forward"):
            del camera[field]
        mapping.validate_message(frame_payload(camera=camera))
        camera = camera_payload()
        del camera["rotation"]
        mapping.validate_message(frame_payload(camera=camera))
        # Neither is a rejection, not a guess.
        camera = camera_payload()
        for field in ("right", "up", "forward", "rotation"):
            del camera[field]
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(frame_payload(camera=camera))
        self.assertEqual(caught.exception.category, mapping.ERR_MISSING_FIELD)
        # An incomplete basis is not silently mixed with a rotation.
        camera = camera_payload()
        del camera["up"]
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(frame_payload(camera=camera))

    def test_non_numeric_fields_are_rejected(self):
        payload = frame_payload()
        payload["camera"]["location"]["x"] = "far away"
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(payload)
        self.assertEqual(caught.exception.category, mapping.ERR_INVALID_FIELD)
        payload = frame_payload()
        payload["camera"]["right"] = {"x": 0.0, "y": True, "z": 0.0}
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(payload)
        payload = frame_payload()
        payload["camera"]["focal_length_mm"] = "50"
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(payload)

    def test_marker_fields_are_required(self):
        payload = frame_payload()
        del payload["markers"][0]["ndc"]
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(payload)
        self.assertIn("ndc", caught.exception.detail)
        payload = frame_payload()
        payload["markers"][0]["location"]["y"] = None
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(payload)

    def test_protocol_authority_and_version_are_enforced(self):
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(session_payload(version=mapping.PROTOCOL_VERSION + 1))
        self.assertEqual(caught.exception.category, mapping.ERR_UNSUPPORTED_PROTOCOL)
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(session_payload(version=1))
        self.assertEqual(caught.exception.category, mapping.ERR_UNSUPPORTED_PROTOCOL)
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(session_payload(protocol="MtoULiveLink"))
        self.assertEqual(caught.exception.category, mapping.ERR_UNSUPPORTED_PROTOCOL)
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.validate_message(session_payload(time_authority="maya"))
        self.assertEqual(caught.exception.category, mapping.ERR_UNSUPPORTED_AUTHORITY)

    def test_bad_resolution_and_rates_are_rejected(self):
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(session_payload(output_resolution={"x": 0, "y": 1080}))
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(session_payload(
                display_rate={"numerator": 30, "denominator": 0}))
        payload = frame_payload()
        payload["time"] = {"display_frame": "abc"}
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(payload)

    def test_expected_type_filter(self):
        with self.assertRaises(mapping.PayloadError):
            mapping.validate_message(session_payload(), expected_type="frame")
        mapping.validate_message(session_payload(), expected_type="session")

    def test_decode_message_rejects_broken_json(self):
        with self.assertRaises(mapping.PayloadError) as caught:
            mapping.decode_message("{not json")
        self.assertEqual(caught.exception.category, mapping.ERR_INVALID_FIELD)
        decoded = mapping.decode_message(
            '{"type": "end", "reason": "stopped"}\n')
        self.assertEqual(decoded["reason"], "stopped")


class CameraParametersTest(unittest.TestCase):

    def test_units_and_film_fit(self):
        attributes, notes = mapping.camera_parameters(
            camera_payload(), session_payload(), (1920.0, 1080.0))
        close(self, attributes["focalLength"], 50.0)
        close(self, attributes["horizontalFilmAperture"], 24.89 / 25.4)
        close(self, attributes["verticalFilmAperture"], 10.41 / 25.4)
        close(self, attributes["horizontalFilmOffset"], 0.5 / 25.4)
        close(self, attributes["verticalFilmOffset"], -0.25 / 25.4)
        self.assertEqual(attributes["filmFit"], mapping.FILM_FIT_HORIZONTAL)
        close(self, attributes["fStop"], 2.8)
        close(self, attributes["focusDistance"], 500.0)
        self.assertTrue(attributes["depthOfField"])
        close(self, attributes["nearClipPlane"], 10.0)
        close(self, attributes["lensSqueezeRatio"], 1.0)
        self.assertIn("MaintainXFOV", notes["film_fit_reason"])

    def test_far_clip_uses_the_payload_fallback(self):
        attributes, notes = mapping.camera_parameters(
            camera_payload(), session_payload(far_clip_fallback_cm=42000.0),
            (1920.0, 1080.0))
        close(self, attributes["farClipPlane"], 42000.0)
        self.assertTrue(notes["far_clip_substituted"])

    def test_far_clip_prefers_a_finite_camera_value(self):
        attributes, notes = mapping.camera_parameters(
            camera_payload(far_clip_cm=12345.0), session_payload(far_clip_fallback_cm=42000.0),
            (1920.0, 1080.0))
        close(self, attributes["farClipPlane"], 12345.0)
        self.assertFalse(notes["far_clip_substituted"])

    def test_far_clip_falls_back_to_the_documented_default(self):
        attributes, notes = mapping.camera_parameters(
            camera_payload(), {"output_resolution": {"x": 1920, "y": 1080}},
            (1920.0, 1080.0))
        close(self, attributes["farClipPlane"], mapping.DEFAULT_FAR_CLIP_CM)
        self.assertTrue(notes["far_clip_substituted"])

    def test_optional_attributes_are_omitted_when_not_declared(self):
        camera = camera_payload()
        for field in ("f_stop", "focus_distance_cm", "depth_of_field"):
            del camera[field]
        attributes, _ = mapping.camera_parameters(
            camera, session_payload(), (1920.0, 1080.0))
        for attribute in ("fStop", "focusDistance", "depthOfField"):
            self.assertNotIn(attribute, attributes)

    def test_invalid_values_are_rejected(self):
        for camera in (camera_payload(focal_length_mm=0.0),
                       camera_payload(sensor_width_mm=-1.0),
                       camera_payload(f_stop=0.0),
                       camera_payload(squeeze_factor=0.0),
                       camera_payload(focus_distance_cm=-5.0)):
            with self.assertRaises(mapping.PayloadError):
                mapping.camera_parameters(camera, session_payload(), (1920.0, 1080.0))


class MarkerReportTest(unittest.TestCase):

    def test_delta_against_unreal_ndc(self):
        matrix = mapping.ue_basis_to_maya_matrix(
            (0, 1, 0), (0, 0, 1), (1, 0, 0), (0, 0, 0))
        notes = {"focal_length_mm": 50.0, "aperture_inches": (0.5, 0.25),
                 "film_fit": mapping.FILM_FIT_HORIZONTAL, "film_offset_inches": (0.0, 0.0),
                 "lens_squeeze_ratio": 1.0}
        markers = [
            {"name": "Centre", "location": {"x": 100.0, "y": 0.0, "z": 0.0},
             "ndc": {"x": 0.005, "y": -0.002}},
            {"name": "Behind", "location": {"x": -100.0, "y": 0.0, "z": 0.0},
             "ndc": {"x": 0.0, "y": 0.0}},
        ]
        report = mapping.marker_report(markers, matrix, notes, (1000, 500))
        close(self, report[0]["maya_ndc"][0], 0.0)
        close(self, report[0]["delta"][0], -0.005)
        close(self, report[0]["delta"][1], 0.002)
        self.assertIsNone(report[1]["maya_ndc"])
        self.assertIn("UNPROJECTABLE", report[1]["error"])


if __name__ == "__main__":
    unittest.main()
