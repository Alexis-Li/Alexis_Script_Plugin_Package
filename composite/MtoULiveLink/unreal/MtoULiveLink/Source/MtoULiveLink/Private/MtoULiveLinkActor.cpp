#include "MtoULiveLinkActor.h"

#include "MtoUCharacterComposition.h"
#include "MtoULiveLinkBinding.h"
#include "MtoULiveLinkSource.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "LiveLinkInstance.h"
#include "UObject/UObjectThreadContext.h"

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
    // PostLoad may not initialize animation or register dynamic components:
    // assigning a mesh can execute its Post Process Blueprint immediately.
    // PostRegisterAllComponents restores the display once loading has finished.
    DisplayTarget = EMtoUDisplayTarget::Driver;
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
    MtoURequestStreamingSessionEnd();
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

    MtoURequestStreamingSessionEnd();
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

    // An explicit refresh ends any active session owning the Binding Actor
    // before replacing its display (Issue #39). The idempotent boundary is
    // also the synchronous publish gate: old queued frames and cached
    // commands fail closed from here, without waiting for the worker to
    // close the socket. A rejected reentrant start above never terminates.
    MtoURequestStreamingSessionEnd();
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
    // A failed refresh keeps Error readiness with no usable Generated Preview,
    // but restores the bound Driver for inspection (Issue #35). Model preview
    // stays blocked because readiness is not usable; display and usability
    // are separate concerns. Without a bound Driver there is nothing to show.
    if (Binding && Binding->SkeletalMesh)
    {
        ShowDriverMesh();
    }
    else
    {
        HideDisplay();
    }
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
    MtoURequestStreamingSessionEnd();
    InvalidateGeneratedPreview(TEXT("Generated Preview deleted. Run Refresh Preview to rebuild it."));
    PreviewSummary.Reset();
    ShowDriverMesh();
}

void AMtoULiveLinkActor::NotifyTransientPreviewReleased()
{
    MtoURequestStreamingSessionEnd();
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
    // Primary Driver and Preview inputs are the Preview revision, but they are
    // also the composition baseline every Additional Part is validated
    // against, so the resolved composition is republished here too.
    SyncCharacterComposition();
    MtoURequestStreamingSessionEnd();
    RebindInputNotifications();
    InvalidateGeneratedPreview(TEXT("Binding inputs changed. Run Refresh Preview."));
    ReapplyDisplayTarget();
}

void AMtoULiveLinkActor::NotifyCharacterPartsChanged()
{
    // One composition boundary: it resolves the new parts, ends an outdated
    // session, and resynchronizes the components, their sources, and their
    // display.
    SyncCharacterComposition();
}

void AMtoULiveLinkActor::NotifyBindingStateChanged()
{
    // The actor's observed Primary Driver, Preview Static Mesh, and Garment
    // Slot Override are what it last applied; a different input means the
    // Preview revision changed.
    const bool bPreviewInputsChanged = !Binding
        || Binding->SkeletalMesh != ObservedDriverMesh.Get()
        || Binding->PreviewStaticMesh != ObservedPreviewMesh.Get()
        || Binding->DriverGarmentSlotOverride != ObservedGarmentSlotOverride;
    if (bPreviewInputsChanged)
    {
        NotifyBindingInputsChanged();
        return;
    }
    NotifyCharacterPartsChanged();
}

void AMtoULiveLinkActor::NotifySourceAssetChanged(const UObject* Asset, const FString& Reason)
{
    if (!Binding || !Asset
        || (Asset != Binding->SkeletalMesh && Asset != Binding->PreviewStaticMesh))
    {
        return;
    }
    // One terminal boundary for every relevant Preview revision change
    // (ADR-0002): the active session ends before the revision is dirtied,
    // and only an explicit Refresh plus a fresh connection can stream again.
    MtoURequestStreamingSessionEnd();
    InvalidateGeneratedPreview(FString::Printf(
        TEXT("%s changed (%s). Run Refresh Preview."), *Asset->GetName(), *Reason));
    // A rebuilt Primary Driver can also change whether an Additional Part is
    // still compatible, so the composition diagnostics are re-resolved here.
    SyncCharacterComposition();
}

void AMtoULiveLinkActor::RefreshBinding()
{
    if (FUObjectThreadContext::Get().IsRoutingPostLoad) { return; }
    SyncCharacterComposition();
    RebindInputNotifications();
    ReapplyDisplayTarget();
}

