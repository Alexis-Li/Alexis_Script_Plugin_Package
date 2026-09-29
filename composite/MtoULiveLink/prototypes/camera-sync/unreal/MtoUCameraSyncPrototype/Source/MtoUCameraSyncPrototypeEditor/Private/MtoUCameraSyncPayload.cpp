// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#include "MtoUCameraSyncPayload.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
TSharedRef<FJsonObject> MtoUCameraSyncObject(double X, double Y)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), X);
	Object->SetNumberField(TEXT("y"), Y);
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncVector(const FVector& Value)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

/** Appends canonical `key=value;` entries in a fixed order; only text equality matters. */
struct FMtoUCameraSyncDigestBuilder
{
	FString Text;

	void Add(const TCHAR* Key, bool Value)
	{
		Text += FString::Printf(TEXT("%s=%d;"), Key, Value ? 1 : 0);
	}
	void Add(const TCHAR* Key, int32 Value)
	{
		Text += FString::Printf(TEXT("%s=%d;"), Key, Value);
	}
	void Add(const TCHAR* Key, double Value)
	{
		Text += FString::Printf(TEXT("%s=%.6f;"), Key, Value);
	}
	void Add(const TCHAR* Key, const FString& Value)
	{
		Text += FString::Printf(TEXT("%s=%s;"), Key, *Value);
	}
	void Add(const TCHAR* Key, const FVector& Value)
	{
		Text += FString::Printf(TEXT("%s=%.6f,%.6f,%.6f;"), Key, Value.X, Value.Y, Value.Z);
	}
	void Add(const TCHAR* Key, const FRotator& Value)
	{
		Text += FString::Printf(TEXT("%s=%.6f,%.6f,%.6f;"), Key, Value.Pitch, Value.Yaw, Value.Roll);
	}
	void Add(const TCHAR* Key, const FVector2D& Value)
	{
		Text += FString::Printf(TEXT("%s=%.6f,%.6f;"), Key, Value.X, Value.Y);
	}
};
}  // namespace

