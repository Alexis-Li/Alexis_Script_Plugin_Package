#include "MtoULinkageEditor.h"

#include "MtoULiveLinkActor.h"

#include "MtoULinkage.h"

#include "Subsystems/AssetEditorSubsystem.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CineCameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTrack.h"
#include "SceneView.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieScene3DAttachTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "UnrealClient.h"
#include "UniversalObjectLocator.h"
#include "UniversalObjectLocatorResolveParams.h"
#include "Widgets/SWeakWidget.h"

namespace UE::MovieScene
{
    struct FSharedPlaybackState;
}

namespace
{
/** The render gate when no editor viewport owns one; reported as its own source. */
constexpr int32 DefaultGateWidth = 1920;
constexpr int32 DefaultGateHeight = 1080;
/** Bounded sub-sequence walk: a real shot hierarchy is shallow. */
constexpr int32 MaxSubSequenceDepth = 4;

FString AxisConstraintName(uint8 Constraint)
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

FString FocusMethodName(ECameraFocusMethod Method)
{
    switch (Method)
    {
    case ECameraFocusMethod::Manual:
        return TEXT("Manual");
    case ECameraFocusMethod::Tracking:
        return TEXT("Tracking");
    case ECameraFocusMethod::Disable:
        return TEXT("Disable");
    default:
        return TEXT("DoNotOverride");
    }
}

/**
 * The pixel rectangle the film aperture occupies inside the output resolution.
 * A camera that constrains its aspect ratio renders its aperture inside the
 * gate and leaves the remaining pixels as bars, which is what the Maya
 * resolution gate has to reproduce.
 */
FIntRect ComputeApertureRect(const FIntRect& FullRect, const FMinimalViewInfo& ViewInfo)
{
    if (!ViewInfo.bConstrainAspectRatio || ViewInfo.AspectRatio <= 0.0f
        || FullRect.Width() <= 0 || FullRect.Height() <= 0)
    {
        return FullRect;
    }
    const double GateAspect = static_cast<double>(FullRect.Width()) / FullRect.Height();
    if (ViewInfo.AspectRatio >= GateAspect)
    {
        const int32 Height = FMath::Clamp(
            FMath::RoundToInt(FullRect.Width() / ViewInfo.AspectRatio), 1, FullRect.Height());
        const int32 Top = FullRect.Min.Y + (FullRect.Height() - Height) / 2;
        return FIntRect(FullRect.Min.X, Top, FullRect.Max.X, Top + Height);
    }
    const int32 Width = FMath::Clamp(
        FMath::RoundToInt(FullRect.Height() * ViewInfo.AspectRatio), 1, FullRect.Width());
    const int32 Left = FullRect.Min.X + (FullRect.Width() - Width) / 2;
    return FIntRect(Left, FullRect.Min.Y, Left + Width, FullRect.Max.Y);
}

/** Every camera value the payload publishes, as the camera-sync prototype names them. */
struct FMtoUCameraSample
{
    FString Name;
    FString ActorName;
    FString ComponentName;
    FString Path;
    bool bCineCamera = false;

    FVector LocationCm = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Right = FVector::ForwardVector;
    FVector Up = FVector::UpVector;
    FVector Forward = FVector::ForwardVector;
    FVector ComponentLocationCm = FVector::ZeroVector;
    bool bViewTransformDiffers = false;
    FString ProjectionMode = TEXT("perspective");

    double FocalLengthMm = 0.0;
    double HorizontalFovDeg = 0.0;
    double VerticalFovDeg = 0.0;

    double SensorWidthMm = 0.0;
    double SensorHeightMm = 0.0;
    double SensorAspectRatio = 0.0;
    double SensorHorizontalOffsetMm = 0.0;
    double SensorVerticalOffsetMm = 0.0;

    FString AspectAxisConstraint = TEXT("MaintainXFOV");
    bool bConstrainAspectRatio = false;
    double RenderedAspectRatio = 0.0;

    double FStop = 0.0;
    FString FocusMethod = TEXT("DoNotOverride");
    double FocusDistanceCm = 0.0;
    double ManualFocusDistanceCm = 0.0;
    bool bDepthOfField = false;

    double SqueezeFactor = 1.0;
    double MinFocalMm = 0.0;
    double MaxFocalMm = 0.0;
    double MinFStop = 0.0;
    double MaxFStop = 0.0;
    double MinimumFocusDistanceCm = 0.0;

    bool bCropEnabled = false;
    double CropAspectRatio = 0.0;

    double NearClipCm = 0.0;
    FString NearClipSource = TEXT("view_info");
    double CustomNearClipCm = 0.0;
    bool bOverrideCustomNearClip = false;
    bool bHasFarClip = false;
    double FarClipCm = 0.0;

    FVector2D OffCenterProjectionOffset = FVector2D::ZeroVector;
    double Overscan = 0.0;
    double OverscanResolutionFraction = 1.0;
    double PostProcessBlendWeight = 1.0;

