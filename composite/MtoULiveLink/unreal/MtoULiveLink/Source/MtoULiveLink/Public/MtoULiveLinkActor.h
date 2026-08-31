#pragma once

#include "GameFramework/Actor.h"

#include "MtoULiveLinkActor.generated.h"

class UMtoULiveLinkBinding;
class USkeletalMeshComponent;
class USkeletalMesh;
class UStaticMesh;
class FMtoUPreviewPreparation;

UENUM()
enum class EMtoUPreviewState : uint8
{
    /** There is no Binding or either required Preview input is absent. */
    None,
    /** Both Preview inputs are present, but the current revision is not refreshed. */
    Dirty,
    /** The current explicit Preview refresh is running and its stage is observable. */
    Building,
    /** The current revision has a complete Generated Preview without a quality warning. */
    Ready,
    /** The current revision has a complete usable Generated Preview with a quality warning. */
    Warning,
    /** An explicit Preview refresh failed for the current revision. */
    Error
};

UENUM()
enum class EMtoUPreviewBuildStage : uint8
{
    None,
    Preflight,
    GeometryConversion,
    WeightTransfer,
    SkeletalMeshBuild,
    Validation
};

UENUM()
enum class EMtoUModelDiagnosticLevel : uint8
{
    None,
    Full,
    Partial,
    BoneOnly,
    Error
};

UENUM()
enum class EMtoUDisplayTarget : uint8
{
    Hidden,
    Driver,
    GeneratedPreview
};

/**
 * One coherent snapshot of the Binding actor's Preview readiness. It describes
 * only whether the current Preview revision has a complete Generated Preview
 * Skeletal Mesh; connection status, display selection, and Model diagnostics
 * remain separate concerns that consume readiness without modifying it.
 */
struct MTOULIVELINK_API FMtoUPreviewReadiness
{
    EMtoUPreviewState State = EMtoUPreviewState::None;
    EMtoUPreviewBuildStage Stage = EMtoUPreviewBuildStage::None;
    USkeletalMesh* GeneratedPreview = nullptr;
    FString Summary;
    FString Diagnostics;

    /** True only for a complete current-revision Generated Preview (Ready or Warning). */
    bool IsUsable() const
    {
        return GeneratedPreview != nullptr
            && (State == EMtoUPreviewState::Ready || State == EMtoUPreviewState::Warning);
    }
};

UCLASS()
class MTOULIVELINK_API AMtoULiveLinkActor : public AActor
{
    GENERATED_BODY()

public:
    AMtoULiveLinkActor();

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void PostLoad() override;
    virtual void PostDuplicate(bool bDuplicateForPIE) override;
    virtual void Destroyed() override;
    virtual void BeginDestroy() override;

    void SetBinding(UMtoULiveLinkBinding* InBinding);
    void SetConnectionStatus(const FString& InStatus);
    USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }
    UMtoULiveLinkBinding* GetBinding() const { return Binding; }
    const FString& GetConnectionStatus() const { return ConnectionStatus; }

    /** The single coherent read of Preview readiness for the current revision. */
    FMtoUPreviewReadiness GetPreviewReadiness() const;

    EMtoUDisplayTarget GetDisplayTarget() const { return DisplayTarget; }
    const FString& GetModelDiagnostics() const { return ModelDiagnostics; }
    EMtoUModelDiagnosticLevel GetModelDiagnosticLevel() const { return ModelDiagnosticLevel; }

    /** Display selection is a separate concern that consumes readiness. */
    void ShowDriverMesh();
    void ShowGeneratedPreview(bool bBoneOnlyDiagnostic);

    /**
     * Connection-time Model diagnostics are separate evidence; they never
     * modify Preview readiness.
     */
    void SetModelDiagnostics(
        const FString& Diagnostics,
        EMtoUModelDiagnosticLevel Level);

    /** Semantic lifecycle notifications that route through the actor's readiness transitions. */
    void NotifyBindingInputsChanged();
    void NotifySourceAssetChanged(const UObject* Asset, const FString& Reason);
    void NotifyGeneratedPreviewDeleted();
    void NotifyTransientPreviewReleased();

private:
    friend class FMtoULiveLinkSource;
    friend class FMtoUPreviewPreparation;
#if WITH_DEV_AUTOMATION_TESTS
    friend class FMtoUPreviewReadinessTestAccess;
#endif

    /** Readiness transitions owned exclusively by the Binding actor. */
    bool BeginPreviewBuild();
    void SetPreviewBuildStage(EMtoUPreviewBuildStage Stage);
    bool CompletePreviewBuild(USkeletalMesh* Mesh, bool bHasWarning,
        const FString& Diagnostics, const FString& Summary = FString(),
        const TArray<int32>& DriverGarmentMaterialSlots = {});
    bool FailPreviewBuild(EMtoUPreviewBuildStage Stage, const FString& Diagnostics);
    void InvalidateGeneratedPreview(const FString& Diagnostics);
    void EnterUnrefreshedReadiness(const FString& Message);
    void ReleaseGeneratedPreview();

    void RefreshBinding();
    void ReapplyDisplayTarget();
    void ApplyModelMorphCurves(
        const TArray<FName>& CurveNames, const TArray<float>& CurveValues);
    void RebindInputNotifications();
    void UnbindInputNotifications();
    void HandleDriverMeshChanged();
    void HandlePreviewMeshChanged();
    void HandlePreviewMeshBuilt(UStaticMesh* Mesh);
    void HideDisplay();

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;

    /** Original Driver display used behind the generated garment in Model preview. */
    UPROPERTY()
    TObjectPtr<USkeletalMeshComponent> DriverMeshComponent;

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<UMtoULiveLinkBinding> Binding;

    UPROPERTY(Transient, DuplicateTransient, VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMesh> GeneratedPreviewMesh;

    UPROPERTY(Transient, DuplicateTransient)
    TArray<int32> DriverGarmentMaterialSlots;

    UPROPERTY(Transient, DuplicateTransient)
    EMtoUPreviewState PreviewState = EMtoUPreviewState::None;

    UPROPERTY(Transient, DuplicateTransient)
    EMtoUPreviewBuildStage PreviewBuildStage = EMtoUPreviewBuildStage::None;

    UPROPERTY(Transient, DuplicateTransient)
    EMtoUDisplayTarget DisplayTarget = EMtoUDisplayTarget::Hidden;

    UPROPERTY(Transient, DuplicateTransient)
    FString PreviewDiagnostics;

    FString PreviewSummary;

    UPROPERTY(Transient, DuplicateTransient)
    FString ModelDiagnostics;

    UPROPERTY(Transient, DuplicateTransient)
    EMtoUModelDiagnosticLevel ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;

    UPROPERTY(VisibleAnywhere, Transient, Category = "MtoU_LiveLink")
    FString ConnectionStatus = TEXT("Disconnected");

    TWeakObjectPtr<USkeletalMesh> ObservedDriverMesh;
    TWeakObjectPtr<UStaticMesh> ObservedPreviewMesh;
    FDelegateHandle DriverMeshChangedHandle;
    FDelegateHandle PreviewMeshChangedHandle;
    FDelegateHandle PreviewMeshBuiltHandle;
};