FString MtoUCameraSyncAxisConstraintName(uint8 Constraint)
{
	switch (static_cast<EAspectRatioAxisConstraint>(Constraint))
	{
	case EAspectRatioAxisConstraint::AspectRatio_MaintainYFOV:
		return TEXT("MaintainYFOV");
	case EAspectRatioAxisConstraint::AspectRatio_MajorAxisFOV:
		return TEXT("MaintainMajorAxisFOV");
	default:
		return TEXT("MaintainXFOV");
	}
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeFrameRate(const FFrameRate& Rate)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("numerator"), Rate.Numerator);
	Object->SetNumberField(TEXT("denominator"), Rate.Denominator);
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeDof(const FMtoUCameraSyncDofSample& Dof)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("b_override_focal_distance"), Dof.bOverrideFocalDistance);
	Object->SetNumberField(TEXT("focal_distance_cm"), Dof.FocalDistanceCm);
	Object->SetBoolField(TEXT("b_override_fstop"), Dof.bOverrideFStop);
	Object->SetNumberField(TEXT("fstop"), Dof.FStop);
	Object->SetBoolField(TEXT("b_override_sensor_width"), Dof.bOverrideSensorWidth);
	Object->SetNumberField(TEXT("sensor_width_mm"), Dof.SensorWidthMm);
	Object->SetBoolField(TEXT("b_override_squeeze_factor"), Dof.bOverrideSqueezeFactor);
	Object->SetNumberField(TEXT("squeeze_factor"), Dof.SqueezeFactor);
	Object->SetBoolField(TEXT("b_override_min_fstop"), Dof.bOverrideMinFStop);
	Object->SetNumberField(TEXT("min_fstop"), Dof.MinFStop);
	Object->SetBoolField(TEXT("b_override_blade_count"), Dof.bOverrideBladeCount);
	Object->SetNumberField(TEXT("blade_count"), Dof.BladeCount);
	Object->SetBoolField(TEXT("b_override_depth_blur_radius"), Dof.bOverrideDepthBlurRadius);
	Object->SetNumberField(TEXT("depth_blur_radius"), Dof.DepthBlurRadius);
	Object->SetBoolField(TEXT("b_override_depth_blur_amount"), Dof.bOverrideDepthBlurAmount);
	Object->SetNumberField(TEXT("depth_blur_amount"), Dof.DepthBlurAmount);
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeCamera(const FMtoUCameraSyncCameraSample& Camera)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Camera.Name);
	Object->SetStringField(TEXT("actor"), Camera.ActorName);
	Object->SetStringField(TEXT("component"), Camera.ComponentName);
	Object->SetStringField(TEXT("path"), Camera.Path);
	Object->SetBoolField(TEXT("cine_camera"), Camera.bCineCamera);

	Object->SetObjectField(TEXT("location"), [&Camera]()
	{
		const TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("x"), Camera.LocationCm.X);
		Location->SetNumberField(TEXT("y"), Camera.LocationCm.Y);
		Location->SetNumberField(TEXT("z"), Camera.LocationCm.Z);
		return Location;
	}());

	const TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
	Rotation->SetNumberField(TEXT("roll"), Camera.Rotation.Roll);
	Rotation->SetNumberField(TEXT("pitch"), Camera.Rotation.Pitch);
	Rotation->SetNumberField(TEXT("yaw"), Camera.Rotation.Yaw);
	Object->SetObjectField(TEXT("rotation"), Rotation);

	Object->SetObjectField(TEXT("right"), MtoUCameraSyncVector(Camera.Right));
	Object->SetObjectField(TEXT("up"), MtoUCameraSyncVector(Camera.Up));
	Object->SetObjectField(TEXT("forward"), MtoUCameraSyncVector(Camera.Forward));

	Object->SetObjectField(TEXT("component_location"), MtoUCameraSyncVector(Camera.ComponentLocationCm));
	Object->SetBoolField(TEXT("view_transform_differs"), Camera.bViewTransformDiffers);

	Object->SetNumberField(TEXT("focal_length_mm"), Camera.FocalLengthMm);
	Object->SetNumberField(TEXT("horizontal_fov_deg"), Camera.HorizontalFovDeg);
	Object->SetNumberField(TEXT("vertical_fov_deg"), Camera.VerticalFovDeg);

	Object->SetNumberField(TEXT("sensor_width_mm"), Camera.SensorWidthMm);
	Object->SetNumberField(TEXT("sensor_height_mm"), Camera.SensorHeightMm);
	Object->SetNumberField(TEXT("sensor_aspect_ratio"), Camera.SensorAspectRatio);
	Object->SetNumberField(TEXT("sensor_horizontal_offset_mm"), Camera.SensorHorizontalOffsetMm);
	Object->SetNumberField(TEXT("sensor_vertical_offset_mm"), Camera.SensorVerticalOffsetMm);

	Object->SetStringField(TEXT("aspect_axis_constraint"), Camera.AspectAxisConstraint);
	Object->SetBoolField(TEXT("constrain_aspect_ratio"), Camera.bConstrainAspectRatio);
	Object->SetNumberField(TEXT("rendered_aspect_ratio"), Camera.RenderedAspectRatio);

	Object->SetNumberField(TEXT("f_stop"), Camera.FStop);
	Object->SetStringField(TEXT("focus_method"), Camera.FocusMethod);
	Object->SetNumberField(TEXT("focus_distance_cm"), Camera.FocusDistanceCm);
	Object->SetNumberField(TEXT("manual_focus_distance_cm"), Camera.ManualFocusDistanceCm);
	Object->SetBoolField(TEXT("depth_of_field"), Camera.bDepthOfField);

	Object->SetNumberField(TEXT("squeeze_factor"), Camera.SqueezeFactor);
	const TSharedRef<FJsonObject> Lens = MakeShared<FJsonObject>();
	Lens->SetNumberField(TEXT("min_focal_mm"), Camera.MinFocalMm);
	Lens->SetNumberField(TEXT("max_focal_mm"), Camera.MaxFocalMm);
	Lens->SetNumberField(TEXT("min_fstop"), Camera.MinFStop);
	Lens->SetNumberField(TEXT("max_fstop"), Camera.MaxFStop);
	Lens->SetNumberField(TEXT("minimum_focus_distance_cm"), Camera.MinimumFocusDistanceCm);
	Object->SetObjectField(TEXT("lens"), Lens);

	const TSharedRef<FJsonObject> Crop = MakeShared<FJsonObject>();
	Crop->SetBoolField(TEXT("enabled"), Camera.bCropEnabled);
	Crop->SetNumberField(TEXT("aspect_ratio"), Camera.CropAspectRatio);
	Object->SetObjectField(TEXT("crop"), Crop);

	Object->SetNumberField(TEXT("near_clip_cm"), Camera.NearClipCm);
	Object->SetStringField(TEXT("near_clip_source"), Camera.NearClipSource);
	Object->SetNumberField(TEXT("custom_near_clip_cm"), Camera.CustomNearClipCm);
	Object->SetBoolField(TEXT("b_override_custom_near_clip"), Camera.bOverrideCustomNearClip);
	if (Camera.bHasFarClip)
	{
		Object->SetNumberField(TEXT("far_clip_cm"), Camera.FarClipCm);
	}
	else
	{
		Object->SetField(TEXT("far_clip_cm"), MakeShared<FJsonValueNull>());
	}

	Object->SetObjectField(TEXT("off_center_projection_offset"),
		MtoUCameraSyncObject(Camera.OffCenterProjectionOffset.X, Camera.OffCenterProjectionOffset.Y));
	Object->SetNumberField(TEXT("overscan"), Camera.Overscan);
	Object->SetNumberField(TEXT("overscan_resolution_fraction"), Camera.OverscanResolutionFraction);
	Object->SetNumberField(TEXT("post_process_blend_weight"), Camera.PostProcessBlendWeight);
	Object->SetObjectField(TEXT("dof"), MtoUCameraSyncSerializeDof(Camera.Dof));
	return Object;
}

