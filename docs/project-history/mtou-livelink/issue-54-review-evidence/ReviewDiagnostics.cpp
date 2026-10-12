#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Misc/ScopeExit.h"
#include "MtoUMultiSubjectFixture.h"
#include "MtoUMultiSubjectProtocol.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReviewOptionalAmbiguity, "Issue54Review.NonRequiredAmbiguity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FReviewOptionalAmbiguity::RunTest(const FString& Parameters)
{
    FMtoUMultiSubjectFixture Fixture;
    ON_SCOPE_EXIT { Fixture.Destroy(); };
    FString Error;
    if (!TestTrue(TEXT("fixture builds"), FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error))) return false;
    TArray<bool> Required;
    if (!TestTrue(TEXT("required set reads"), FMtoUMultiSubjectProtocol::CollectRequiredTargetBones(*Fixture.PropMesh, Required, Error))) return false;
    const int32 End = Fixture.PropMesh->GetRefSkeleton().FindBoneIndex(TEXT("PropTipEnd"));
    if (!TestTrue(TEXT("the disputed target is non-required"), Required.IsValidIndex(End) && !Required[End])) return false;
    FMtoUNegotiationMap Map;
    TestTrue(TEXT("unmodified declaration accepted"), FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(Fixture.PropDeclaration, *Fixture.PropMesh, Map).IsEmpty());
    {
        FReferenceSkeletonModifier Modifier(Fixture.PropMesh->GetRefSkeleton(), Fixture.PropSkeleton);
        Modifier.Rename(TEXT("PropTipEnd"), TEXT("End12"));
    }
    auto Declaration = Fixture.PropDeclaration;
    Declaration.Bones[3].Name = TEXT("End");
    auto Other = Declaration.Bones[3]; Other.Name = TEXT("End1");
    Declaration.Bones.Add(Other);
    const FTransform Bind = Declaration.Bind[3]; Declaration.Bind.Add(Bind);
    for (int32 Order=0; Order<2; ++Order)
    {
        if (Order) Swap(Declaration.Bones[3].Name, Declaration.Bones[4].Name);
        Error = FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(Declaration, *Fixture.PropMesh, Map);
        AddInfo(FString::Printf(TEXT("order=%d optional_target=%d required=false result=%s"), Order, End, *Error));
        TestTrue(TEXT("ambiguity entirely outside necessary dependencies must not veto the pair"), Error.IsEmpty());
    }
    Declaration.Bones.SetNum(3); Declaration.Bind.SetNum(3);
    Error = FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(Declaration, *Fixture.PropMesh, Map);
    AddInfo(TEXT("same necessary declaration without optional branches: ") + Error);
    TestTrue(TEXT("same required bones accepted after removing optional source branches"), Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReviewUniqueNameRename, "Issue54Review.UniqueNameRename",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FReviewUniqueNameRename::RunTest(const FString& Parameters)
{
    FMtoUMultiSubjectFixture Fixture;
    ON_SCOPE_EXIT { Fixture.Destroy(); };
    FString Error;
    if (!TestTrue(TEXT("fixture builds"), FMtoUMultiSubjectFixtureBuilder::Build(TEXT("character-prop"), Fixture, Error))) return false;
    {
        FReferenceSkeletonModifier Modifier(Fixture.CharacterMesh->GetRefSkeleton(), Fixture.CharacterSkeleton);
        Modifier.Rename(TEXT("Head"), TEXT("Joint12"));
    }
    auto Declaration = Fixture.CharacterDeclaration;
    Declaration.Bones[3].Name = TEXT("Joint");
    FMtoUNegotiationMap Map;
    Error = FMtoUMultiSubjectProtocol::DescribeNegotiationMismatch(Declaration, *Fixture.CharacterMesh, Map);
    AddInfo(FString::Printf(TEXT("unique Joint -> required Joint12: error='%s', target=%d, rename_count=%d"), *Error, Map.SourceToTarget[3], Map.ImportRenames.Num()));
    TestFalse(TEXT("product rename precondition rejects a globally unique short name"), Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReviewAnchorConvention, "Issue54Review.AnchorConvention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FReviewAnchorConvention::RunTest(const FString& Parameters)
{
    const FTransform Common(FRotator(0,25,0), FVector(320,-180,40));
    const FTransform Root(FQuat(FVector(1,0,0), PI/2));
    const FTransform Lift(FVector(0,0,400));
    // At rest the bind/current projection cancels Root, leaving target reference.
    const FTransform Projected = FTransform::Identity * Root.Inverse() * Root;
    const FTransform Actual = Projected * ((Common * Root) * Lift);
    const FTransform Expected = Projected * (Common * Lift);
    const double Degrees = FMath::RadiansToDegrees(Actual.GetRotation().AngularDistance(Expected.GetRotation()));
    AddInfo(FString::Printf(TEXT("real-pair anchor reintroduces %.6f degrees after bind projection"), Degrees));
    TestTrue(TEXT("import-only root convention must not rotate the world-space target"), Degrees < 0.001);
    return true;
}
#endif
