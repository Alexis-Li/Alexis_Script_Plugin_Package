#pragma once

#include "GameFramework/Actor.h"

#include "MtoULiveLinkActor.generated.h"

class UMtoULiveLinkBinding;
class USkeletalMeshComponent;
class USkeletalMesh;

UCLASS()
class MTOULIVELINK_API AMtoULiveLinkActor : public AActor
{
    GENERATED_BODY()

public:
    AMtoULiveLinkActor();

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;

    void SetBinding(UMtoULiveLinkBinding* InBinding);
    void SetConnectionStatus(const FString& InStatus);
    USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }
    UMtoULiveLinkBinding* GetBinding() const { return Binding; }
    const FString& GetConnectionStatus() const { return ConnectionStatus; }

    /** True while the actor owns a complete, transactional Generated Preview. */
    bool HasReadyGeneratedPreview() const { return GeneratedPreviewMesh != nullptr; }
    USkeletalMesh* GetGeneratedPreviewMesh() const { return GeneratedPreviewMesh; }

private:
    void RefreshBinding();

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;

    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<UMtoULiveLinkBinding> Binding;

    UPROPERTY(Transient, VisibleAnywhere, Category = "MtoU_LiveLink")
    TObjectPtr<USkeletalMesh> GeneratedPreviewMesh;

    UPROPERTY(VisibleAnywhere, Transient, Category = "MtoU_LiveLink")
    FString ConnectionStatus = TEXT("Disconnected");
};
