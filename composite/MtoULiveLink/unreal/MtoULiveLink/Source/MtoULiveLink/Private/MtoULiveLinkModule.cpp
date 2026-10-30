#include "MtoULiveLinkSource.h"

#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

class FMtoULiveLinkModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (Source)
        {
            return;
        }
        if (!ensureMsgf(
                FModuleManager::Get().LoadModule(TEXT("LiveLink")),
                TEXT("MtoULiveLink depends on the LiveLink module.")))
        {
            return;
        }

        IModularFeatures& Features = IModularFeatures::Get();
        if (!ensureMsgf(
                Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName),
                TEXT("The Live Link client modular feature is unavailable.")))
        {
            return;
        }
        ILiveLinkClient& Client = Features.GetModularFeature<ILiveLinkClient>(
            ILiveLinkClient::ModularFeatureName);
        // World unloads and editor shutdown reach the shared idempotent
        // termination boundary through the synchronous world-cleanup
        // broadcast, because Actor::Destroyed() never runs on the
        // DestroyWorld path.
        WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(
            &MtoUNotifyEditorWorldCleanup);
        // `-MtoULiveLinkPort=<n>` moves the listener off 54321, so a test
        // editor and a person's own editor can run on one machine.
        uint32 Port = 54321;
        FParse::Value(FCommandLine::Get(), TEXT("MtoULiveLinkPort="), Port);
        Source = MakeShared<FMtoULiveLinkSource>(
            static_cast<uint16>(Port >= 1 && Port <= 65535 ? Port : 54321));
        MtoUSetActiveSource(Source);
        Client.AddSource(Source);
    }

    virtual void ShutdownModule() override
    {
        if (WorldCleanupHandle.IsValid())
        {
            FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
            WorldCleanupHandle.Reset();
        }
        if (!Source)
        {
            return;
        }
        Source->StopListener();
        MtoUSetActiveSource(nullptr);
        IModularFeatures& Features = IModularFeatures::Get();
        if (Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
        {
            Features.GetModularFeature<ILiveLinkClient>(
                ILiveLinkClient::ModularFeatureName).RemoveSource(Source);
        }
        Source.Reset();
    }

private:
    TSharedPtr<FMtoULiveLinkSource> Source;
    FDelegateHandle WorldCleanupHandle;
};

IMPLEMENT_MODULE(FMtoULiveLinkModule, MtoULiveLink)
