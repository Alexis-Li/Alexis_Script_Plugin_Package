// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#include "MtoUCameraSyncCapture.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CineCameraComponent.h"
#include "CineCameraSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "SceneView.h"

namespace
{
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
 * The pixel rectangle the film aperture occupies inside the output resolution. A camera
 * that constrains its aspect ratio renders its aperture inside the gate and leaves the
 * remaining pixels as bars, which is what the Maya resolution gate has to reproduce.
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
}  // namespace

bool FMtoUCameraSyncCapture::CaptureCamera(
	UCameraComponent& Camera,
	const FIntPoint& OutputResolution,
	FMtoUCameraSyncCameraSample& OutCamera,
	FMtoUCameraSyncViewSample& OutView,
	FMtoUCameraSyncProjectionSample& OutProjection,
	FString& OutError)
{
	FMinimalViewInfo ViewInfo;
	Camera.GetCameraView(0.0f, ViewInfo);

	if (ViewInfo.ProjectionMode != ECameraProjectionMode::Perspective)
	{
		OutError = TEXT("prototype supports perspective cameras only");
		return false;
	}
	if (OutputResolution.X <= 0 || OutputResolution.Y <= 0)
	{
		OutError = TEXT("output resolution must be positive");
		return false;
	}

	const UCineCameraComponent* Cine = Cast<UCineCameraComponent>(&Camera);

	OutCamera = FMtoUCameraSyncCameraSample();
	OutCamera.Name = Camera.GetName();
	OutCamera.ComponentName = Camera.GetName();
	OutCamera.Path = Camera.GetPathName();
	OutCamera.bCineCamera = Cine != nullptr;
	if (const AActor* Owner = Camera.GetOwner())
	{
		OutCamera.ActorName = Owner->GetActorNameOrLabel();
		OutCamera.Name = Owner->GetActorNameOrLabel();
	}

	// The view transform is what renders, so it is what Maya has to reproduce.
	OutCamera.LocationCm = ViewInfo.Location;
	OutCamera.Rotation = ViewInfo.Rotation;
	OutCamera.Right = ViewInfo.Rotation.RotateVector(FVector::RightVector);
	OutCamera.Up = ViewInfo.Rotation.RotateVector(FVector::UpVector);
	OutCamera.Forward = ViewInfo.Rotation.RotateVector(FVector::ForwardVector);
	OutCamera.ComponentLocationCm = Camera.GetComponentLocation();
	OutCamera.bViewTransformDiffers =
		!OutCamera.ComponentLocationCm.Equals(OutCamera.LocationCm, 0.1);

	OutCamera.AspectAxisConstraint = MtoUCameraSyncAxisConstraintName(
		ViewInfo.AspectRatioAxisConstraint.Get(EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV));
	OutCamera.bConstrainAspectRatio = ViewInfo.bConstrainAspectRatio != 0;
	OutCamera.RenderedAspectRatio = OutCamera.bConstrainAspectRatio
		? ViewInfo.AspectRatio
		: static_cast<double>(OutputResolution.X) / static_cast<double>(OutputResolution.Y);

	// A camera without an explicit near clip renders with the engine default, and Maya
	// rejects a clip plane of zero, so the resolved value is what gets transferred.
	OutCamera.NearClipCm = ViewInfo.GetFinalPerspectiveNearClipPlane();
	OutCamera.NearClipSource = ViewInfo.PerspectiveNearClipPlane > 0.0f
		? TEXT("view_info")
		: TEXT("engine_default");
	OutCamera.OffCenterProjectionOffset = ViewInfo.OffCenterProjectionOffset;
	OutCamera.Overscan = ViewInfo.GetOverscan();
	OutCamera.OverscanResolutionFraction = ViewInfo.OverscanResolutionFraction;
	OutCamera.PostProcessBlendWeight = Camera.PostProcessBlendWeight;

	if (Cine)
	{
		OutCamera.FocalLengthMm = Cine->CurrentFocalLength;
		OutCamera.FStop = Cine->CurrentAperture;
		OutCamera.HorizontalFovDeg = Cine->GetHorizontalFieldOfView();
		OutCamera.VerticalFovDeg = Cine->GetVerticalFieldOfView();

		OutCamera.SensorWidthMm = Cine->Filmback.SensorWidth;
		OutCamera.SensorHeightMm = Cine->Filmback.SensorHeight;
		OutCamera.SensorAspectRatio = Cine->Filmback.SensorAspectRatio;
		OutCamera.SensorHorizontalOffsetMm = Cine->Filmback.SensorHorizontalOffset;
		OutCamera.SensorVerticalOffsetMm = Cine->Filmback.SensorVerticalOffset;

		OutCamera.FocusMethod = FocusMethodName(Cine->FocusSettings.FocusMethod);
		OutCamera.ManualFocusDistanceCm = Cine->FocusSettings.ManualFocusDistance;
		OutCamera.bDepthOfField = Cine->FocusSettings.FocusMethod != ECameraFocusMethod::Disable;

		OutCamera.SqueezeFactor = Cine->LensSettings.SqueezeFactor;
		OutCamera.MinFocalMm = Cine->LensSettings.MinFocalLength;
		OutCamera.MaxFocalMm = Cine->LensSettings.MaxFocalLength;
		OutCamera.MinFStop = Cine->LensSettings.MinFStop;
		OutCamera.MaxFStop = Cine->LensSettings.MaxFStop;
		OutCamera.MinimumFocusDistanceCm = Cine->LensSettings.MinimumFocusDistance;

		OutCamera.CropAspectRatio = Cine->CropSettings.AspectRatio;
		OutCamera.bCropEnabled = Cine->CropSettings.AspectRatio > 0.0f;

		OutCamera.CustomNearClipCm = Cine->CustomNearClippingPlane;
		OutCamera.bOverrideCustomNearClip = Cine->bOverride_CustomNearClippingPlane;
	}
	else
	{
		OutCamera.FocalLengthMm = 0.0;
		OutCamera.HorizontalFovDeg = ViewInfo.FOV;
		OutCamera.VerticalFovDeg = ViewInfo.FOV;
		OutCamera.bDepthOfField = ViewInfo.PostProcessSettings.bOverride_DepthOfFieldFstop != 0;
	}

	// Focus distance and depth of field as the renderer receives them.
	const FPostProcessSettings& PostProcess = ViewInfo.PostProcessSettings;
	OutCamera.FocusDistanceCm = PostProcess.bOverride_DepthOfFieldFocalDistance
		? PostProcess.DepthOfFieldFocalDistance
		: (Cine ? Cine->CurrentFocusDistance : 0.0);
	OutCamera.Dof.bOverrideFocalDistance = PostProcess.bOverride_DepthOfFieldFocalDistance != 0;
	OutCamera.Dof.FocalDistanceCm = PostProcess.DepthOfFieldFocalDistance;
	OutCamera.Dof.bOverrideFStop = PostProcess.bOverride_DepthOfFieldFstop != 0;
	OutCamera.Dof.FStop = PostProcess.DepthOfFieldFstop;
	OutCamera.Dof.bOverrideSensorWidth = PostProcess.bOverride_DepthOfFieldSensorWidth != 0;
	OutCamera.Dof.SensorWidthMm = PostProcess.DepthOfFieldSensorWidth;
	OutCamera.Dof.bOverrideSqueezeFactor = PostProcess.bOverride_DepthOfFieldSqueezeFactor != 0;
	OutCamera.Dof.SqueezeFactor = PostProcess.DepthOfFieldSqueezeFactor;
	OutCamera.Dof.bOverrideMinFStop = PostProcess.bOverride_DepthOfFieldMinFstop != 0;
	OutCamera.Dof.MinFStop = PostProcess.DepthOfFieldMinFstop;
	OutCamera.Dof.bOverrideBladeCount = PostProcess.bOverride_DepthOfFieldBladeCount != 0;
	OutCamera.Dof.BladeCount = PostProcess.DepthOfFieldBladeCount;
	OutCamera.Dof.bOverrideDepthBlurRadius = PostProcess.bOverride_DepthOfFieldDepthBlurRadius != 0;
	OutCamera.Dof.DepthBlurRadius = PostProcess.DepthOfFieldDepthBlurRadius;
	OutCamera.Dof.bOverrideDepthBlurAmount = PostProcess.bOverride_DepthOfFieldDepthBlurAmount != 0;
	OutCamera.Dof.DepthBlurAmount = PostProcess.DepthOfFieldDepthBlurAmount;

	OutView = FMtoUCameraSyncViewSample();
	OutView.LocationCm = ViewInfo.Location;
	OutView.Rotation = ViewInfo.Rotation;
	OutView.FovDeg = ViewInfo.FOV;
	OutView.AspectRatio = OutCamera.RenderedAspectRatio;
	OutView.AspectAxisConstraint = OutCamera.AspectAxisConstraint;
	OutView.bConstrainAspectRatio = OutCamera.bConstrainAspectRatio;
	OutView.OffCenterProjectionOffset = ViewInfo.OffCenterProjectionOffset;
	OutView.Overscan = ViewInfo.GetOverscan();
	OutView.OverscanResolutionFraction = ViewInfo.OverscanResolutionFraction;
	OutView.PerspectiveNearClipCm = ViewInfo.GetFinalPerspectiveNearClipPlane();

	FSceneViewProjectionData ProjectionData;
	const FIntRect FullRect(0, 0, OutputResolution.X, OutputResolution.Y);
	const FIntRect ApertureRect = ComputeApertureRect(FullRect, ViewInfo);
	ProjectionData.ViewOrigin = ViewInfo.Location;
	ProjectionData.ViewRotationMatrix = FInverseRotationMatrix(ViewInfo.Rotation) * FMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(1, 0, 0, 0),
		FPlane(0, 1, 0, 0),
		FPlane(0, 0, 0, 1));
	ProjectionData.SetViewRectangle(FullRect);
	FMinimalViewInfo MutableView = ViewInfo;
	FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle(
		MutableView,
		ViewInfo.AspectRatioAxisConstraint.Get(EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV),
		ApertureRect,
		ProjectionData);

	const FMatrix ViewMatrix = FTranslationMatrix(-ProjectionData.ViewOrigin)
		* ProjectionData.ViewRotationMatrix;
	OutProjection.ViewProjection = ViewMatrix * ProjectionData.ProjectionMatrix;
	OutProjection.ViewRect = ApertureRect;
	return true;
}

