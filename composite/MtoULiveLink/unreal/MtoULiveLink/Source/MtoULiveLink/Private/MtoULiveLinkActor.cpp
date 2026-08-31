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

    DriverMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("DriverMeshComponent"));
    DriverMeshComponent->SetupAttachment(SkeletalMeshComponent);
    DriverMeshComponent->SetDisablePostProcessBlueprint(true);
    DriverMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    DriverMeshComponent->SetGenerateOverlapEvents(false);
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
    EnterUnrefreshedReadiness(TEXT("Level loaded. Run Refresh Preview."));
    ShowDriverMesh();
    RefreshBinding();
}

void AMtoULiveLinkActor::PostDuplicate(bool bDuplicateForPIE)
{
    Super::PostDuplicate(bDuplicateForPIE);
    GeneratedPreviewMesh = nullptr;
    EnterUnrefreshedReadiness(TEXT("Actor duplicated or reloaded. Run Refresh Preview."));
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
    EnterUnrefreshedReadiness(TEXT("Run Refresh Preview to prepare the current Binding inputs."));
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

FMtoUPreviewReadiness AMtoULiveLinkActor::GetPreviewReadiness() const
{
    FMtoUPreviewReadiness Readiness;
    Readiness.State = PreviewState;
    Readiness.Stage = PreviewBuildStage;
    Readiness.GeneratedPreview = GeneratedPreviewMesh;
    Readiness.Summary = PreviewSummary;
    Readiness.Diagnostics = PreviewDiagnostics;
    return Readiness;
}

bool AMtoULiveLinkActor::BeginPreviewBuild()
{
    if (PreviewState == EMtoUPreviewState::Building)
    {
        ensureAlwaysMsgf(false, TEXT(
            "MtoU Preview readiness: ignored a refresh start while the current refresh is still Building."));
        return false;
    }

    ReleaseGeneratedPreview();
    HideDisplay();
    PreviewState = EMtoUPreviewState::Building;
    PreviewBuildStage = EMtoUPreviewBuildStage::Preflight;
    PreviewDiagnostics = TEXT("Preparing Generated Preview.");
    PreviewSummary = PreviewDiagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    return true;
}

void AMtoULiveLinkActor::SetPreviewBuildStage(EMtoUPreviewBuildStage Stage)
{
    // The enum order matches the preparation pipeline, so stage observation is
    // monotonic and accepted only while the current refresh is Building.
    if (PreviewState != EMtoUPreviewState::Building
        || Stage < PreviewBuildStage)
    {
        ensureAlwaysMsgf(false, TEXT(
            "MtoU Preview readiness: ignored build stage %d observed outside monotonic progression (state %d, stage %d)."),
            static_cast<int32>(Stage), static_cast<int32>(PreviewState), static_cast<int32>(PreviewBuildStage));
        return;
    }
    PreviewBuildStage = Stage;
}

bool AMtoULiveLinkActor::CompletePreviewBuild(
    USkeletalMesh* Mesh, bool bHasWarning, const FString& Diagnostics,
    const FString& Summary, const TArray<int32>& InDriverGarmentMaterialSlots)
{
    if (PreviewState != EMtoUPreviewState::Building)
    {
        ensureAlwaysMsgf(false, TEXT(
            "MtoU Preview readiness: ignored a commit outside Building (state %d); the current readiness is unchanged."),
            static_cast<int32>(PreviewState));
        return false;
    }

    if (!Mesh
        || Mesh->GetOuter() != this
        || !Mesh->HasAnyFlags(RF_Transient)
        || Mesh->HasAnyFlags(RF_Public | RF_Standalone))
    {
        // A Generated Preview with invalid ownership or persistence flags is a
        // Validation-stage failure; it can never become ready.
        FailPreviewBuild(
            EMtoUPreviewBuildStage::Validation,
            TEXT("Preview preparation returned a mesh that is not actor-owned transient data."));
        return false;
    }

    GeneratedPreviewMesh = Mesh;
    DriverGarmentMaterialSlots = InDriverGarmentMaterialSlots;
    PreviewState = bHasWarning ? EMtoUPreviewState::Warning : EMtoUPreviewState::Ready;
    PreviewBuildStage = EMtoUPreviewBuildStage::Validation;
    PreviewDiagnostics = Diagnostics;
    PreviewSummary = Summary.IsEmpty() ? Diagnostics : Summary;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    return true;
}

bool AMtoULiveLinkActor::FailPreviewBuild(
    EMtoUPreviewBuildStage Stage, const FString& Diagnostics)
{
    if (PreviewState != EMtoUPreviewState::Building)
    {
        ensureAlwaysMsgf(false, TEXT(
            "MtoU Preview readiness: ignored a failure outside Building (state %d); the current readiness is unchanged."),
            static_cast<int32>(PreviewState));
        return false;
    }

    ReleaseGeneratedPreview();
    HideDisplay();
    PreviewState = EMtoUPreviewState::Error;
    PreviewBuildStage = Stage;
    PreviewDiagnostics = Diagnostics;
    PreviewSummary = Diagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
    return true;
}

void AMtoULiveLinkActor::InvalidateGeneratedPreview(const FString& Diagnostics)
{
    ReleaseGeneratedPreview();
    EnterUnrefreshedReadiness(Diagnostics);
}

void AMtoULiveLinkActor::EnterUnrefreshedReadiness(const FString& Message)
{
    const bool bConfigured = Binding
        && Binding->SkeletalMesh
        && Binding->PreviewStaticMesh;
    PreviewState = bConfigured
        ? EMtoUPreviewState::Dirty
        : EMtoUPreviewState::None;
    PreviewBuildStage = EMtoUPreviewBuildStage::None;
    PreviewDiagnostics = bConfigured ? Message : FString();
    PreviewSummary = PreviewDiagnostics;
    ModelDiagnostics.Reset();
    ModelDiagnosticLevel = EMtoUModelDiagnosticLevel::None;
}

void AMtoULiveLinkActor::NotifyGeneratedPreviewDeleted()
{
    InvalidateGeneratedPreview(TEXT("Generated Preview deleted. Run Refresh Preview to rebuild it."));
    PreviewSummary.Reset();
    ShowDriverMesh();
}

void AMtoULiveLinkActor::NotifyTransientPreviewReleased()
{
    ReleaseGeneratedPreview();
    EnterUnrefreshedReadiness(TEXT("The transient Generated Preview was released. Run Refresh Preview."));
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
    DriverGarmentMaterialSlots.Reset();
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
    // Connection-time Model evidence is a separate concern: it is recorded
    // here and never modifies Ready or Warning readiness.
    ModelDiagnostics = Diagnostics;
    ModelDiagnosticLevel = Level;
}

void AMtoULiveLinkActor::NotifyBindingInputsChanged()
{
    RebindInputNotifications();
    InvalidateGeneratedPreview(TEXT("Binding inputs changed. Run Refresh Preview."));
    ReapplyDisplayTarget();
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
}

void AMtoULiveLinkActor::ReapplyDisplayTarget()
{
    if (!SkeletalMeshComponent || !DriverMeshComponent)
    {
        return;
    }

    DriverMeshComponent->SetLeaderPoseComponent(nullptr);
    DriverMeshComponent->SetForcedLOD(0);
    DriverMeshComponent->ShowAllMaterialSections(0);
    DriverMeshComponent->SetSkeletalMeshAsset(nullptr);
    DriverMeshComponent->SetLightingChannels(
        SkeletalMeshComponent->LightingChannels.bChannel0,
        SkeletalMeshComponent->LightingChannels.bChannel1,
        SkeletalMeshComponent->LightingChannels.bChannel2);
    DriverMeshComponent->SetCastInsetShadow(SkeletalMeshComponent->bCastInsetShadow);

    switch (DisplayTarget)
    {
    case EMtoUDisplayTarget::Driver:
        SkeletalMeshComponent->SetSkeletalMeshAsset(Binding ? Binding->SkeletalMesh : nullptr);
        break;
    case EMtoUDisplayTarget::GeneratedPreview:
        SkeletalMeshComponent->SetSkeletalMeshAsset(GeneratedPreviewMesh);
        if (Binding && Binding->SkeletalMesh && !DriverGarmentMaterialSlots.IsEmpty())
        {
            DriverMeshComponent->SetSkeletalMeshAsset(Binding->SkeletalMesh);
            DriverMeshComponent->SetForcedLOD(1);
            DriverMeshComponent->SetLeaderPoseComponent(SkeletalMeshComponent);
            for (const int32 MaterialSlot : DriverGarmentMaterialSlots)
            {
                DriverMeshComponent->ShowMaterialSection(
                    MaterialSlot, INDEX_NONE, false, 0);
            }
        }
        break;
    case EMtoUDisplayTarget::Hidden:
    default:
        SkeletalMeshComponent->SetSkeletalMeshAsset(nullptr);
        break;
    }
    SkeletalMeshComponent->SetDisablePostProcessBlueprint(true);
    SkeletalMeshComponent->SetUpdateAnimationInEditor(true);
    SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
    SkeletalMeshComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
    if (ULiveLinkInstance* Instance =
            Cast<ULiveLinkInstance>(SkeletalMeshComponent->GetAnimInstance()))
    {
        Instance->SetSubject(
            FLiveLinkSubjectName(FName(TEXT("MtoU_Character"))));
        Instance->EnableLiveLinkEvaluation(true);
    }
}

void AMtoULiveLinkActor::ApplyModelMorphCurves(
    const TArray<FName>& CurveNames,
    const TArray<float>& CurveValues)
{
    USkeletalMesh* DriverMesh = DriverMeshComponent
        ? DriverMeshComponent->GetSkeletalMeshAsset()
        : nullptr;
    if (DisplayTarget != EMtoUDisplayTarget::GeneratedPreview || !DriverMesh)
    {
        return;
    }
    const int32 CurveCount = FMath::Min(CurveNames.Num(), CurveValues.Num());
    for (int32 Index = 0; Index < CurveCount; ++Index)
    {
        if (DriverMesh->FindMorphTarget(CurveNames[Index]))
        {
            DriverMeshComponent->SetMorphTarget(CurveNames[Index], CurveValues[Index]);
        }
    }
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
    // Refresh runs synchronously on the Game Thread, so the only source
    // rebuild events that can arrive while Building are produced by this
    // build's own read of the source meshes; they must not self-invalidate.
    if (PreviewState == EMtoUPreviewState::Building)
    {
        return;
    }
    NotifySourceAssetChanged(ObservedDriverMesh.Get(), TEXT("source rebuild"));
}

void AMtoULiveLinkActor::HandlePreviewMeshChanged()
{
    if (PreviewState == EMtoUPreviewState::Building)
    {
        return;
    }
    NotifySourceAssetChanged(ObservedPreviewMesh.Get(), TEXT("PostEdit change"));
}

void AMtoULiveLinkActor::HandlePreviewMeshBuilt(UStaticMesh* Mesh)
{
    if (PreviewState == EMtoUPreviewState::Building)
    {
        return;
    }
    NotifySourceAssetChanged(Mesh, TEXT("source rebuild"));
}
