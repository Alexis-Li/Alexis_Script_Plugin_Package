#if WITH_DEV_AUTOMATION_TESTS

#include "MtoULiveLinkFactories.h"

#include "MtoULiveLinkActor.h"
#include "MtoULiveLinkBinding.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoULiveLinkFactoriesTest,
    "MtoULiveLink.Editor.Factories",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMtoULiveLinkFactoriesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UMtoULiveLinkBindingFactory* BindingFactory = NewObject<UMtoULiveLinkBindingFactory>();
    UMtoULiveLinkBinding* Binding = Cast<UMtoULiveLinkBinding>(BindingFactory->FactoryCreateNew(
        UMtoULiveLinkBinding::StaticClass(), GetTransientPackage(), NAME_None,
        RF_Transient, nullptr, GWarn));
    TestNotNull(TEXT("binding factory creates its supported asset"), Binding);
    TestTrue(TEXT("new binding participates in editor transactions"),
        Binding && Binding->HasAnyFlags(RF_Transactional));

    UMtoULiveLinkActorFactory* ActorFactory = NewObject<UMtoULiveLinkActorFactory>();
    FText Error;
    TestFalse(TEXT("an empty binding cannot be placed"),
        Binding && ActorFactory->CanCreateActorFrom(FAssetData(Binding), Error));
    TestEqual(TEXT("empty binding gives the actionable placement error"), Error.ToString(),
        FString(TEXT("Select a Skeletal Mesh on the binding before placing it.")));

    if (!Binding)
    {
        return false;
    }
    USkeletalMesh* TemplateMesh = LoadObject<USkeletalMesh>(
        nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    TestNotNull(TEXT("stock skeletal mesh template is available"), TemplateMesh);
    if (!TemplateMesh)
    {
        return false;
    }
    Binding->SkeletalMesh = NewObject<USkeletalMesh>(GetTransientPackage(),
        USkeletalMesh::StaticClass(), NAME_None, RF_Transient, TemplateMesh);
    TestTrue(TEXT("placement fixture is transient"),
        Binding->SkeletalMesh->HasAnyFlags(RF_Transient));
    TestTrue(TEXT("a binding with a skeletal mesh can be placed"),
        ActorFactory->CanCreateActorFrom(FAssetData(Binding), Error));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    TestNotNull(TEXT("editor world is created"), World);
    const FTransform DropTransform(FRotator(10.0, 20.0, 30.0), FVector(100.0, 200.0, 300.0));
    AddExpectedError(TEXT("has no render data"),
        EAutomationExpectedErrorFlags::Contains, 2);
    AMtoULiveLinkActor* Actor = World
        ? Cast<AMtoULiveLinkActor>(ActorFactory->CreateActor(
            Binding, World->GetCurrentLevel(), DropTransform))
        : nullptr;
    TestNotNull(TEXT("configured binding creates the binding actor"), Actor);
    if (Actor)
    {
        TestTrue(TEXT("spawned actor retains the binding"), Actor->GetBinding() == Binding);
        TestTrue(TEXT("actor factory preserves the drop transform"),
            Actor->GetActorTransform().Equals(DropTransform));
        TestTrue(TEXT("actor factory resolves the asset from the actor"),
            ActorFactory->GetAssetFromActorInstance(Actor) == Binding);
    }
    if (World)
    {
        World->DestroyWorld(false);
    }
    return true;
}

#endif
