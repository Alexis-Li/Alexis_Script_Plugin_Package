#pragma once

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

#include "MtoULiveLinkActor.generated.h"

class UMtoULiveLinkBinding;
class USkeletalMeshComponent;
class USkeletalMesh;
class UStaticMesh;
class FMtoUPreviewPreparation;
struct FMtoUCharacterComposition;

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
 * Display component of one enabled Additional Part. Each part evaluates the
 * one character Live Link subject itself, so the whole character poses from the
 * same session, while the Primary Driver stays the only source of skeleton
 * baseline, garment Preview data, and accepted Preview Morphs.
 */
UCLASS(ClassGroup = MtoULiveLink)
class MTOULIVELINK_API UMtoUCharacterPartComponent : public USkeletalMeshComponent
{
    GENERATED_BODY()

public:
    /** Stable Binding identity of the Additional Part this component displays. */
    UPROPERTY(Transient, DuplicateTransient)
    FGuid PartId;

    /** Resolved Skeletal Mesh of that part, independent of the current display selection. */
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<USkeletalMesh> PartMesh;

    /** True when this component already displays exactly the given part identity. */
    bool MatchesPart(const FGuid& InPartId, const USkeletalMesh* InMesh) const
    {
        return PartId == InPartId && PartMesh.Get() == InMesh;
    }
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

    /**
     * Enabled Additional Part components in canonical part order. They are a
     * display concern of the same character; the Primary Driver remains
     * GetSkeletalMeshComponent().
     */
    const TArray<TObjectPtr<UMtoUCharacterPartComponent>>& GetCharacterPartComponents() const
    {
        return CharacterPartComponents;
    }

    /** Part-level composition problems of the current Binding; empty when every enabled part is compatible. */
    const FString& GetCharacterPartDiagnostics() const { return CharacterPartDiagnostics; }

    /** Human-readable character composition for the Details panel. */
    const FString& GetCharacterPartSummary() const { return CharacterPartSummary; }

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
    /**
     * The Binding's Additional Parts list changed. It is a character
     * composition change, not a Preview revision change, so a generated
     * garment Preview survives while the display components resynchronize.
     */
    void NotifyCharacterPartsChanged();

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

    /**
     * The actor's single composition boundary: resolve the Binding's character
     * composition, publish its diagnostics, and reconcile the display
     * components an enabled Part change requires. Only a change of the enabled
     * composition ends the streaming session; renaming a part or reordering
     * the list changes nothing observable.
     */
    void SyncCharacterComposition();
    bool CharacterPartsMatch(const FMtoUCharacterComposition& Composition) const;
    void ApplyCharacterPartDisplay();
    void ReconcileCharacterPartComponents(const FMtoUCharacterComposition& Composition);
    void DestroyCharacterPartComponent(UMtoUCharacterPartComponent& Component);
    void ConfigureLiveLinkInstance(USkeletalMeshComponent& Component);
    void InheritPrimaryDisplaySettings(USkeletalMeshComponent& Component);
    void HandleCharacterPartMeshChanged();

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

    /**
     * Display components of the enabled Additional Parts, in canonical part
     * order. They are transient runtime state rebuilt from the Binding, never
     * saved with the actor.
     */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UMtoUCharacterPartComponent>> CharacterPartComponents;

    FString CharacterPartDiagnostics;
    FString CharacterPartSummary;

    /**
     * One observed Additional Part source: the mesh whose rebuild makes the
     * negotiated composition stale, and the handle that observes it.
     */
    struct FMtoUObservedPartMesh
    {
        TWeakObjectPtr<USkeletalMesh> Mesh;
        FDelegateHandle ChangedHandle;
    };

    TArray<FMtoUObservedPartMesh> ObservedPartMeshes;

    TWeakObjectPtr<USkeletalMesh> ObservedDriverMesh;
    TWeakObjectPtr<UStaticMesh> ObservedPreviewMesh;
    FDelegateHandle DriverMeshChangedHandle;
    FDelegateHandle PreviewMeshChangedHandle;
    FDelegateHandle PreviewMeshBuiltHandle;
};