    bool bOverrideFocalDistance = false;
    double DofFocalDistanceCm = 0.0;
    bool bOverrideFStop = false;
    double DofFStop = 0.0;
    bool bOverrideSensorWidth = false;
    double DofSensorWidthMm = 0.0;
    bool bOverrideSqueezeFactor = false;
    double DofSqueezeFactor = 1.0;
    bool bOverrideMinFStop = false;
    double DofMinFStop = 0.0;
    bool bOverrideBladeCount = false;
    int32 DofBladeCount = 0;
    bool bOverrideDepthBlurRadius = false;
    double DofDepthBlurRadius = 0.0;
    bool bOverrideDepthBlurAmount = false;
    double DofDepthBlurAmount = 0.0;
};

/** Reads the evaluated state of one camera component as the renderer receives it. */
bool CaptureCameraSample(
    UCameraComponent& Camera,
    const FIntPoint& OutputResolution,
    FMtoUCameraSample& Out,
    FString& OutError)
{
    FMinimalViewInfo ViewInfo;
    Camera.GetCameraView(0.0f, ViewInfo);
    if (OutputResolution.X <= 0 || OutputResolution.Y <= 0)
    {
        OutError = TEXT("the resolved output resolution is not positive");
        return false;
    }

    const UCineCameraComponent* Cine = Cast<UCineCameraComponent>(&Camera);

    Out = FMtoUCameraSample();
    Out.Name = Camera.GetName();
    Out.ComponentName = Camera.GetName();
    Out.Path = Camera.GetPathName();
    Out.bCineCamera = Cine != nullptr;
    if (const AActor* Owner = Camera.GetOwner())
    {
        Out.ActorName = Owner->GetActorNameOrLabel();
        Out.Name = Owner->GetActorNameOrLabel();
    }

    // The view transform is what renders, so it is what Maya has to reproduce.
    Out.LocationCm = ViewInfo.Location;
    Out.Rotation = ViewInfo.Rotation;
    Out.Right = ViewInfo.Rotation.RotateVector(FVector::RightVector);
    Out.Up = ViewInfo.Rotation.RotateVector(FVector::UpVector);
    Out.Forward = ViewInfo.Rotation.RotateVector(FVector::ForwardVector);
    Out.ComponentLocationCm = Camera.GetComponentLocation();
    Out.bViewTransformDiffers = !Out.ComponentLocationCm.Equals(Out.LocationCm, 0.1);
    Out.ProjectionMode = ViewInfo.ProjectionMode == ECameraProjectionMode::Orthographic
        ? FString(TEXT("orthographic"))
        : FString(TEXT("perspective"));

    Out.AspectAxisConstraint = AxisConstraintName(
        ViewInfo.AspectRatioAxisConstraint.Get(EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV));
    Out.bConstrainAspectRatio = ViewInfo.bConstrainAspectRatio != 0;
    Out.RenderedAspectRatio = Out.bConstrainAspectRatio
        ? ViewInfo.AspectRatio
        : static_cast<double>(OutputResolution.X) / static_cast<double>(OutputResolution.Y);

    Out.NearClipCm = ViewInfo.GetFinalPerspectiveNearClipPlane();
    Out.NearClipSource = ViewInfo.PerspectiveNearClipPlane > 0.0f
        ? FString(TEXT("view_info"))
        : FString(TEXT("engine_default"));
    Out.OffCenterProjectionOffset = ViewInfo.OffCenterProjectionOffset;
    Out.Overscan = ViewInfo.GetOverscan();
    Out.OverscanResolutionFraction = ViewInfo.OverscanResolutionFraction;
    Out.PostProcessBlendWeight = Camera.PostProcessBlendWeight;

    if (Cine)
    {
        Out.FocalLengthMm = Cine->CurrentFocalLength;
        Out.FStop = Cine->CurrentAperture;
        Out.HorizontalFovDeg = Cine->GetHorizontalFieldOfView();
        Out.VerticalFovDeg = Cine->GetVerticalFieldOfView();

        Out.SensorWidthMm = Cine->Filmback.SensorWidth;
        Out.SensorHeightMm = Cine->Filmback.SensorHeight;
        Out.SensorAspectRatio = Cine->Filmback.SensorAspectRatio;
        Out.SensorHorizontalOffsetMm = Cine->Filmback.SensorHorizontalOffset;
        Out.SensorVerticalOffsetMm = Cine->Filmback.SensorVerticalOffset;

        Out.FocusMethod = FocusMethodName(Cine->FocusSettings.FocusMethod);
        Out.ManualFocusDistanceCm = Cine->FocusSettings.ManualFocusDistance;
        Out.bDepthOfField = Cine->FocusSettings.FocusMethod != ECameraFocusMethod::Disable;

        Out.SqueezeFactor = Cine->LensSettings.SqueezeFactor;
        Out.MinFocalMm = Cine->LensSettings.MinFocalLength;
        Out.MaxFocalMm = Cine->LensSettings.MaxFocalLength;
        Out.MinFStop = Cine->LensSettings.MinFStop;
        Out.MaxFStop = Cine->LensSettings.MaxFStop;
        Out.MinimumFocusDistanceCm = Cine->LensSettings.MinimumFocusDistance;

        Out.CropAspectRatio = Cine->CropSettings.AspectRatio;
        Out.bCropEnabled = Cine->CropSettings.AspectRatio > 0.0f;

        Out.CustomNearClipCm = Cine->CustomNearClippingPlane;
        Out.bOverrideCustomNearClip = Cine->bOverride_CustomNearClippingPlane;
    }
    else
    {
        Out.FocalLengthMm = 0.0;
        Out.HorizontalFovDeg = ViewInfo.FOV;
        Out.VerticalFovDeg = ViewInfo.FOV;
        Out.bDepthOfField = ViewInfo.PostProcessSettings.bOverride_DepthOfFieldFstop != 0;
    }

    const FPostProcessSettings& PostProcess = ViewInfo.PostProcessSettings;
    Out.FocusDistanceCm = PostProcess.bOverride_DepthOfFieldFocalDistance
        ? PostProcess.DepthOfFieldFocalDistance
        : (Cine ? Cine->CurrentFocusDistance : 0.0);
    // The camera's view information carries no far plane in this engine
    // generation, so the field is reported as unbounded and the receiving side
    // substitutes its documented default instead of guessing a number here.
    Out.bHasFarClip = false;
    Out.FarClipCm = 0.0;

    Out.bOverrideFocalDistance = PostProcess.bOverride_DepthOfFieldFocalDistance != 0;
    Out.DofFocalDistanceCm = PostProcess.DepthOfFieldFocalDistance;
    Out.bOverrideFStop = PostProcess.bOverride_DepthOfFieldFstop != 0;
    Out.DofFStop = PostProcess.DepthOfFieldFstop;
    Out.bOverrideSensorWidth = PostProcess.bOverride_DepthOfFieldSensorWidth != 0;
    Out.DofSensorWidthMm = PostProcess.DepthOfFieldSensorWidth;
    Out.bOverrideSqueezeFactor = PostProcess.bOverride_DepthOfFieldSqueezeFactor != 0;
    Out.DofSqueezeFactor = PostProcess.DepthOfFieldSqueezeFactor;
    Out.bOverrideMinFStop = PostProcess.bOverride_DepthOfFieldMinFstop != 0;
    Out.DofMinFStop = PostProcess.DepthOfFieldMinFstop;
    Out.bOverrideBladeCount = PostProcess.bOverride_DepthOfFieldBladeCount != 0;
    Out.DofBladeCount = PostProcess.DepthOfFieldBladeCount;
    Out.bOverrideDepthBlurRadius = PostProcess.bOverride_DepthOfFieldDepthBlurRadius != 0;
    Out.DofDepthBlurRadius = PostProcess.DepthOfFieldDepthBlurRadius;
    Out.bOverrideDepthBlurAmount = PostProcess.bOverride_DepthOfFieldDepthBlurAmount != 0;
    Out.DofDepthBlurAmount = PostProcess.DepthOfFieldDepthBlurAmount;
    return true;
}

TSharedRef<FJsonObject> SerializeVector(const FVector& Value)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("x"), Value.X);
    Object->SetNumberField(TEXT("y"), Value.Y);
    Object->SetNumberField(TEXT("z"), Value.Z);
    return Object;
}

TSharedRef<FJsonObject> SerializeVector2(const FVector2D& Value)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("x"), Value.X);
    Object->SetNumberField(TEXT("y"), Value.Y);
    return Object;
}