FString MtoUCameraSyncCameraContentDigest(const FMtoUCameraSyncCameraSample& Camera)
{
	// One entry per field MtoUCameraSyncSerializeCamera publishes: any change the client
	// would apply is a change of the evaluation target, including a same-tick edit of the
	// camera this frame is already following.
	FMtoUCameraSyncDigestBuilder Digest;
	Digest.Add(TEXT("name"), Camera.Name);
	Digest.Add(TEXT("actor"), Camera.ActorName);
	Digest.Add(TEXT("component"), Camera.ComponentName);
	Digest.Add(TEXT("path"), Camera.Path);
	Digest.Add(TEXT("cine_camera"), Camera.bCineCamera);
	Digest.Add(TEXT("location"), Camera.LocationCm);
	Digest.Add(TEXT("rotation"), Camera.Rotation);
	Digest.Add(TEXT("right"), Camera.Right);
	Digest.Add(TEXT("up"), Camera.Up);
	Digest.Add(TEXT("forward"), Camera.Forward);
	Digest.Add(TEXT("component_location"), Camera.ComponentLocationCm);
	Digest.Add(TEXT("view_transform_differs"), Camera.bViewTransformDiffers);
	Digest.Add(TEXT("focal_length_mm"), Camera.FocalLengthMm);
	Digest.Add(TEXT("horizontal_fov_deg"), Camera.HorizontalFovDeg);
	Digest.Add(TEXT("vertical_fov_deg"), Camera.VerticalFovDeg);
	Digest.Add(TEXT("sensor_width_mm"), Camera.SensorWidthMm);
	Digest.Add(TEXT("sensor_height_mm"), Camera.SensorHeightMm);
	Digest.Add(TEXT("sensor_aspect_ratio"), Camera.SensorAspectRatio);
	Digest.Add(TEXT("sensor_horizontal_offset_mm"), Camera.SensorHorizontalOffsetMm);
	Digest.Add(TEXT("sensor_vertical_offset_mm"), Camera.SensorVerticalOffsetMm);
	Digest.Add(TEXT("aspect_axis_constraint"), Camera.AspectAxisConstraint);
	Digest.Add(TEXT("constrain_aspect_ratio"), Camera.bConstrainAspectRatio);
	Digest.Add(TEXT("rendered_aspect_ratio"), Camera.RenderedAspectRatio);
	Digest.Add(TEXT("f_stop"), Camera.FStop);
	Digest.Add(TEXT("focus_method"), Camera.FocusMethod);
	Digest.Add(TEXT("focus_distance_cm"), Camera.FocusDistanceCm);
	Digest.Add(TEXT("manual_focus_distance_cm"), Camera.ManualFocusDistanceCm);
	Digest.Add(TEXT("depth_of_field"), Camera.bDepthOfField);
	Digest.Add(TEXT("squeeze_factor"), Camera.SqueezeFactor);
	Digest.Add(TEXT("min_focal_mm"), Camera.MinFocalMm);
	Digest.Add(TEXT("max_focal_mm"), Camera.MaxFocalMm);
	Digest.Add(TEXT("min_fstop"), Camera.MinFStop);
	Digest.Add(TEXT("max_fstop"), Camera.MaxFStop);
	Digest.Add(TEXT("minimum_focus_distance_cm"), Camera.MinimumFocusDistanceCm);
	Digest.Add(TEXT("crop_enabled"), Camera.bCropEnabled);
	Digest.Add(TEXT("crop_aspect_ratio"), Camera.CropAspectRatio);
	Digest.Add(TEXT("near_clip_cm"), Camera.NearClipCm);
	Digest.Add(TEXT("near_clip_source"), Camera.NearClipSource);
	Digest.Add(TEXT("custom_near_clip_cm"), Camera.CustomNearClipCm);
	Digest.Add(TEXT("override_custom_near_clip"), Camera.bOverrideCustomNearClip);
	Digest.Add(TEXT("has_far_clip"), Camera.bHasFarClip);
	Digest.Add(TEXT("far_clip_cm"), Camera.FarClipCm);
	Digest.Add(TEXT("off_center_projection_offset"), Camera.OffCenterProjectionOffset);
	Digest.Add(TEXT("overscan"), Camera.Overscan);
	Digest.Add(TEXT("overscan_resolution_fraction"), Camera.OverscanResolutionFraction);
	Digest.Add(TEXT("post_process_blend_weight"), Camera.PostProcessBlendWeight);
	Digest.Add(TEXT("dof_override_focal_distance"), Camera.Dof.bOverrideFocalDistance);
	Digest.Add(TEXT("dof_focal_distance_cm"), Camera.Dof.FocalDistanceCm);
	Digest.Add(TEXT("dof_override_fstop"), Camera.Dof.bOverrideFStop);
	Digest.Add(TEXT("dof_fstop"), Camera.Dof.FStop);
	Digest.Add(TEXT("dof_override_sensor_width"), Camera.Dof.bOverrideSensorWidth);
	Digest.Add(TEXT("dof_sensor_width_mm"), Camera.Dof.SensorWidthMm);
	Digest.Add(TEXT("dof_override_squeeze_factor"), Camera.Dof.bOverrideSqueezeFactor);
	Digest.Add(TEXT("dof_squeeze_factor"), Camera.Dof.SqueezeFactor);
	Digest.Add(TEXT("dof_override_min_fstop"), Camera.Dof.bOverrideMinFStop);
	Digest.Add(TEXT("dof_min_fstop"), Camera.Dof.MinFStop);
	Digest.Add(TEXT("dof_override_blade_count"), Camera.Dof.bOverrideBladeCount);
	Digest.Add(TEXT("dof_blade_count"), Camera.Dof.BladeCount);
	Digest.Add(TEXT("dof_override_depth_blur_radius"), Camera.Dof.bOverrideDepthBlurRadius);
	Digest.Add(TEXT("dof_depth_blur_radius"), Camera.Dof.DepthBlurRadius);
	Digest.Add(TEXT("dof_override_depth_blur_amount"), Camera.Dof.bOverrideDepthBlurAmount);
	Digest.Add(TEXT("dof_depth_blur_amount"), Camera.Dof.DepthBlurAmount);
	return Digest.Text;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeCut(const FMtoUCameraSyncCutSample& Cut)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("has_track"), Cut.bHasTrack);
	Object->SetNumberField(TEXT("section_count"), Cut.SectionCount);
	Object->SetBoolField(TEXT("active"), Cut.bActive);
	Object->SetNumberField(TEXT("active_index"), Cut.ActiveIndex);
	Object->SetStringField(TEXT("stage"), Cut.Stage);
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeTime(const FMtoUCameraSyncTimeSample& Time)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("display_frame"), Time.DisplayFrame);
	Object->SetNumberField(TEXT("source_frame"), Time.SourceFrame);
	Object->SetNumberField(TEXT("seconds"), Time.Seconds);
	Object->SetNumberField(TEXT("tick"), static_cast<double>(Time.Tick));
	Object->SetObjectField(TEXT("display_rate"), MtoUCameraSyncSerializeFrameRate(Time.DisplayRate));
	Object->SetObjectField(TEXT("tick_resolution"), MtoUCameraSyncSerializeFrameRate(Time.TickResolution));
	Object->SetNumberField(TEXT("playback_start"), Time.PlaybackStart);
	Object->SetNumberField(TEXT("playback_end"), Time.PlaybackEnd);
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeView(const FMtoUCameraSyncViewSample& View)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
	Location->SetNumberField(TEXT("x"), View.LocationCm.X);
	Location->SetNumberField(TEXT("y"), View.LocationCm.Y);
	Location->SetNumberField(TEXT("z"), View.LocationCm.Z);
	Object->SetObjectField(TEXT("location"), Location);
	const TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
	Rotation->SetNumberField(TEXT("roll"), View.Rotation.Roll);
	Rotation->SetNumberField(TEXT("pitch"), View.Rotation.Pitch);
	Rotation->SetNumberField(TEXT("yaw"), View.Rotation.Yaw);
	Object->SetObjectField(TEXT("rotation"), Rotation);
	Object->SetNumberField(TEXT("fov_deg"), View.FovDeg);
	Object->SetNumberField(TEXT("aspect_ratio"), View.AspectRatio);
	Object->SetStringField(TEXT("aspect_axis_constraint"), View.AspectAxisConstraint);
	Object->SetBoolField(TEXT("constrain_aspect_ratio"), View.bConstrainAspectRatio);
	Object->SetObjectField(TEXT("off_center_projection_offset"),
		MtoUCameraSyncObject(View.OffCenterProjectionOffset.X, View.OffCenterProjectionOffset.Y));
	Object->SetNumberField(TEXT("overscan"), View.Overscan);
	Object->SetNumberField(TEXT("overscan_resolution_fraction"), View.OverscanResolutionFraction);
	Object->SetNumberField(TEXT("perspective_near_clip_cm"), View.PerspectiveNearClipCm);
	if (View.bHasPerspectiveFarClip)
	{
		Object->SetNumberField(TEXT("perspective_far_clip_cm"), View.PerspectiveFarClipCm);
	}
	else
	{
		Object->SetField(TEXT("perspective_far_clip_cm"), MakeShared<FJsonValueNull>());
	}
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeProjection(const FMtoUCameraSyncProjectionSample& Projection)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Matrix;
	Matrix.Reserve(16);
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			Matrix.Add(MakeShared<FJsonValueNumber>(Projection.ViewProjection.M[Row][Column]));
		}
	}
	Object->SetArrayField(TEXT("view_proj"), Matrix);

	const TSharedRef<FJsonObject> Rect = MakeShared<FJsonObject>();
	Rect->SetNumberField(TEXT("min_x"), Projection.ViewRect.Min.X);
	Rect->SetNumberField(TEXT("min_y"), Projection.ViewRect.Min.Y);
	Rect->SetNumberField(TEXT("max_x"), Projection.ViewRect.Max.X);
	Rect->SetNumberField(TEXT("max_y"), Projection.ViewRect.Max.Y);
	Object->SetObjectField(TEXT("view_rect"), Rect);
	return Object;
}