void AMtoULiveLinkActor::ReapplyDisplayTarget()
{
    if (FUObjectThreadContext::Get().IsRoutingPostLoad) { return; }
    if (!SkeletalMeshComponent || !DriverMeshComponent)
    {
        return;
    }

    DriverMeshComponent->SetLeaderPoseComponent(nullptr);
    DriverMeshComponent->SetForcedLOD(0);
    DriverMeshComponent->ShowAllMaterialSections(0);
    DriverMeshComponent->SetSkeletalMeshAsset(nullptr);
    InheritPrimaryDisplaySettings(*DriverMeshComponent);

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
    ConfigureLiveLinkInstance(*SkeletalMeshComponent);
    ApplyCharacterPartDisplay();
}

void AMtoULiveLinkActor::ConfigureLiveLinkInstance(USkeletalMeshComponent& Component)
{
    Component.SetDisablePostProcessBlueprint(true);
    Component.SetUpdateAnimationInEditor(true);
    Component.SetAnimationMode(EAnimationMode::AnimationBlueprint);
    Component.SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
    if (ULiveLinkInstance* Instance = Cast<ULiveLinkInstance>(Component.GetAnimInstance()))
    {
        // Every part evaluates the one character subject, so the whole composed
        // character poses from the single streaming session.
        Instance->SetSubject(
            FLiveLinkSubjectName(FName(TEXT("MtoU_Character"))));
        Instance->EnableLiveLinkEvaluation(true);
    }
}

void AMtoULiveLinkActor::InheritPrimaryDisplaySettings(USkeletalMeshComponent& Component)
{
    // A secondary display component shows the same character, so it follows
    // the primary's lighting and shadow settings instead of owning its own.
    Component.SetLightingChannels(
        SkeletalMeshComponent->LightingChannels.bChannel0,
        SkeletalMeshComponent->LightingChannels.bChannel1,
        SkeletalMeshComponent->LightingChannels.bChannel2);
    Component.SetCastInsetShadow(SkeletalMeshComponent->bCastInsetShadow);
}

void AMtoULiveLinkActor::SyncCharacterComposition()
{
    if (FUObjectThreadContext::Get().IsRoutingPostLoad) { return; }
    const FMtoUCharacterComposition Composition = FMtoUCharacterComposition::Resolve(Binding);
    CharacterPartDiagnostics = Composition.Diagnostics;
    CharacterPartSummary = Composition.Summary;

    if (CharacterPartsMatch(Composition))
    {
        // Renaming a part or reordering the list changes neither a session nor
        // a display component; only the enabled composition is observable.
        return;
    }
    // Adding, removing, enabling, disabling, or replacing a part invalidates
    // the negotiated composition: the running session must end before its
    // frames and cached commands can reach the new component set. Removing the
    // Primary Driver removes the whole character, parts included.
    MtoURequestStreamingSessionEnd();
    ReconcileCharacterPartComponents(Composition);
    // The component set changed, so its source observations and its display
    // follow here instead of in every caller.
    RebindInputNotifications();
    ApplyCharacterPartDisplay();
}

bool AMtoULiveLinkActor::CharacterPartsMatch(const FMtoUCharacterComposition& Composition) const
{
    const TArray<FMtoUCharacterPartIdentity>& Targets = Composition.GetEnabledIdentity();
    if (CharacterPartComponents.Num() != Targets.Num())
    {
        return false;
    }
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const UMtoUCharacterPartComponent* Component = CharacterPartComponents[Index];
        if (!Component || !Component->MatchesPart(Targets[Index].PartId, Targets[Index].Mesh))
        {
            return false;
        }
    }
    return true;
}