TSharedRef<FJsonObject> SerializeCamera(const FMtoUCameraSample& Camera)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Camera.Name);
    Object->SetStringField(TEXT("actor"), Camera.ActorName);
    Object->SetStringField(TEXT("component"), Camera.ComponentName);
    Object->SetStringField(TEXT("path"), Camera.Path);
    Object->SetBoolField(TEXT("cine_camera"), Camera.bCineCamera);
    Object->SetStringField(TEXT("projection_mode"), Camera.ProjectionMode);

    Object->SetObjectField(TEXT("location"), SerializeVector(Camera.LocationCm));
    const TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("roll"), Camera.Rotation.Roll);
    Rotation->SetNumberField(TEXT("pitch"), Camera.Rotation.Pitch);
    Rotation->SetNumberField(TEXT("yaw"), Camera.Rotation.Yaw);
    Object->SetObjectField(TEXT("rotation"), Rotation);
    Object->SetObjectField(TEXT("right"), SerializeVector(Camera.Right));
    Object->SetObjectField(TEXT("up"), SerializeVector(Camera.Up));
    Object->SetObjectField(TEXT("forward"), SerializeVector(Camera.Forward));
    Object->SetObjectField(TEXT("component_location"), SerializeVector(Camera.ComponentLocationCm));
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
        SerializeVector2(Camera.OffCenterProjectionOffset));
    Object->SetNumberField(TEXT("overscan"), Camera.Overscan);
    Object->SetNumberField(TEXT("overscan_resolution_fraction"),
        Camera.OverscanResolutionFraction);
    Object->SetNumberField(TEXT("post_process_blend_weight"), Camera.PostProcessBlendWeight);

    const TSharedRef<FJsonObject> Dof = MakeShared<FJsonObject>();
    Dof->SetBoolField(TEXT("b_override_focal_distance"), Camera.bOverrideFocalDistance);
    Dof->SetNumberField(TEXT("focal_distance_cm"), Camera.DofFocalDistanceCm);
    Dof->SetBoolField(TEXT("b_override_fstop"), Camera.bOverrideFStop);
    Dof->SetNumberField(TEXT("fstop"), Camera.DofFStop);
    Dof->SetBoolField(TEXT("b_override_sensor_width"), Camera.bOverrideSensorWidth);
    Dof->SetNumberField(TEXT("sensor_width_mm"), Camera.DofSensorWidthMm);
    Dof->SetBoolField(TEXT("b_override_squeeze_factor"), Camera.bOverrideSqueezeFactor);
    Dof->SetNumberField(TEXT("squeeze_factor"), Camera.DofSqueezeFactor);
    Dof->SetBoolField(TEXT("b_override_min_fstop"), Camera.bOverrideMinFStop);
    Dof->SetNumberField(TEXT("min_fstop"), Camera.DofMinFStop);
    Dof->SetBoolField(TEXT("b_override_blade_count"), Camera.bOverrideBladeCount);
    Dof->SetNumberField(TEXT("blade_count"), Camera.DofBladeCount);
    Dof->SetBoolField(TEXT("b_override_depth_blur_radius"), Camera.bOverrideDepthBlurRadius);
    Dof->SetNumberField(TEXT("depth_blur_radius"), Camera.DofDepthBlurRadius);
    Dof->SetBoolField(TEXT("b_override_depth_blur_amount"), Camera.bOverrideDepthBlurAmount);
    Dof->SetNumberField(TEXT("depth_blur_amount"), Camera.DofDepthBlurAmount);
    Object->SetObjectField(TEXT("dof"), Dof);
    return Object;
}
/** Appends canonical `key=value;` entries in a fixed order; only text equality matters. */
struct FMtoUDigestBuilder
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
        Text += FString::Printf(TEXT("%s=%.6f,%.6f,%.6f;"),
            Key, Value.Pitch, Value.Yaw, Value.Roll);
    }
    void Add(const TCHAR* Key, const FVector2D& Value)
    {
        Text += FString::Printf(TEXT("%s=%.6f,%.6f;"), Key, Value.X, Value.Y);
    }
};

/**
 * Canonical text of every camera value the payload publishes, compared for
 * equality only. The evaluation identity carries it, so editing the same camera
 * at the same sequence time (transform, focal length, filmback, offsets, clip
 * planes, depth of field) starts a new target generation instead of leaving
 * the old content converged.
 */
FString CameraContentDigest(const FMtoUCameraSample& Camera)
{
    FMtoUDigestBuilder Digest;
    Digest.Add(TEXT("name"), Camera.Name);
    Digest.Add(TEXT("actor"), Camera.ActorName);
    Digest.Add(TEXT("component"), Camera.ComponentName);
    Digest.Add(TEXT("path"), Camera.Path);
    Digest.Add(TEXT("cine_camera"), Camera.bCineCamera);
    Digest.Add(TEXT("projection_mode"), Camera.ProjectionMode);
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
    Digest.Add(TEXT("dof_override_focal_distance"), Camera.bOverrideFocalDistance);
    Digest.Add(TEXT("dof_focal_distance_cm"), Camera.DofFocalDistanceCm);
    Digest.Add(TEXT("dof_override_fstop"), Camera.bOverrideFStop);
    Digest.Add(TEXT("dof_fstop"), Camera.DofFStop);
    Digest.Add(TEXT("dof_override_sensor_width"), Camera.bOverrideSensorWidth);
    Digest.Add(TEXT("dof_sensor_width_mm"), Camera.DofSensorWidthMm);
    Digest.Add(TEXT("dof_override_squeeze_factor"), Camera.bOverrideSqueezeFactor);
    Digest.Add(TEXT("dof_squeeze_factor"), Camera.DofSqueezeFactor);
    Digest.Add(TEXT("dof_override_min_fstop"), Camera.bOverrideMinFStop);
    Digest.Add(TEXT("dof_min_fstop"), Camera.DofMinFStop);
    Digest.Add(TEXT("dof_override_blade_count"), Camera.bOverrideBladeCount);
    Digest.Add(TEXT("dof_blade_count"), Camera.DofBladeCount);
    Digest.Add(TEXT("dof_override_depth_blur_radius"), Camera.bOverrideDepthBlurRadius);
    Digest.Add(TEXT("dof_depth_blur_radius"), Camera.DofDepthBlurRadius);
    Digest.Add(TEXT("dof_override_depth_blur_amount"), Camera.bOverrideDepthBlurAmount);
    Digest.Add(TEXT("dof_depth_blur_amount"), Camera.DofDepthBlurAmount);
    return Digest.Text;
}

/**
 * The resolution gate: the aperture extent is what Maya writes to
 * `defaultResolution`, because Maya derives the axis it does not keep from the
 * gate aspect, so sending the full gate instead would change the field of view
 * (measured 0.25-0.38 NDC error on the vertical axis). The full render gate and
 * the flat width/height spelling travel with it so one object serves both the
 * apply and the restore path.
 */
TSharedRef<FJsonObject> SerializeGate(
    const FIntPoint& OutputResolution,
    const FIntRect& ApertureRect,
    const FString& ResolutionSource,
    const FMtoUCameraSample& Camera)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    const TSharedRef<FJsonObject> Output = MakeShared<FJsonObject>();
    Output->SetNumberField(TEXT("x"), OutputResolution.X);
    Output->SetNumberField(TEXT("y"), OutputResolution.Y);
    Object->SetObjectField(TEXT("output_resolution"), Output);

    const TSharedRef<FJsonObject> Aperture = MakeShared<FJsonObject>();
    Aperture->SetNumberField(TEXT("x"), ApertureRect.Width());
    Aperture->SetNumberField(TEXT("y"), ApertureRect.Height());
    Object->SetObjectField(TEXT("aperture_resolution"), Aperture);

    Object->SetStringField(TEXT("resolution_source"), ResolutionSource);
    const double ApertureAspect = ApertureRect.Height() > 0
        ? static_cast<double>(ApertureRect.Width()) / ApertureRect.Height()
        : 0.0;
    Object->SetNumberField(TEXT("aspect_ratio"), ApertureAspect);
    Object->SetNumberField(TEXT("overscan"), Camera.Overscan);
    Object->SetNumberField(TEXT("overscan_resolution_fraction"),
        Camera.OverscanResolutionFraction);

    Object->SetNumberField(TEXT("width"), ApertureRect.Width());
    Object->SetNumberField(TEXT("height"), ApertureRect.Height());
    Object->SetNumberField(TEXT("pixel_aspect_ratio"), ApertureAspect);
    return Object;
}

/** Which camera cut the evaluated camera came from, and the camera it names. */
struct FMtoUCutSample
{
    bool bHasTrack = false;
    int32 SectionCount = 0;
    bool bActive = false;
    int32 ActiveIndex = INDEX_NONE;
    FString Stage = TEXT("none");
    FString Name = TEXT("-");
};

