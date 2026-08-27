#include "MtoULiveLinkActor.h"

#include "MtoULiveLinkBinding.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "LiveLinkInstance.h"

AMtoULiveLinkActor::AMtoULiveLinkActor()
{
    SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMeshComponent"));
    SetRootComponent(SkeletalMeshComponent);
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(true);
}

void AMtoULiveLinkActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RefreshBinding();
}

void AMtoULiveLinkActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    RefreshBinding();
}

void AMtoULiveLinkActor::PostLoad()
{
    Super::PostLoad();
    GeneratedPreviewMesh = nullptr;
    PreviewState = Binding ? EMtoUPreviewState::Dirty : EMtoUPreviewState::None;
    PreviewBuildStage = EMtoUPreviewBuildStage::None;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
    PreviewDiagnostics = PreviewState == EMtoUPreviewState::Dirty
        ? TEXT("Level loaded. Run Refresh Preview.")
        : FString();
    ShowDriverMesh();
    RefreshBinding();
}

void AMtoULiveLinkActor::PostDuplicate(bool bDuplicateForPIE)
{
    Super::PostDuplicate(bDuplicateForPIE);
    GeneratedPreviewMesh = nullptr;
    PreviewState = Binding ? EMtoUPreviewState::Dirty : EMtoUPreviewState::None;
    PreviewBuildStage = EMtoUPreviewBuildStage::None;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
    PreviewDiagnostics = TEXT("Actor duplicated or reloaded. Run Refresh Preview.");
    ShowDriverMesh();
    RefreshBinding();
}

void AMtoULiveLinkActor::Destroyed()
{
    ReleaseGeneratedPreview();
    Super::Destroyed();
}

void AMtoULiveLinkActor::BeginDestroy()
{
    UnbindInputNotifications();
    ReleaseGeneratedPreview();
    Super::BeginDestroy();
}

void AMtoULiveLinkActor::SetBinding(UMtoULiveLinkBinding* InBinding)
{
    if (Binding == InBinding)
    {
        RefreshBinding();
        return;
    }

    ReleaseGeneratedPreview();
    Binding = InBinding;
    PreviewState = Binding && Binding->PreviewStaticMesh
        ? EMtoUPreviewState::Dirty
        : EMtoUPreviewState::None;
    PreviewBuildStage = EMtoUPreviewBuildStage::None;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
    PreviewDiagnostics = PreviewState == EMtoUPreviewState::Dirty
        ? TEXT("Run Refresh Preview to prepare the current Binding inputs.")
        : FString();
    RebindInputNotifications();
    if (Binding)
    {
        ShowDriverMesh();
    }
    else
    {
        HideDisplay();
    }
    RefreshBinding();
}

void AMtoULiveLinkActor::SetConnectionStatus(const FString& InStatus)
{
    ConnectionStatus = InStatus;
}

void AMtoULiveLinkActor::BeginPreviewBuild()
{
    ReleaseGeneratedPreview();
    HideDisplay();
    PreviewState = EMtoUPreviewState::Building;
    PreviewBuildStage = EMtoUPreviewBuildStage::Preflight;
    PreviewDiagnostics = TEXT("Preparing Generated Preview.");
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
}

void AMtoULiveLinkActor::SetPreviewBuildStage(EMtoUPreviewBuildStage Stage)
{
    PreviewBuildStage = Stage;
}

void AMtoULiveLinkActor::CompletePreviewBuild(
    USkeletalMesh* Mesh, bool bHasWarning, const FString& Diagnostics)
{
    if (!Mesh || Mesh->GetOuter() != this || !Mesh->HasAnyFlags(RF_Transient))
    {
        FailPreviewBuild(
            EMtoUPreviewBuildStage::Validation,
            TEXT("Preview preparation returned a mesh that is not actor-owned transient data."));
        return;
    }

    GeneratedPreviewMesh = Mesh;
    bPreviewBuildHasWarning = bHasWarning;
    PreviewState = bHasWarning ? EMtoUPreviewState::Warning : EMtoUPreviewState::Ready;
    PreviewBuildStage = EMtoUPreviewBuildStage::Validation;
    PreviewDiagnostics = Diagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    ShowGeneratedPreview(false);
}

void AMtoULiveLinkActor::FailPreviewBuild(
    EMtoUPreviewBuildStage Stage, const FString& Diagnostics)
{
    ReleaseGeneratedPreview();
    HideDisplay();
    PreviewState = EMtoUPreviewState::Error;
    PreviewBuildStage = Stage;
    PreviewDiagnostics = Diagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
}

void AMtoULiveLinkActor::InvalidateGeneratedPreview(const FString& Diagnostics)
{
    ReleaseGeneratedPreview();
    HideDisplay();
    PreviewState = Binding ? EMtoUPreviewState::Dirty : EMtoUPreviewState::None;
    PreviewBuildStage = EMtoUPreviewBuildStage::None;
    PreviewDiagnostics = Diagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    bPreviewBuildHasWarning = false;
}

void AMtoULiveLinkActor::ReleaseGeneratedPreview()
{
    if (GeneratedPreviewMesh
        && SkeletalMeshComponent
        && SkeletalMeshComponent->GetSkeletalMeshAsset() == GeneratedPreviewMesh)
    {
        SkeletalMeshComponent->SetSkeletalMeshAsset(nullptr);
    }
    GeneratedPreviewMesh = nullptr;
    if (DisplayTarget == EMtoUDisplayTarget::GeneratedPreview)
    {
        HideDisplay();
    }
}

void AMtoULiveLinkActor::ShowDriverMesh()
{
    DisplayTarget = EMtoUDisplayTarget::Driver;
    ReapplyDisplayTarget();
}