TArray<TSharedPtr<FJsonValue>> MtoUCameraSyncSerializeMarkers(const TArray<FMtoUCameraSyncMarkerSample>& Markers)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Markers.Num());
	for (const FMtoUCameraSyncMarkerSample& Marker : Markers)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Marker.Name);
		const TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("x"), Marker.LocationCm.X);
		Location->SetNumberField(TEXT("y"), Marker.LocationCm.Y);
		Location->SetNumberField(TEXT("z"), Marker.LocationCm.Z);
		Entry->SetObjectField(TEXT("location"), Location);
		Entry->SetBoolField(TEXT("projected"), Marker.bProjected);
		Entry->SetObjectField(TEXT("ndc"), MtoUCameraSyncObject(Marker.Ndc.X, Marker.Ndc.Y));
		Entry->SetObjectField(TEXT("pixel"), MtoUCameraSyncObject(Marker.Pixel.X, Marker.Pixel.Y));
		Entry->SetBoolField(TEXT("inside"),
			Marker.bProjected && FMath::Abs(Marker.Ndc.X) <= 1.0 && FMath::Abs(Marker.Ndc.Y) <= 1.0);
		Values.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Values;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeFrame(const FMtoUCameraSyncFrameSample& Frame)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("frame"));
	Object->SetNumberField(TEXT("frame_serial"), static_cast<double>(Frame.Serial));
	Object->SetNumberField(TEXT("eval_serial"), static_cast<double>(Frame.EvalSerial));
	Object->SetStringField(TEXT("eval_identity"), Frame.EvalIdentity);
	Object->SetStringField(TEXT("sequence"), Frame.SequenceName);
	Object->SetStringField(TEXT("sequence_path"), Frame.SequencePath);
	Object->SetObjectField(TEXT("output_resolution"), MtoUCameraSyncObject(
		Frame.OutputResolution.X, Frame.OutputResolution.Y));
	Object->SetObjectField(TEXT("aperture_resolution"), MtoUCameraSyncObject(
		Frame.ApertureResolution.X, Frame.ApertureResolution.Y));
	Object->SetStringField(TEXT("resolution_source"), Frame.ResolutionSource);
	Object->SetObjectField(TEXT("time"), MtoUCameraSyncSerializeTime(Frame.Time));
	Object->SetObjectField(TEXT("camera_cut"), MtoUCameraSyncSerializeCut(Frame.Cut));
	Object->SetObjectField(TEXT("camera"), MtoUCameraSyncSerializeCamera(Frame.Camera));
	Object->SetObjectField(TEXT("view"), MtoUCameraSyncSerializeView(Frame.View));
	Object->SetObjectField(TEXT("projection"), MtoUCameraSyncSerializeProjection(Frame.Projection));
	Object->SetArrayField(TEXT("markers"), MtoUCameraSyncSerializeMarkers(Frame.Markers));
	return Object;
}