FMtoUCutSample DescribeCut(
    const UMovieSceneSequence* Sequence,
    const FFrameRate& DisplayRate,
    const FFrameRate& TickResolution,
    double DisplayFrame,
    const UCameraComponent* Camera)
{
    FMtoUCutSample Cut;
    const UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
    if (!MovieScene)
    {
        return Cut;
    }
    const UMovieSceneTrack* Track = MovieScene->GetCameraCutTrack();
    if (!Track)
    {
        return Cut;
    }
    Cut.bHasTrack = true;
    const TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
    Cut.SectionCount = Sections.Num();
    const FFrameNumber Tick = TickResolution.AsFrameNumber(DisplayFrame / DisplayRate.AsDecimal());
    for (int32 Index = 0; Index < Sections.Num(); ++Index)
    {
        if (Sections[Index] && Sections[Index]->GetRange().Contains(Tick))
        {
            Cut.bActive = true;
            Cut.ActiveIndex = Index;
            Cut.Stage = TEXT("root");
            break;
        }
    }
    if (Camera)
    {
        Cut.Name = Camera->GetOwner()
            ? Camera->GetOwner()->GetActorNameOrLabel()
            : Camera->GetName();
    }
    if (!Cut.bActive)
    {
        // The engine may still resolve a cut owned by a subsequence instance.
        Cut.Stage = Camera != nullptr ? FString(TEXT("subsequence")) : FString(TEXT("pending"));
    }
    return Cut;
}

/** The render gate the editor owns: the active viewport when there is one. */
FIntPoint ResolveOutputResolution(FString& OutSource)
{
    if (GEditor && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
    {
        if (const FViewport* Viewport = GEditor->GetActiveViewport())
        {
            const FIntPoint Size = Viewport->GetSizeXY();
            if (Size.X > 0 && Size.Y > 0)
            {
                OutSource = TEXT("editor_viewport");
                return Size;
            }
        }
    }
    OutSource = TEXT("default");
    return FIntPoint(DefaultGateWidth, DefaultGateHeight);
}

/** Root sequence and every sub sequence its tracks reach, within a bounded depth. */
void GatherSequences(UMovieSceneSequence* Sequence, TArray<UMovieSceneSequence*>& Out, int32 Depth)
{
    if (!Sequence || Depth > MaxSubSequenceDepth || Out.Contains(Sequence))
    {
        return;
    }
    Out.Add(Sequence);
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        return;
    }
    for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        const UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track);
        if (!SubTrack)
        {
            continue;
        }
        for (const UMovieSceneSection* Section : SubTrack->GetAllSections())
        {
            const UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
            if (SubSection)
            {
                GatherSequences(SubSection->GetSequence(), Out, Depth + 1);
            }
        }
    }
}

/** The engine branch one writer drives; the panel and the tests read the label. */
FString WriterKind(const UMovieSceneTrack* Track)
{
    if (Cast<UMovieSceneSkeletalAnimationTrack>(Track))
    {
        return TEXT("skeletal_animation");
    }
    if (Cast<UMovieScene3DTransformTrack>(Track))
    {
        return TEXT("transform");
    }
    if (Cast<UMovieScene3DAttachTrack>(Track))
    {
        return TEXT("attach");
    }
    if (Cast<UMovieSceneVisibilityTrack>(Track))
    {
        return TEXT("visibility");
    }
    // The Control Rig track lives in the ControlRig plugin, which this plugin
    // does not declare as a dependency; the label is read from the class name so
    // the mute/restore below stays class-agnostic for it as well.
    if (Track->GetClass()->GetName().Contains(TEXT("ControlRigParameter")))
    {
        return TEXT("control_rig");
    }
    if (Cast<UMovieScenePropertyTrack>(Track))
    {
        return TEXT("property");
    }
    return TEXT("other");
}

void CollectComponentSubtree(const USceneComponent* Component, TSet<const USceneComponent*>& Out)
{
    if (!Component || Out.Contains(Component))
    {
        return;
    }
    Out.Add(Component);
    for (const TObjectPtr<USceneComponent>& Child : Component->GetAttachChildren())
    {
        CollectComponentSubtree(Child.Get(), Out);
    }
}

void CollectActorContent(const AActor* Actor,
    TSet<const UObject*>& OutObjects, TSet<const USceneComponent*>& OutComponents)
{
    if (!Actor)
    {
        return;
    }
    OutObjects.Add(Actor);
    for (const UActorComponent* Component : Actor->GetComponents())
    {
        if (!Component)
        {
            continue;
        }
        OutObjects.Add(Component);
        if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
        {
            CollectComponentSubtree(SceneComponent, OutComponents);
        }
    }
}

/** Everything one object's writer moves: itself, plus what hangs under it. */
void CollectDrivenContent(const UObject* Resolved,
    TSet<const UObject*>& OutObjects, TSet<const USceneComponent*>& OutComponents)
{
    if (!Resolved)
    {
        return;
    }
    if (const AActor* Actor = Cast<AActor>(Resolved))
    {
        CollectActorContent(Actor, OutObjects, OutComponents);
        return;
    }
    OutObjects.Add(Resolved);
    if (const USceneComponent* Component = Cast<USceneComponent>(Resolved))
    {
        CollectComponentSubtree(Component, OutComponents);
    }
}

bool ContainsTarget(const TSet<const UObject*>& Objects, const TSet<const USceneComponent*>& Components,
    const TArray<UObject*>& TargetObjects)
{
    for (const UObject* Target : TargetObjects)
    {
        if (!Target)
        {
            continue;
        }
        if (Objects.Contains(Target))
        {
            return true;
        }
        if (const USceneComponent* AsComponent = Cast<USceneComponent>(Target))
        {
            if (Components.Contains(AsComponent))
            {
                return true;
            }
        }
    }
    return false;
}

/** Names the v10 rejection code for one refusal, or an empty string. */
const TCHAR* CodeWriterNotIsolatable = TEXT("WRITER_NOT_ISOLATABLE");
const TCHAR* CodeSharedWriter = TEXT("SHARED_WRITER");
const TCHAR* CodeMovingSocket = TEXT("MOVING_SOCKET");
const TCHAR* CodeTargetUnknown = TEXT("TARGET_UNKNOWN");

/** What one takeover recorded, restored per resource and idempotently. */
struct FMtoUMutedWriter
{
    TWeakObjectPtr<UMovieSceneTrack> Track;
    bool bWholeTrackMuted = false;
    TArray<TPair<int32, bool>> Rows;
    FString Name;
    bool bRestored = false;
};

/** The only module state: the writers of the current takeover and the last evaluation. */
struct FMtoULinkageEditorState
{
    TArray<FMtoUMutedWriter> Writers;
    bool bTakeoverActive = false;
    bool bHooksInstalled = false;

    FString SequenceKey;
    FString EvalIdentity;
    int32 EvalSerial = 0;
    // Counts the discrete events of this sequence. It is part of every
    // evaluation identity, so an event (a pause exactly on the target in
    // flight, an edit that keeps the time) always names new work.
    int32 EventEpoch = 0;
    bool bTimeEvaluated = false;
    double LastEvaluatedDisplayFrame = -1.0;
    // The previous target this module produced, so a new one can be told
    // apart as continuous playback or a discrete event.
    bool bHasLastTarget = false;
    bool bLastTargetPlaying = false;
    double LastTargetDisplayFrame = 0.0;
    double LastTargetSeconds = 0.0;
    // Set when the user edits the evaluated camera, its actor or the open
    // sequence; consumed by the next target as a discrete event.
    bool bPendingDiscontinuity = false;
    TWeakObjectPtr<const UCameraComponent> LastCamera;
    FDelegateHandle PropertyChangedHandle;
    FDelegateHandle ActorMovedHandle;

    /**
     * The source the current takeover muted, held weakly: the restore
     * re-evaluates it only while it still exists, and the validation notices
     * when the sequence, its editor, its world or the target went away.
     */
    TWeakObjectPtr<ULevelSequence> TakeoverSequence;
    TWeakObjectPtr<UWorld> TakeoverWorld;
    TWeakPtr<ISequencer> TakeoverSequencer;
    TWeakObjectPtr<ULevelSequencePlayer> TakeoverPlayer;
    TWeakObjectPtr<const AActor> TakeoverAnchor;
    bool bTakeoverUsedSequencer = false;