void AMtoULiveLinkActor::ReconcileCharacterPartComponents(
    const FMtoUCharacterComposition& Composition)
{
    // Adopt every part component the actor still owns. A component the current
    // composition does not claim is a leftover, for example one the engine
    // duplicated together with the actor; it is destroyed instead of reused.
    TArray<UMtoUCharacterPartComponent*> Previous;
    GetComponents(Previous);
    for (const TObjectPtr<UMtoUCharacterPartComponent>& Component : CharacterPartComponents)
    {
        if (Component && !Previous.Contains(Component))
        {
            Previous.Add(Component);
        }
    }

    const TArray<FMtoUCharacterPartIdentity>& Targets = Composition.GetEnabledIdentity();
    CharacterPartComponents.Reset(Targets.Num());
    for (const FMtoUCharacterPartIdentity& Target : Targets)
    {
        UMtoUCharacterPartComponent* Component = nullptr;
        for (int32 Index = Previous.Num() - 1; Index >= 0; --Index)
        {
            UMtoUCharacterPartComponent* Candidate = Previous[Index];
            if (Candidate && Candidate->MatchesPart(Target.PartId, Target.Mesh))
            {
                Component = Candidate;
                Previous.RemoveAt(Index, EAllowShrinking::No);
                break;
            }
        }
        if (!Component)
        {
            Component = NewObject<UMtoUCharacterPartComponent>(this);
            Component->PartId = Target.PartId;
            Component->PartMesh = Target.Mesh;
            Component->SetupAttachment(SkeletalMeshComponent);
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetGenerateOverlapEvents(false);
            Component->SetDisablePostProcessBlueprint(true);
            Component->RegisterComponent();
        }
        CharacterPartComponents.Add(Component);
    }

    for (UMtoUCharacterPartComponent* Stale : Previous)
    {
        if (Stale)
        {
            DestroyCharacterPartComponent(*Stale);
        }
    }
}

void AMtoULiveLinkActor::DestroyCharacterPartComponent(UMtoUCharacterPartComponent& Component)
{
    // Drop the display and the animation state before unregistering, so a
    // disabled or replaced part can never leave a visible or morph-carrying
    // component behind.
    Component.ClearMorphTargets();
    Component.SetSkeletalMeshAsset(nullptr);
    Component.SetAnimInstanceClass(nullptr);
    Component.PartMesh = nullptr;
    Component.DestroyComponent();
}

void AMtoULiveLinkActor::ApplyCharacterPartDisplay()
{
    for (const TObjectPtr<UMtoUCharacterPartComponent>& Component : CharacterPartComponents)
    {
        if (!Component)
        {
            continue;
        }
        // Parts follow the character display selection: the Driver and the
        // Generated Preview both show the whole composed character, while the
        // hidden state leaves no visible attachment behind.
        InheritPrimaryDisplaySettings(*Component);
        // Disable Post Process evaluation before assigning the mesh. UE may
        // still initialize its instance; this runs only after PostLoad.
        Component->SetDisablePostProcessBlueprint(true);
        Component->SetSkeletalMeshAsset(
            DisplayTarget == EMtoUDisplayTarget::Hidden ? nullptr : Component->PartMesh);
        ConfigureLiveLinkInstance(*Component);
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
    // The actor now observes, and has applied, these Preview inputs.
    ObservedGarmentSlotOverride = Binding->DriverGarmentSlotOverride;
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
    for (const TObjectPtr<UMtoUCharacterPartComponent>& Component : CharacterPartComponents)
    {
        if (!Component || !Component->PartMesh)
        {
            continue;
        }
        FMtoUObservedPartMesh& Observed = ObservedPartMeshes.AddDefaulted_GetRef();
        Observed.Mesh = Component->PartMesh;
        Observed.ChangedHandle = Component->PartMesh->GetOnMeshChanged().AddUObject(
            this, &AMtoULiveLinkActor::HandleCharacterPartMeshChanged);
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
    for (const FMtoUObservedPartMesh& Observed : ObservedPartMeshes)
    {
        if (Observed.Mesh.IsValid())
        {
            Observed.Mesh->GetOnMeshChanged().Remove(Observed.ChangedHandle);
        }
    }
    ObservedPartMeshes.Reset();
    ObservedDriverMesh.Reset();
    ObservedPreviewMesh.Reset();
    ObservedGarmentSlotOverride.Reset();
    DriverMeshChangedHandle.Reset();
    PreviewMeshChangedHandle.Reset();
    PreviewMeshBuiltHandle.Reset();
}

void AMtoULiveLinkActor::HandleCharacterPartMeshChanged()
{
    // Refresh runs synchronously on the Game Thread, so the only source
    // rebuild events that can arrive while Building are produced by this
    // build's own read of the source meshes; they must not self-invalidate.
    if (PreviewState == EMtoUPreviewState::Building)
    {
        return;
    }
    // A rebuilt part can change its Skeleton, bones, Morph library, or
    // reference pose, so the negotiated composition is stale: end the session
    // and let the components and sources resynchronize. The garment Preview
    // revision depends on the Primary Driver and the imported Preview, never
    // on a part, so readiness survives.
    MtoURequestStreamingSessionEnd();
    SyncCharacterComposition();
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
