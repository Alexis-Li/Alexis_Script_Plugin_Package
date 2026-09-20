#pragma once

#include "Engine/DataAsset.h"

#include "MtoULiveLinkBinding.generated.h"

class USkeletalMesh;
class UStaticMesh;

/**
 * One additional character part of a Binding: a Skeletal Mesh displayed beside
 * the Primary Driver under the same character, streaming session, and Live Link
 * subject. An enabled part must share the Primary's Skeleton, and every bone it
 * contains must map by name and parent path onto the Primary with a compatible
 * reference pose. A disabled part takes part in neither negotiation nor display.
 */
USTRUCT(BlueprintType)
struct MTOULIVELINK_API FMtoUCharacterPart
{
    GENERATED_BODY()

    /**
     * Stable internal identity of this part. It lets the Binding actor reuse
     * one display component across array edits, undo, redo, and duplication,
     * and it carries no user configuration meaning; the Binding keeps it
     * unique and regenerates it when it is missing or repeated.
     */
    UPROPERTY(VisibleAnywhere, Category = "MtoU_LiveLink", meta = (EditCondition = "false"))
    FGuid PartId;

    /**
     * User-facing name used by composition diagnostics. An empty name falls
     * back to the Skeletal Mesh name so every part stays identifiable.
     */
    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Name"))
    FString PartName;

    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Skeletal Mesh"))
    TObjectPtr<USkeletalMesh> SkeletalMesh;

    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Enabled"))
    bool bEnabled = true;
};

UCLASS(BlueprintType, meta = (DisplayName = "MtoU_LiveLink Binding"))
class MTOULIVELINK_API UMtoULiveLinkBinding : public UDataAsset
{
    GENERATED_BODY()

public:
    /**
     * The Primary Driver Skeletal Mesh of the character. It carries the
     * complete deformation hierarchy, supplies the only garment Preview data,
     * and defines the skeleton baseline every enabled Additional Part must map
     * onto.
     */
    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink",
        meta = (DisplayName = "Primary Driver Skeletal Mesh"))
    TObjectPtr<USkeletalMesh> SkeletalMesh;

    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink", meta = (DisplayName = "Preview Static Mesh"))
    TObjectPtr<UStaticMesh> PreviewStaticMesh;

    /**
     * Optional advanced manual override of the Driver garment source. Lists
     * stable imported Driver material-slot names; an empty list keeps automatic
     * garment resolution. Names must stay unique and present on the current
     * Driver import or Refresh fails before any build.
     */
    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "MtoU_LiveLink",
        meta = (DisplayName = "Driver Garment Slot Override",
            Tooltip = "Manual selection of Driver source material slots for garment resolution. Leave empty for automatic resolution from geometry with optional material-slot evidence. Entries are stable imported Driver slot names (never numeric section indices) and every name must exist exactly once on the current Driver import. Selected regions still pass the same geometry coverage and alignment validation as automatic results."))
    TArray<FName> DriverGarmentSlotOverride;

    /**
     * Character parts added beside the Primary Driver. The list is empty for
     * every Binding that predates this field, which keeps a single-mesh
     * Binding working without migration. Entries are validated when the
     * character connects, not when the asset is edited.
     */
    UPROPERTY(EditAnywhere, Category = "MtoU_LiveLink",
        meta = (DisplayName = "Additional Parts", TitleProperty = "PartName",
            Tooltip = "Additional Skeletal Meshes of the same character, for example a separated Head. Every enabled part must share the Primary Driver Skeleton, map its bones by name and parent path onto the Primary, and match the Primary reference pose for those bones. Body, face, and BlendShape preview keep working through the Primary, so a part never changes garment resolution, weight transfer, or Preview Morph transfer."))
    TArray<FMtoUCharacterPart> AdditionalParts;

    virtual void PostLoad() override;

    /** Gives every part a stable unique identity, repairing missing and repeated ones. */
    void EnsureCharacterPartIds();

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