    TWeakObjectPtr<AActor> ObservedActor;
    TArray<TWeakPtr<SWidget>> PanelWidgets;
    FSimpleMulticastDelegate ViewChanged;
};

FMtoULinkageEditorState& GetState()
{
    static FMtoULinkageEditorState State;
    return State;
}

double DisplayFrameOfTick(const FFrameRate& TickResolution, const FFrameRate& DisplayRate,
    const FFrameNumber& Tick)
{
    return TickResolution.AsSeconds(FFrameTime(Tick)) * DisplayRate.AsDecimal();
}

void ForceSequenceEvaluation(const FMtoULinkageEditor::FEvaluationSource& Source)
{
    if (Source.Sequencer.IsValid())
    {
        Source.Sequencer->ForceEvaluate();
        return;
    }
    if (Source.Player)
    {
        // A jump to the position the reader already holds re-evaluates without
        // moving the playhead, which is all the restore needs.
        const FQualifiedFrameTime Current = Source.Player->GetCurrentTime();
        Source.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
            Current.Time, EUpdatePositionMethod::Jump));
    }
}
}
bool FMtoULinkageEditor::EvaluateCameraTime(
    const FEvaluationSource& Source, FMtoUCameraTimeTarget& OutTarget, FString& OutError)
{
    OutError.Reset();
    OutTarget = FMtoUCameraTimeTarget();
    if (!Source.Sequence)
    {
        OutError = TEXT("no Level Sequence is open");
        return false;
    }
    const UMovieScene* MovieScene = Source.Sequence->GetMovieScene();
    if (!MovieScene)
    {
        OutError = TEXT("the open Level Sequence has no Movie Scene");
        return false;
    }
    if (!Source.Sequencer.IsValid() && !Source.Player)
    {
        OutError = TEXT("the open Level Sequence has no evaluation session");
        return false;
    }

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    if (DisplayRate.Numerator <= 0 || DisplayRate.Denominator <= 0
        || TickResolution.Numerator <= 0 || TickResolution.Denominator <= 0)
    {
        OutError = TEXT("the open Level Sequence has a non-positive display or tick rate");
        return false;
    }
    const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
    const FFrameNumber PlaybackStartTick = Range.HasLowerBound()
        ? Range.GetLowerBoundValue() : FFrameNumber(0);
    const FFrameNumber PlaybackEndTick = Range.HasUpperBound()
        ? Range.GetUpperBoundValue() : FFrameNumber(0);
    const double PlaybackStart = DisplayFrameOfTick(TickResolution, DisplayRate, PlaybackStartTick);
    const double PlaybackEnd = DisplayFrameOfTick(TickResolution, DisplayRate, PlaybackEndTick);

    const FQualifiedFrameTime Current = Source.Sequencer.IsValid()
        ? Source.Sequencer->GetGlobalTime()
        : Source.Player->GetCurrentTime();
    const FFrameTime DisplayTime = FFrameRate::TransformTime(Current.Time, Current.Rate, DisplayRate);
    const double DisplayFrame = DisplayTime.AsDecimal();
    const FFrameTime TickTime = FFrameRate::TransformTime(Current.Time, Current.Rate, TickResolution);

    // The editor evaluates on its own tick, so a moved playhead can still report the
    // previous shot's camera. One forced evaluation keeps the resolved camera in step
    // with the sampled time before the target leaves this module: the open Sequencer
    // re-evaluates, and an owned player jumps to the position it already holds, which
    // re-evaluates without moving the playhead.
    FMtoULinkageEditorState& State = GetState();
    const FString SequenceKey = Source.Sequence->GetPathName();
    if (SequenceKey != State.SequenceKey)
    {
        State.SequenceKey = SequenceKey;
        State.EvalIdentity.Reset();
        State.EvalSerial = 0;
        State.EventEpoch = 0;
        State.bTimeEvaluated = false;
        State.bHasLastTarget = false;
    }
    if (Source.Sequencer.IsValid())
    {
        const bool bTimeChanged = !State.bTimeEvaluated
            || !FMath::IsNearlyEqual(State.LastEvaluatedDisplayFrame, DisplayFrame, 1e-6);
        if (bTimeChanged)
        {
            Source.Sequencer->ForceEvaluate();
            State.bTimeEvaluated = true;
            State.LastEvaluatedDisplayFrame = DisplayFrame;
        }
    }
    else if (Source.Player)
    {
        Source.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
            Current.Time, EUpdatePositionMethod::Jump));
    }

    const UCameraComponent* Camera = Source.Sequencer.IsValid()
        ? Source.Sequencer->GetLastEvaluatedCameraCut().Get()
        : Source.Player->GetActiveCameraComponent();
    if (!Camera)
    {
        OutError = TEXT("the sequence has no evaluated camera at the current time");
        return false;
    }

    FString ResolutionSource;
    const FIntPoint OutputResolution = ResolveOutputResolution(ResolutionSource);
    FMtoUCameraSample CameraSample;
    if (!CaptureCameraSample(
            *const_cast<UCameraComponent*>(Camera), OutputResolution, CameraSample, OutError))
    {
        return false;
    }

    // A heartbeat repeats a target and keeps its evaluation serial. Only a real jump,
    // cut, camera change or change of the camera content starts a new generation and
    // supersedes in-flight results.
    const int64 Tick = TickTime.GetFrame().Value;
    const int32 MilliTick = FMath::Clamp(
        FMath::RoundToInt(TickTime.GetSubFrame() * 1000.0f), 0, 999);
    const FString ContentDigest = CameraContentDigest(CameraSample);
    const FString Sample = FString::Printf(TEXT("%s@%lld+%03d/%s#%s"),
        *Source.Sequence->GetName(), Tick, MilliTick,
        Camera->GetPathName().IsEmpty() ? TEXT("-") : *Camera->GetPathName(), *ContentDigest);
    const bool bPlaying = Source.Sequencer.IsValid()
        ? Source.Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing
        : Source.Player->IsPlaying();
    const double PlayRate = Source.Sequencer.IsValid()
        ? Source.Sequencer->GetPlaybackSpeed()
        : Source.Player->GetPlayRate();
    const double NowSeconds = FPlatformTime::Seconds();
    const bool bIdentityChanged = Sample != State.EvalIdentity;
    bool bDiscontinuous = false;
    if (State.bHasLastTarget)
    {
        if (State.bPendingDiscontinuity)
        {
            // The user edited the camera, its actor or the sequence.
            bDiscontinuous = true;
        }
        else if (State.bLastTargetPlaying && !bPlaying)
        {
            // A pause cancels the work in flight even when it stops exactly on
            // the target that was already waiting.
            bDiscontinuous = true;
        }
        else if (bIdentityChanged && !bPlaying)
        {
            // Scrub, jump (also within one shot) or a change while paused.
            bDiscontinuous = true;
        }
        else if (bIdentityChanged)
        {
            // While playing, the time must advance as playback advances it;
            // a backward step (loop wrap) or a step the elapsed time does not
            // explain is a jump.
            const double Step = DisplayFrame - State.LastTargetDisplayFrame;
            const double Expected = State.bLastTargetPlaying
                ? (NowSeconds - State.LastTargetSeconds) * DisplayRate.AsDecimal()
                    * FMath::Abs(PlayRate)
                : 0.0;
            const double Tolerance = FMath::Max(1.5, Expected * 0.5);
            bDiscontinuous = Step < 0.0 || FMath::Abs(Step - Expected) > Tolerance;
        }
    }
    State.bPendingDiscontinuity = false;
    if (bDiscontinuous)
    {
        ++State.EventEpoch;
    }
    if (bIdentityChanged || bDiscontinuous)
    {
        State.EvalIdentity = Sample;
        ++State.EvalSerial;
    }
    const FString Identity = FString::Printf(TEXT("%s~e%d"), *Sample, State.EventEpoch);
    State.bHasLastTarget = true;
    State.bLastTargetPlaying = bPlaying;
    State.LastTargetDisplayFrame = DisplayFrame;
    State.LastTargetSeconds = NowSeconds;
    State.LastCamera = Camera;

    const FMtoUCutSample Cut = DescribeCut(
        Source.Sequence, DisplayRate, TickResolution, DisplayFrame, Camera);

    FMinimalViewInfo ViewInfo;
    const_cast<UCameraComponent*>(Camera)->GetCameraView(0.0f, ViewInfo);
    const FIntRect ApertureRect = ComputeApertureRect(
        FIntRect(FIntPoint::ZeroValue, OutputResolution), ViewInfo);

    OutTarget.EvalId = Identity;
    OutTarget.EvalSerial = State.EvalSerial;
    // TransportSerial and FrameSerial stay at zero: the runtime owns the send counters.
    OutTarget.DisplayFrame = DisplayFrame;
    OutTarget.Seconds = (DisplayFrame - PlaybackStart) / FMath::Max(DisplayRate.AsDecimal(), 1.0);
    OutTarget.PlaybackRangeStart = FMath::FloorToInt32(PlaybackStart);
    OutTarget.PlaybackRangeEnd = FMath::FloorToInt32(PlaybackEnd);
    OutTarget.DisplayRateNumerator = DisplayRate.Numerator;
    OutTarget.DisplayRateDenominator = DisplayRate.Denominator;
    OutTarget.TickNumerator = TickResolution.Numerator;
    OutTarget.TickDenominator = TickResolution.Denominator;
    OutTarget.CutStage = Cut.Stage;
    OutTarget.CutName = Cut.Name;
    OutTarget.bDiscontinuous = bDiscontinuous;
    OutTarget.Camera = SerializeCamera(CameraSample);
    OutTarget.Gate = SerializeGate(
        OutputResolution, ApertureRect, ResolutionSource, CameraSample);
    return true;
}
TArray<FMtoULinkageEditor::FWriterRecord> FMtoULinkageEditor::CollectTargetWriters(
    const FEvaluationSource& Source,
    const TArray<UObject*>& TargetObjects,
    const AActor* TargetAnchor,
    FString& OutError)
{
    OutError.Reset();
    TArray<FWriterRecord> Records;
    if (!Source.Sequence)
    {
        OutError = TEXT("no Level Sequence is open");
        return Records;
    }
    if (TargetObjects.IsEmpty() || !TargetAnchor)
    {
        OutError = TEXT("the paired target resolves to no owned components");
        return Records;
    }

    // The paired target is the anchor actor, its own root motion and the target
    // components; a writer that reaches anything beyond that drives another
    // object and cannot be muted without touching it.
    UWorld* World = Source.World;
    if (!World)
    {
        OutError = TEXT("the paired target has no world to resolve its sequence against");
        return Records;
    }
    TSet<const UObject*> AllowedObjects;
    TSet<const USceneComponent*> AllowedComponents;
    AllowedObjects.Add(TargetAnchor);
    if (const USceneComponent* AnchorRoot = TargetAnchor->GetRootComponent())
    {
        AllowedObjects.Add(AnchorRoot);
        AllowedComponents.Add(AnchorRoot);
    }
    for (const UObject* Target : TargetObjects)
    {
        if (!Target)
        {
            continue;
        }
        AllowedObjects.Add(Target);
        if (const USceneComponent* Component = Cast<USceneComponent>(Target))
        {
            AllowedComponents.Add(Component);
        }
    }

    TArray<UMovieSceneSequence*> Sequences;
    GatherSequences(Source.Sequence, Sequences, 0);
    for (UMovieSceneSequence* Sequence : Sequences)
    {
        const UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
        if (!MovieScene)
        {
            continue;
        }
        const UE::UniversalObjectLocator::FResolveParams ResolveParams(World);
        for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
        {
            TArray<UObject*, TInlineAllocator<1>> Resolved;
            Sequence->LocateBoundObjects(Binding.GetObjectGuid(), ResolveParams,
                TSharedPtr<const UE::MovieScene::FSharedPlaybackState>(), Resolved);

            TSet<const UObject*> DrivenObjects;
            TSet<const USceneComponent*> DrivenComponents;
            for (UObject* Object : Resolved)
            {
                CollectDrivenContent(Object, DrivenObjects, DrivenComponents);
            }
            if (DrivenObjects.IsEmpty() && DrivenComponents.IsEmpty())
            {
                // A binding nothing resolves to cannot be proved to belong to the
                // target, so its writers stay untouched instead of muted blind.
                continue;
            }
            if (!ContainsTarget(DrivenObjects, DrivenComponents, TargetObjects))
            {
                continue;
            }
            for (const UObject* Object : DrivenObjects)
            {
                if (!Object || AllowedObjects.Contains(Object))
                {
                    continue;
                }
                OutError = FString::Printf(
                    TEXT("a writer of the target also drives '%s', which is not part of the paired target"),
                    *Object->GetPathName());
                return Records;
            }
            for (const USceneComponent* Component : DrivenComponents)
            {
                if (!Component || AllowedComponents.Contains(Component))
                {
                    continue;
                }
                OutError = FString::Printf(
                    TEXT("a writer of the target also drives '%s', which is not part of the paired target"),
                    *Component->GetPathName());
                return Records;
            }

            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (!Track)
                {
                    continue;
                }
                FWriterRecord Record;
                Record.Track = Track;
                Record.Kind = WriterKind(Track);
                Record.BindingName = Track->GetName();
                const int32 MaxRow = Track->GetMaxRowIndex();
                for (int32 Row = 0; Row <= MaxRow; ++Row)
                {
                    if (!Track->IsRowEvalDisabled(Row, /*bInCheckLocal=*/true))
                    {
                        Record.Rows.Add(Row);
                    }
                }
                Records.Add(MoveTemp(Record));
            }
        }
    }
    return Records;
}

