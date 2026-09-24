#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "MtoULiveLinkActor.h"

/** Details customization for the Binding actor. */
class FMtoULiveLinkActorDetails final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

    /** How urgent the combined Preview and connection state is. */
    enum class ESeverity : uint8
    {
        Neutral,
        Info,
        Success,
        Warning,
        Error
    };

    /** UI-only projection of the actor's independent Preview and connection state. */
    struct FStatusView
    {
        FText Preview;
        FText Connection;
        FText Summary;
        FText Cause;
        FText Candidates;
        FText RawDiagnostics;
        ESeverity PreviewSeverity = ESeverity::Neutral;
        ESeverity ConnectionSeverity = ESeverity::Neutral;
        /** Both axes in one line; the panel renders the two lines separately. */
        FText State;
        FText NextStep;
        FText Display;
        ESeverity Severity = ESeverity::Neutral;
    };

    /** The status row content for the actor's current public state. */
    static FStatusView MakeStatusView(TWeakObjectPtr<AMtoULiveLinkActor> Actor);

    /** Projection of a supplied readiness snapshot for focused Editor tests. */
    static FStatusView MakeStatusViewForReadiness(
        TWeakObjectPtr<AMtoULiveLinkActor> Actor, const FMtoUPreviewReadiness& Readiness);

    /**
     * The status row's shared cache. Every binding of the row reads one
     * presenter, so a Slate poll keeps the formatted status instead of rebuilding
     * it, and rebuilds it as soon as the actor state it was built from moves.
     * The actor is held weakly, so a destroyed actor can never serve a stale view.
     */
    class FStatusPresenter
    {
    public:
        explicit FStatusPresenter(TWeakObjectPtr<AMtoULiveLinkActor> InActor = nullptr);

        /** The status for the actor's current state, rebuilt only when that state changed. */
        const FStatusView& Get();

    private:
        /** The actor state the cached view was built from. */
        struct FKey
        {
            bool bHasActor = false;
            EMtoUPreviewState State = EMtoUPreviewState::None;
            EMtoUPreviewBuildStage Stage = EMtoUPreviewBuildStage::None;
            FString PreviewDiagnostics;
            FString PreviewSummary;
            FString Connection;
            FString ModelDiagnostics;
            FString CharacterDiagnostics;
            EMtoUDisplayTarget Display = EMtoUDisplayTarget::Hidden;
        };

        TWeakObjectPtr<AMtoULiveLinkActor> Actor;
        bool bHasView = false;
        FKey CachedKey;
        FStatusView View;
    };

    /** True while the "Delete Preview" action can remove a usable Generated Preview. */
    static bool CanDeletePreview(TWeakObjectPtr<AMtoULiveLinkActor> Actor);

    /**
     * Exact body of the Details "Refresh Preview" button click: scoped slow
     * task plus the public RefreshActor path with per-stage progress.
     * Named so automation executes the same code the button invokes.
     */
    static FReply HandleRefreshPreviewClicked(TWeakObjectPtr<AMtoULiveLinkActor> Actor);
};