void AMtoULiveLinkActor::ShowGeneratedPreview(bool bBoneOnlyDiagnostic)
{
    if (!GeneratedPreviewMesh)
    {
        return;
    }
    DisplayTarget = EMtoUDisplayTarget::GeneratedPreview;
    ReapplyDisplayTarget();
    SkeletalMeshComponent->ClearMorphTargets();
    if (bBoneOnlyDiagnostic)
    {
        ConnectionStatus = TEXT("Connected: bone-only diagnostic; not valid for model acceptance");
    }
}

void AMtoULiveLinkActor::SetModelDiagnostics(
    const FString& Diagnostics,
    EMtoUModelDiagnosticLevel Level)
{
    ModelDiagnostics = Diagnostics;
    ModelDiagnosticLevel = Level;
    if (GeneratedPreviewMesh)
    {
        PreviewState = bPreviewBuildHasWarning || Level == EMtoUModelDiagnosticLevel::Partial
            ? EMtoUPreviewState::Warning
            : EMtoUPreviewState::Ready;
    }
}

void AMtoULiveLinkActor::NotifyBindingInputsChanged()
{
    RebindInputNotifications();
    InvalidateGeneratedPreview(TEXT("Binding inputs changed. Run Refresh Preview."));
}

void AMtoULiveLinkActor::NotifySourceAssetChanged(const UObject* Asset, const FString& Reason)
{
    if (!Binding || !Asset
        || (Asset != Binding->SkeletalMesh && Asset != Binding->PreviewStaticMesh))
    {
        return;
    }
    InvalidateGeneratedPreview(FString::Printf(
        TEXT("%s changed (%s). Run Refresh Preview."), *Asset->GetName(), *Reason));
}

void AMtoULiveLinkActor::RefreshBinding()
{
    RebindInputNotifications();
    ReapplyDisplayTarget();
    SkeletalMeshComponent->SetUpdateAnimationInEditor(true);
    SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
    SkeletalMeshComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
    if (ULiveLinkInstance* Instance = Cast<ULiveLinkInstance>(SkeletalMeshComponent->GetAnimInstance()))
    {
        Instance->SetSubject(FLiveLinkSubjectName(FName(TEXT("MtoU_Character"))));
        Instance->EnableLiveLinkEvaluation(true);
    }
}

void AMtoULiveLinkActor::ReapplyDisplayTarget()
{
    if (!SkeletalMeshComponent)
    {
        return;
    }

    SkeletalMeshComponent->SetDisablePostProcessBlueprint(true);
    switch (DisplayTarget)
    {
    case EMtoUDisplayTarget::Driver:
        SkeletalMeshComponent->SetSkeletalMeshAsset(Binding ? Binding->SkeletalMesh : nullptr);
        break;
    case EMtoUDisplayTarget::GeneratedPreview:
        SkeletalMeshComponent->SetSkeletalMeshAsset(GeneratedPreviewMesh);
        break;
    case EMtoUDisplayTarget::Hidden:
    default:
        SkeletalMeshComponent->SetSkeletalMeshAsset(nullptr);
        break;
    }
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(true);
}

void AMtoULiveLinkActor::HideDisplay()
{
    DisplayTarget = EMtoUDisplayTarget::Hidden;
    ReapplyDisplayTarget();
}

void AMtoULiveLinkActor::RebindInputNotifications()
{
    UnbindInputNotifications();
    if (!Binding)
    {
        return;
    }
    if (Binding->SkeletalMesh)
    {
        ObservedDriverMesh = Binding->SkeletalMesh;
        DriverMeshChangedHandle = Binding->SkeletalMesh->GetOnMeshChanged().AddUObject(
            this, &AMtoULiveLinkActor::HandleDriverMeshChanged);
    }
    if (Binding->PreviewStaticMesh)
    {
        ObservedPreviewMesh = Binding->PreviewStaticMesh;
        PreviewMeshChangedHandle = Binding->PreviewStaticMesh->GetOnMeshChanged().AddUObject(
            this, &AMtoULiveLinkActor::HandlePreviewMeshChanged);
        PreviewMeshBuiltHandle = Binding->PreviewStaticMesh->OnPostMeshBuild().AddUObject(
            this, &AMtoULiveLinkActor::HandlePreviewMeshBuilt);
    }
}

void AMtoULiveLinkActor::UnbindInputNotifications()
{
    if (ObservedDriverMesh.IsValid())
    {
        ObservedDriverMesh->GetOnMeshChanged().Remove(DriverMeshChangedHandle);
    }
    if (ObservedPreviewMesh.IsValid())
    {
        ObservedPreviewMesh->GetOnMeshChanged().Remove(PreviewMeshChangedHandle);
        ObservedPreviewMesh->OnPostMeshBuild().Remove(PreviewMeshBuiltHandle);
    }
    ObservedDriverMesh.Reset();
    ObservedPreviewMesh.Reset();
    DriverMeshChangedHandle.Reset();
    PreviewMeshChangedHandle.Reset();
    PreviewMeshBuiltHandle.Reset();
}

void AMtoULiveLinkActor::HandleDriverMeshChanged()
{
    NotifySourceAssetChanged(ObservedDriverMesh.Get(), TEXT("source rebuild"));
}

void AMtoULiveLinkActor::HandlePreviewMeshChanged()
{
    NotifySourceAssetChanged(ObservedPreviewMesh.Get(), TEXT("PostEdit change"));
}

void AMtoULiveLinkActor::HandlePreviewMeshBuilt(UStaticMesh* Mesh)
{
    NotifySourceAssetChanged(Mesh, TEXT("source rebuild"));
}