bool FMtoULinkageEditor::TakeOverTarget(
    const FEvaluationSource& Source,
    const TArray<UObject*>& TargetObjects,
    const AActor* TargetAnchor,
    FString& OutError,
    FString& OutErrorCode)
{
    OutError.Reset();
    OutErrorCode.Reset();
    if (!Source.Sequence)
    {
        OutError = TEXT("no Level Sequence is open");
        OutErrorCode = CodeWriterNotIsolatable;
        return false;
    }
    if (TargetObjects.IsEmpty() || !TargetAnchor)
    {
        OutError = TEXT("the paired target resolves to no owned components");
        OutErrorCode = CodeTargetUnknown;
        return false;
    }

    // A socket already carries the parent motion, so the Maya world root pose
    // would be applied on top of a motion this module cannot mute selectively.
    for (const UObject* Object : TargetObjects)
    {
        const USceneComponent* Component = Cast<USceneComponent>(const_cast<UObject*>(Object));
        if (Component && Component->GetAttachSocketName() != NAME_None)
        {
            OutError = FString::Printf(
                TEXT("the target '%s' is attached at socket '%s'; a socket carries the parent motion, so the Maya world root pose would be applied twice"),
                *Object->GetName(), *Component->GetAttachSocketName().ToString());
            OutErrorCode = CodeMovingSocket;
            return false;
        }
    }

    if (Source.Sequencer.IsValid()
        && ULevelSequenceEditorBlueprintLibrary::IsLevelSequenceLocked())
    {
        OutError = TEXT("the open Level Sequence is locked for editing; its writers cannot be muted");
        OutErrorCode = CodeWriterNotIsolatable;
        return false;
    }

    FString InventoryError;
    const TArray<FWriterRecord> Writers =
        CollectTargetWriters(Source, TargetObjects, TargetAnchor, InventoryError);
    if (!InventoryError.IsEmpty())
    {
        // The inventory refused because one writer reaches beyond the target.
        OutError = InventoryError;
        OutErrorCode = CodeSharedWriter;
        return false;
    }

    FMtoULinkageEditorState& State = GetState();
    for (const FWriterRecord& Writer : Writers)
    {
        UMovieSceneTrack* Track = Writer.Track.Get();
        if (!Track)
        {
            continue;
        }
        const int32 Existing = State.Writers.IndexOfByPredicate(
            [Track](const FMtoUMutedWriter& Candidate) { return Candidate.Track == Track; });
        if (Existing != INDEX_NONE)
        {
            // A second takeover of the same writers is idempotent: the recorded
            // prior state stays the one the restore returns to.
            continue;
        }
        FMtoUMutedWriter Muted;
        Muted.Track = Track;
        Muted.Name = Writer.Kind + TEXT(":") + Writer.BindingName;
        Muted.bWholeTrackMuted = !Track->IsEvalDisabled(/*bInCheckLocal=*/true);
        for (const int32 Row : Writer.Rows)
        {
            Muted.Rows.Emplace(Row, Track->IsRowEvalDisabled(Row, /*bInCheckLocal=*/true));
        }
        // The local flag is never serialized with the asset, so a takeover can
        // never mute a Level Sequence permanently.
        if (Muted.bWholeTrackMuted)
        {
            Track->SetLocalEvalDisabled(true);
        }
        for (const TPair<int32, bool>& Row : Muted.Rows)
        {
            Track->SetLocalRowEvalDisabled(true, Row.Key);
        }
        State.Writers.Add(MoveTemp(Muted));
    }
    State.bTakeoverActive = true;
    State.TakeoverSequence = Source.Sequence;
    State.TakeoverWorld = Source.World;
    State.TakeoverSequencer = Source.Sequencer;
    State.TakeoverPlayer = Source.Player;
    State.TakeoverAnchor = TargetAnchor;
    State.bTakeoverUsedSequencer = Source.Sequencer.IsValid();

    // Everything the writers owned is released: one evaluation re-establishes
    // what drives the target now instead of leaving a stale pose behind.
    ForceSequenceEvaluation(Source);
    return true;
}

