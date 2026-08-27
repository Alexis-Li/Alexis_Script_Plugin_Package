#pragma once

#include "GameFramework/Actor.h"

#include "MtoULiveLinkActor.generated.h"

class UMtoULiveLinkBinding;
class USkeletalMeshComponent;
class USkeletalMesh;
class UStaticMesh;

UENUM()
enum class EMtoUPreviewState : uint8
{
    None,
    Dirty,
    Building,
    Ready,
    Warning,
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

    /** True while the actor owns a complete, transactional Generated Preview. */
    bool HasReadyGeneratedPreview() const { return GeneratedPreviewMesh != nullptr; }
    USkeletalMesh* GetGeneratedPreviewMesh() const { return GeneratedPreviewMesh; }
    EMtoUPreviewState GetPreviewState() const { return PreviewState; }
    EMtoUPreviewBuildStage GetPreviewBuildStage() const { return PreviewBuildStage; }
    EMtoUDisplayTarget GetDisplayTarget() const { return DisplayTarget; }
    const FString& GetPreviewDiagnostics() const { return PreviewDiagnostics; }
    const FString& GetModelDiagnostics() const { return ModelDiagnostics; }
    EMtoUModelDiagnosticLevel GetModelDiagnosticLevel() const { return ModelDiagnosticLevel; }

    void BeginPreviewBuild();
    void SetPreviewBuildStage(EMtoUPreviewBuildStage Stage);
    void CompletePreviewBuild(USkeletalMesh* Mesh, bool bHasWarning, const FString& Diagnostics);
    void FailPreviewBuild(EMtoUPreviewBuildStage Stage, const FString& Diagnostics);
    void InvalidateGeneratedPreview(const FString& Diagnostics);
    void ReleaseGeneratedPreview();
    void ShowDriverMesh();
    void ShowGeneratedPreview(bool bBoneOnlyDiagnostic);
    void ReapplyDisplayTarget();
    void SetModelDiagnostics(
        const FString& Diagnostics,
        EMtoUModelDiagnosticLevel Level);
    void NotifyBindingInputsChanged();
    void NotifySourceAssetChanged(const UObject* Asset, const FString& Reason);

private:
    void RefreshBinding();
    void RebindInputNotifications();
    void UnbindInputNotifications();
    void HandleDriverMeshChanged();
    void HandlePreviewMeshChanged();
    void HandlePreviewMeshBuilt(UStaticMesh* Mesh);

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<UMtoULiveLinkBinding> Binding;

    UPROPERTY(Transient, DuplicateTransient, VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMesh> GeneratedPreviewMesh;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    EMtoUPreviewState PreviewState = EMtoUPreviewState::None;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    EMtoUPreviewBuildStage PreviewBuildStage = EMtoUPreviewBuildStage::None;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    EMtoUDisplayTarget DisplayTarget = EMtoUDisplayTarget::Hidden;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    FString PreviewDiagnostics;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    FString ModelDiagnostics;

    UPROPERTY(VisibleAnywhere, Transient, DuplicateTransient, Category = "MtoU Preview")
    EMtoUModelDiagnosticLevel ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;

    UPROPERTY(VisibleAnywhere, Transient, Category = "MtoU_LiveLink")
    FString ConnectionStatus = TEXT("Disconnected");

    TWeakObjectPtr<USkeletalMesh> ObservedDriverMesh;
    TWeakObjectPtr<UStaticMesh> ObservedPreviewMesh;
    FDelegateHandle DriverMeshChangedHandle;
    FDelegateHandle PreviewMeshChangedHandle;
    FDelegateHandle PreviewMeshBuiltHandle;
    bool bPreviewBuildHasWarning = false;

    void HideDisplay();
};