void FMtoUCameraSyncCapture::CollectMarkers(
	UWorld& World,
	TArray<FMtoUCameraSyncMarkerSample>& OutMarkers)
{
	static const FName MarkerTag(TEXT("MtoUCameraSyncMarker"));
	OutMarkers.Reset();
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || !Actor->ActorHasTag(MarkerTag))
		{
			continue;
		}
		FMtoUCameraSyncMarkerSample Marker;
		Marker.Name = Actor->GetActorNameOrLabel();
		Marker.LocationCm = Actor->GetActorLocation();
		OutMarkers.Add(Marker);
	}
	OutMarkers.Sort([](const FMtoUCameraSyncMarkerSample& A, const FMtoUCameraSyncMarkerSample& B)
	{
		return A.Name < B.Name;
	});
}

bool FMtoUCameraSyncCapture::ProjectToNdc(
	const FMatrix& ViewProjection,
	const FVector& WorldCm,
	FVector2D& OutNdc)
{
	const FVector4 Clip = ViewProjection.TransformFVector4(FVector4(WorldCm.X, WorldCm.Y, WorldCm.Z, 1.0));
	if (Clip.W <= UE_KINDA_SMALL_NUMBER)
	{
		OutNdc = FVector2D::ZeroVector;
		return false;
	}
	OutNdc = FVector2D(Clip.X / Clip.W, Clip.Y / Clip.W);
	return true;
}

FVector2D FMtoUCameraSyncCapture::NdcToPixel(const FVector2D& Ndc, const FIntRect& ViewRect)
{
	const double HalfX = Ndc.X * 0.5 + 0.5;
	const double HalfY = 0.5 - Ndc.Y * 0.5;
	return FVector2D(
		ViewRect.Min.X + HalfX * ViewRect.Width(),
		ViewRect.Min.Y + HalfY * ViewRect.Height());
}
