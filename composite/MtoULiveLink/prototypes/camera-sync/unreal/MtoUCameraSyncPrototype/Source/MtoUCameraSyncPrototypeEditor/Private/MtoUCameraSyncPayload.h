// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "Math/IntPoint.h"
#include "Math/IntRect.h"
#include "Math/Matrix.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "Misc/FrameRate.h"

class FJsonObject;
class FJsonValue;

/** Depth of field values as the renderer receives them, plus the authored overrides. */
struct FMtoUCameraSyncDofSample
{
	bool bOverrideFocalDistance = false;
	double FocalDistanceCm = 0.0;
	bool bOverrideFStop = false;
	double FStop = 0.0;
	bool bOverrideSensorWidth = false;
	double SensorWidthMm = 0.0;
	bool bOverrideSqueezeFactor = false;
	double SqueezeFactor = 1.0;
	bool bOverrideMinFStop = false;
	double MinFStop = 0.0;
	bool bOverrideBladeCount = false;
	int32 BladeCount = 0;
	bool bOverrideDepthBlurRadius = false;
	double DepthBlurRadius = 0.0;
	bool bOverrideDepthBlurAmount = false;
	double DepthBlurAmount = 0.0;
};

/** One evaluated camera as the wire protocol describes it. */
struct FMtoUCameraSyncCameraSample
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

	FMtoUCameraSyncDofSample Dof;
};

/** A spatial marker in the level with its Unreal-projected screen position. */
struct FMtoUCameraSyncMarkerSample
{
	FString Name;
	FVector LocationCm = FVector::ZeroVector;
	bool bProjected = false;
	FVector2D Ndc = FVector2D::ZeroVector;
	FVector2D Pixel = FVector2D::ZeroVector;
};

/** Which camera cut the evaluated camera came from. */
struct FMtoUCameraSyncCutSample
{
	bool bHasTrack = false;
	int32 SectionCount = 0;
	bool bActive = false;
	int32 ActiveIndex = INDEX_NONE;
	FString Stage = TEXT("none");
};

/** Sequence time in every unit the two hosts need. */
struct FMtoUCameraSyncTimeSample
{
	FFrameRate DisplayRate = FFrameRate(24, 1);
	FFrameRate TickResolution = FFrameRate(24000, 1);
	int32 PlaybackStart = 0;
	int32 PlaybackEnd = 0;
	double DisplayFrame = 0.0;
	int32 SourceFrame = 0;
	double Seconds = 0.0;
	int64 Tick = 0;
};

/** The evaluated view that Unreal would render. */
struct FMtoUCameraSyncViewSample
{
	FVector LocationCm = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	double FovDeg = 0.0;
	double AspectRatio = 0.0;
	FString AspectAxisConstraint = TEXT("MaintainXFOV");
	bool bConstrainAspectRatio = false;
	FVector2D OffCenterProjectionOffset = FVector2D::ZeroVector;
	double Overscan = 0.0;
	double OverscanResolutionFraction = 1.0;
	double PerspectiveNearClipCm = 0.0;
	bool bHasPerspectiveFarClip = false;
	double PerspectiveFarClipCm = 0.0;
};

/** The view-projection matrix the marker projection used. */
struct FMtoUCameraSyncProjectionSample
{
	FMatrix ViewProjection = FMatrix::Identity;
	FIntRect ViewRect;
};

/** One complete published frame. */
struct FMtoUCameraSyncFrameSample
{
	int64 Serial = 0;
	int64 EvalSerial = 0;
	FString EvalIdentity;
	FString SequenceName;
	FString SequencePath;
	FIntPoint OutputResolution = FIntPoint(1920, 1080);
	FString ResolutionSource = TEXT("prototype");
	/** Pixel extent of the film aperture inside the output resolution. */
	FIntPoint ApertureResolution = FIntPoint(1920, 1080);

	FMtoUCameraSyncTimeSample Time;
	FMtoUCameraSyncCutSample Cut;
	FMtoUCameraSyncCameraSample Camera;
	FMtoUCameraSyncViewSample View;
	FMtoUCameraSyncProjectionSample Projection;
	TArray<FMtoUCameraSyncMarkerSample> Markers;
};

/** Fields that do not change until the session restarts. */
struct FMtoUCameraSyncSessionSample
{
	FString SequenceName;
	FString SequencePath;
	FIntPoint OutputResolution = FIntPoint(1920, 1080);
	FString ResolutionSource = TEXT("prototype");
	FIntPoint ApertureResolution = FIntPoint(1920, 1080);
	FFrameRate DisplayRate = FFrameRate(24, 1);
	FFrameRate TickResolution = FFrameRate(24000, 1);
	int32 PlaybackStart = 0;
	int32 PlaybackEnd = 0;
	double FarClipFallbackCm = 100000.0;
	FMtoUCameraSyncCutSample Cut;
	TArray<FString> MarkerNames;
};

FString MtoUCameraSyncAxisConstraintName(uint8 Constraint);

TSharedRef<FJsonObject> MtoUCameraSyncSerializeFrameRate(const FFrameRate& Rate);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeDof(const FMtoUCameraSyncDofSample& Dof);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeCamera(const FMtoUCameraSyncCameraSample& Camera);

/**
 * Canonical text of every camera value MtoUCameraSyncSerializeCamera publishes, compared
 * for equality only. The evaluation identity carries it, so editing the same camera at the
 * same sequence time (transform, focal length, filmback, offsets, clip planes, depth of
 * field) starts a new target generation instead of leaving the old content converged.
 * Formatting precision is 1e-6 of the payload's own unit.
 */
FString MtoUCameraSyncCameraContentDigest(const FMtoUCameraSyncCameraSample& Camera);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeCut(const FMtoUCameraSyncCutSample& Cut);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeTime(const FMtoUCameraSyncTimeSample& Time);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeView(const FMtoUCameraSyncViewSample& View);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeProjection(const FMtoUCameraSyncProjectionSample& Projection);
TArray<TSharedPtr<FJsonValue>> MtoUCameraSyncSerializeMarkers(const TArray<FMtoUCameraSyncMarkerSample>& Markers);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeFrame(const FMtoUCameraSyncFrameSample& Frame);
TSharedRef<FJsonObject> MtoUCameraSyncSerializeSession(const FMtoUCameraSyncSessionSample& Session);