bool FMtoULinkageEditor::RestoreTarget(FString& OutError, FString& OutErrorCode)
{
    OutError.Reset();
    OutErrorCode.Reset();
    FMtoULinkageEditorState& State = GetState();
    if (!State.bTakeoverActive)
    {
        // Restores are idempotent per resource: a second call has nothing left.
        return true;
    }
    bool bRestoredAny = false;
    TArray<FString> Residuals;
    for (FMtoUMutedWriter& Muted : State.Writers)
    {
        if (Muted.bRestored)
        {
            continue;
        }
        UMovieSceneTrack* Track = Muted.Track.Get();
        if (!Track)
        {
            Residuals.Add(FString::Printf(
                TEXT("the muted writer '%s' no longer exists, so its saved state could not be returned"),
                *Muted.Name));
            Muted.bRestored = true;
            continue;
        }
        if (Muted.bWholeTrackMuted)
        {
            Track->SetLocalEvalDisabled(false);
        }
        for (const TPair<int32, bool>& Row : Muted.Rows)
        {
            Track->SetLocalRowEvalDisabled(Row.Value, Row.Key);
        }
        Muted.bRestored = true;
        bRestoredAny = true;
    }
    State.Writers.Reset();
    State.bTakeoverActive = false;
    if (bRestoredAny)
    {
        // Every writer is enabled again: one evaluation re-establishes each
        // writer's own pose instead of leaving the preview's last pose behind.
        // A closed editor or a destroyed player has nothing left to evaluate.
        FMtoULinkageEditor::FEvaluationSource Source;
        Source.Sequence = State.TakeoverSequence.Get();
        Source.World = State.TakeoverWorld.Get();
        Source.Sequencer = State.TakeoverSequencer.Pin();
        Source.Player = State.TakeoverPlayer.Get();
        if (Source.Sequence && (Source.Sequencer.IsValid() || Source.Player))
        {
            ForceSequenceEvaluation(Source);
        }
    }
    State.TakeoverSequence.Reset();
    State.TakeoverWorld.Reset();
    State.TakeoverSequencer.Reset();
    State.TakeoverPlayer.Reset();
    State.TakeoverAnchor.Reset();
    State.bTakeoverUsedSequencer = false;
    if (!Residuals.IsEmpty())
    {
        OutError = FString::Join(Residuals, TEXT("; "));
        OutErrorCode = TEXT("RECOVERY_REFUSED");
        return false;
    }
    return true;
}
bool FMtoULinkageEditor::ValidateTakeover(FString& OutReason)
{
    OutReason.Reset();
    const FMtoULinkageEditorState& State = GetState();
    if (!State.bTakeoverActive)
    {
        return true;
    }
    const ULevelSequence* Sequence = State.TakeoverSequence.Get();
    if (!Sequence)
    {
        OutReason = TEXT("the Level Sequence the takeover muted was unloaded");
        return false;
    }
    if (State.bTakeoverUsedSequencer)
    {
        const TSharedPtr<ISequencer> Sequencer = State.TakeoverSequencer.Pin();
        if (!Sequencer.IsValid() || Sequencer->GetRootMovieSceneSequence() != Sequence)
        {
            OutReason = FString::Printf(
                TEXT("the Level Sequence '%s' was closed in the editor"), *Sequence->GetName());
            return false;
        }
        if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
        {
            OutReason = FString::Printf(
                TEXT("the editor switched from the Level Sequence '%s' to another sequence"),
                *Sequence->GetName());
            return false;
        }
    }
    else if (!State.TakeoverPlayer.IsValid())
    {
        OutReason = FString::Printf(
            TEXT("the player of the Level Sequence '%s' was destroyed"), *Sequence->GetName());
        return false;
    }
    const UWorld* World = State.TakeoverWorld.Get();
    if (!World || World->bIsTearingDown)
    {
        OutReason = TEXT("the world of the takeover was closed");
        return false;
    }
    const AActor* Anchor = State.TakeoverAnchor.Get();
    if (!IsValid(Anchor) || Anchor->GetWorld() != World)
    {
        OutReason = TEXT("the paired target was destroyed or left the takeover's world");
        return false;
    }
    return true;
}

/** The open editor Sequencer of one sequence, when the sequence is open in one. */
TSharedPtr<ISequencer> FindOpenSequencer(const ULevelSequence* Sequence)
{
    if (!GEditor || !Sequence)
    {
        return nullptr;
    }
    UAssetEditorSubsystem* AssetEditorSubsystem =
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        return nullptr;
    }
    for (IAssetEditorInstance* EditorInstance :
        AssetEditorSubsystem->FindEditorsForAssetAndSubObjects(
            const_cast<ULevelSequence*>(Sequence)))
    {
        if (!EditorInstance
            || EditorInstance->GetEditorName() != FName(TEXT("LevelSequenceEditor")))
        {
            continue;
        }
        ILevelSequenceEditorToolkit* Toolkit =
            static_cast<ILevelSequenceEditorToolkit*>(EditorInstance);
        if (Toolkit)
        {
            return Toolkit->GetSequencer();
        }
    }
    return nullptr;
}

