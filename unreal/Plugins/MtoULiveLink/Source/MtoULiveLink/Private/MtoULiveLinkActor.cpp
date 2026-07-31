#include "MtoULiveLinkActor.h"

#include "MtoULiveLinkBinding.h"

#include "Components/SkeletalMeshComponent.h"
#include "LiveLinkInstance.h"

AMtoULiveLinkActor::AMtoULiveLinkActor()
{
    SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMeshComponent"));
    SetRootComponent(SkeletalMeshComponent);
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

void AMtoULiveLinkActor::SetBinding(UMtoULiveLinkBinding* InBinding)
{
    Binding = InBinding;
    RefreshBinding();
}

void AMtoULiveLinkActor::SetConnectionStatus(const FString& InStatus)
{
    ConnectionStatus = InStatus;
}

void AMtoULiveLinkActor::RefreshBinding()
{
    SkeletalMeshComponent->SetSkeletalMeshAsset(Binding ? Binding->SkeletalMesh : nullptr);
    SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
    SkeletalMeshComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
    if (ULiveLinkInstance* Instance = Cast<ULiveLinkInstance>(SkeletalMeshComponent->GetAnimInstance()))
    {
        Instance->SetSubject(FLiveLinkSubjectName(FName(TEXT("MtoU_Character"))));
        Instance->EnableLiveLinkEvaluation(true);
    }
}