TSharedRef<FJsonObject> MtoUCameraSyncSerializeSession(const FMtoUCameraSyncSessionSample& Session)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("session"));
	Object->SetStringField(TEXT("protocol"), TEXT("MtoUCameraSync"));
	Object->SetNumberField(TEXT("version"), 1);
	Object->SetStringField(TEXT("time_authority"), TEXT("unreal"));
	Object->SetStringField(TEXT("sequence"), Session.SequenceName);
	Object->SetStringField(TEXT("sequence_path"), Session.SequencePath);
	Object->SetObjectField(TEXT("display_rate"), MtoUCameraSyncSerializeFrameRate(Session.DisplayRate));
	Object->SetObjectField(TEXT("tick_resolution"), MtoUCameraSyncSerializeFrameRate(Session.TickResolution));
	const TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
	Range->SetNumberField(TEXT("start"), Session.PlaybackStart);
	Range->SetNumberField(TEXT("end"), Session.PlaybackEnd);
	Object->SetObjectField(TEXT("playback_range"), Range);
	Object->SetObjectField(TEXT("output_resolution"), MtoUCameraSyncObject(
		Session.OutputResolution.X, Session.OutputResolution.Y));
	Object->SetObjectField(TEXT("aperture_resolution"), MtoUCameraSyncObject(
		Session.ApertureResolution.X, Session.ApertureResolution.Y));
	Object->SetStringField(TEXT("resolution_source"), Session.ResolutionSource);
	Object->SetNumberField(TEXT("far_clip_fallback_cm"), Session.FarClipFallbackCm);
	Object->SetObjectField(TEXT("camera_cut"), MtoUCameraSyncSerializeCut(Session.Cut));
	TArray<TSharedPtr<FJsonValue>> MarkerNames;
	MarkerNames.Reserve(Session.MarkerNames.Num());
	for (const FString& Name : Session.MarkerNames)
	{
		MarkerNames.Add(MakeShared<FJsonValueString>(Name));
	}
	Object->SetArrayField(TEXT("marker_names"), MarkerNames);
	return Object;
}
