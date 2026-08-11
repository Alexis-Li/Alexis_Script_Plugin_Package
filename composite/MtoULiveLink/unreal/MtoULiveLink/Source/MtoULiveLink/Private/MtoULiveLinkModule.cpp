#include "MtoULiveLinkSource.h"

#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
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
        Source = MakeShared<FMtoULiveLinkSource>();
        Client.AddSource(Source);
    }

    virtual void ShutdownModule() override
    {
        if (!Source)
        {
            return;
        }
        Source->StopListener();
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
};

IMPLEMENT_MODULE(FMtoULiveLinkModule, MtoULiveLink)