bool ResolveOpenEvaluationSource(
    UWorld* PreferredWorld, FMtoULinkageEditor::FEvaluationSource& Out)
{
    Out = FMtoULinkageEditor::FEvaluationSource();
    ULevelSequence* Sequence =
        ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
    if (!Sequence)
    {
        return false;
    }
    const TSharedPtr<ISequencer> Sequencer = FindOpenSequencer(Sequence);
    if (!Sequencer.IsValid())
    {
        return false;
    }
    Out.Sequence = Sequence;
    Out.Sequencer = Sequencer;
    Out.World = PreferredWorld;
    if (!Out.World && GEditor)
    {
        Out.World = GEditor->GetEditorWorldContext().World();
    }
    return Out.World != nullptr;
}

/** The components the paired Binding Actor's drives: its primary plus its parts. */
TArray<UObject*> ResolveTargetComponents(const AActor* Actor)
{
    TArray<UObject*> Components;
    if (!Actor)
    {
        return Components;
    }
    if (const AMtoULiveLinkActor* BindingActor = Cast<AMtoULiveLinkActor>(Actor))
    {
        if (USkeletalMeshComponent* Primary = BindingActor->GetSkeletalMeshComponent())
        {
            Components.Add(Primary);
        }
        for (const TObjectPtr<UMtoUCharacterPartComponent>& Part :
            BindingActor->GetCharacterPartComponents())
        {
            if (Part)
            {
                Components.Add(Part.Get());
            }
        }
        return Components;
    }
    for (UActorComponent* Component : Actor->GetComponents())
    {
        if (Component && Component->IsA<USceneComponent>())
        {
            Components.Add(Component);
        }
    }
    return Components;
}

/** The paired anchor: the pairing the panel remembered, or the runtime's own path. */
AActor* ResolvePairedAnchor()
{
    FMtoULinkageEditorState& State = GetState();
    if (const FMtoULinkageView View = MtoUGetLinkageView(); View.bPaired)
    {
        if (AActor* Observed = State.ObservedActor.Get())
        {
            return Observed;
        }
        if (!View.TargetPath.IsEmpty())
        {
            return FindObject<AActor>(nullptr, *View.TargetPath);
        }
    }
    return nullptr;
}

bool EvaluateCameraTimeHook(FMtoUCameraTimeTarget& OutTarget)
{
    FMtoULinkageEditor::FEvaluationSource Source;
    if (!ResolveOpenEvaluationSource(nullptr, Source))
    {
        return false;
    }
    FString Error;
    return FMtoULinkageEditor::EvaluateCameraTime(Source, OutTarget, Error);
}

bool TakeOverTargetHook(FString& OutError, FString& OutErrorCode)
{
    const AActor* Anchor = ResolvePairedAnchor();
    if (!Anchor)
    {
        OutError = TEXT("the paired target no longer resolves");
        OutErrorCode = CodeTargetUnknown;
        return false;
    }
    FMtoULinkageEditor::FEvaluationSource Source;
    if (!ResolveOpenEvaluationSource(Anchor->GetWorld(), Source))
    {
        OutError = TEXT("no open Level Sequence owns the paired target");
        OutErrorCode = CodeWriterNotIsolatable;
        return false;
    }
    return FMtoULinkageEditor::TakeOverTarget(
        Source, ResolveTargetComponents(Anchor), Anchor, OutError, OutErrorCode);
}

bool RestoreTargetHook(FString& OutError, FString& OutErrorCode)
{
    return FMtoULinkageEditor::RestoreTarget(OutError, OutErrorCode);
}

bool ValidateTakeoverHook(FString& OutReason)
{
    return FMtoULinkageEditor::ValidateTakeover(OutReason);
}

/** An edit of the evaluated camera, its actor or the open sequence. */
void NoteEditedObject(const UObject* Object)
{
    FMtoULinkageEditorState& State = GetState();
    if (!Object || !State.bHasLastTarget)
    {
        return;
    }
    const UCameraComponent* Camera = State.LastCamera.Get();
    const AActor* CameraActor = Camera ? Camera->GetOwner() : nullptr;
    const UObject* Sequence = State.SequenceKey.IsEmpty()
        ? nullptr
        : FindObject<UObject>(nullptr, *State.SequenceKey);
    if ((Camera && (Object == Camera || Object->IsIn(Camera)))
        || (CameraActor && (Object == CameraActor || Object->IsIn(CameraActor)))
        || (Sequence && (Object == Sequence || Object->IsIn(Sequence))))
    {
        State.bPendingDiscontinuity = true;
    }
}

void OnObjectPropertyChangedHook(UObject* Object, FPropertyChangedEvent& Event)
{
    (void)Event;
    NoteEditedObject(Object);
}

void OnActorMovedHook(AActor* Actor)
{
    NoteEditedObject(Actor);
}

void OnViewChangedHook(const FMtoULinkageView& View)
{
    FMtoULinkageEditorState& State = GetState();
    State.ViewChanged.Broadcast();
    // A panel that was closed leaves its weak reference behind; a broadcast
    // purges those, so the registered set stays the live panels.
    State.PanelWidgets.RemoveAll(
        [](const TWeakPtr<SWidget>& Widget) { return !Widget.IsValid(); });
    for (const TWeakPtr<SWidget>& Widget : State.PanelWidgets)
    {
        if (TSharedPtr<SWidget> Pinned = Widget.Pin())
        {
            Pinned->Invalidate(EInvalidateWidgetReason::Paint);
        }
    }
}

void FMtoULinkageEditor::InstallHooks()
{
    FMtoULinkageEditorState& State = GetState();
    if (State.bHooksInstalled)
    {
        return;
    }
    FMtoULinkageHooks Hooks;
    Hooks.EvaluateCameraTime = &EvaluateCameraTimeHook;
    Hooks.TakeOverTarget = &TakeOverTargetHook;
    Hooks.RestoreTarget = &RestoreTargetHook;
    Hooks.ValidateTakeover = &ValidateTakeoverHook;
    Hooks.OnViewChanged = &OnViewChangedHook;
    MtoUSetLinkageHooks(Hooks);
    State.PropertyChangedHandle =
        FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&OnObjectPropertyChangedHook);
    if (GEngine)
    {
        State.ActorMovedHandle = GEngine->OnActorMoved().AddStatic(&OnActorMovedHook);
    }
    State.bHooksInstalled = true;
}

void FMtoULinkageEditor::UninstallHooks()
{
    FMtoULinkageEditorState& State = GetState();
    if (!State.bHooksInstalled)
    {
        return;
    }
    // Leaving the editor with a takeover active would leave the sequence silent,
    // so the borrowed writers return before the hooks detach.
    FString RestoreError;
    FString RestoreErrorCode;
    RestoreTarget(RestoreError, RestoreErrorCode);
    MtoUSetLinkageHooks(FMtoULinkageHooks());
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(State.PropertyChangedHandle);
    State.PropertyChangedHandle.Reset();
    if (GEngine)
    {
        GEngine->OnActorMoved().Remove(State.ActorMovedHandle);
    }
    State.ActorMovedHandle.Reset();
    State.bHooksInstalled = false;
}

void FMtoULinkageEditor::SetObservedLinkageActor(TWeakObjectPtr<AActor> Actor)
{
    GetState().ObservedActor = Actor;
}

TWeakObjectPtr<AActor> FMtoULinkageEditor::GetObservedLinkageActor()
{
    return GetState().ObservedActor;
}

FSimpleMulticastDelegate& FMtoULinkageEditor::OnLinkageViewChanged()
{
    return GetState().ViewChanged;
}

void FMtoULinkageEditor::RegisterLinkagePanelWidget(TWeakPtr<SWidget> Widget)
{
    FMtoULinkageEditorState& State = GetState();
    State.PanelWidgets.AddUnique(Widget);
}

