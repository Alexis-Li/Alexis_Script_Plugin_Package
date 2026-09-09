#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class AMtoULiveLinkActor;

/** Details customization for the Binding actor (MtoU Preview category). */
class FMtoULiveLinkActorDetails final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

    /**
     * Exact body of the Details "Refresh Preview" button click: scoped slow
     * task plus the public RefreshActor path with per-stage progress.
     * Named so automation executes the same code the button invokes.
     */
    static FReply HandleRefreshPreviewClicked(TWeakObjectPtr<AMtoULiveLinkActor> Actor);
};
