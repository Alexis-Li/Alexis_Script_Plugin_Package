// MtoU camera sync prototype (Issue 52 verification). Editor-only prototype code.

#pragma once

#include "CoreMinimal.h"
#include "MtoUCameraSyncPayload.h"

class UCameraComponent;
class UWorld;

/**
 * Reads the evaluated state of one camera component exactly as Unreal would render it:
 * the view information, the projection matrix at the requested resolution, and the
 * spatial markers projected through that matrix.
 */
class FMtoUCameraSyncCapture
{
public:
	/** Captures camera, view and projection. Returns false when the camera cannot be read. */
	static bool CaptureCamera(
		UCameraComponent& Camera,
		const FIntPoint& OutputResolution,
		FMtoUCameraSyncCameraSample& OutCamera,
		FMtoUCameraSyncViewSample& OutView,
		FMtoUCameraSyncProjectionSample& OutProjection,
		FString& OutError);

	/** Collects every actor tagged MtoUCameraSyncMarker with its world location. */
	static void CollectMarkers(UWorld& World, TArray<FMtoUCameraSyncMarkerSample>& OutMarkers);

	/** True when the point projects in front of the camera; fills the normalized device coordinates. */
	static bool ProjectToNdc(const FMatrix& ViewProjection, const FVector& WorldCm, FVector2D& OutNdc);

	/** Converts normalized device coordinates (+Y up) into pixel coordinates inside the view rectangle. */
	static FVector2D NdcToPixel(const FVector2D& Ndc, const FIntRect& ViewRect);
};
