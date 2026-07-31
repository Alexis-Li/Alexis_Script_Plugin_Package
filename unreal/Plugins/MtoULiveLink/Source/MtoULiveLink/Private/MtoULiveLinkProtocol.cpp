#include "MtoULiveLinkProtocol.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool ParseObject(const TArray<uint8>& Payload, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
    if (Payload.IsEmpty())
    {
        OutError = TEXT("Empty JSON payload.");
        return false;
    }

    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
    const FString Text = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
    {
        OutError = TEXT("Payload must be a JSON object.");
        return false;
    }
    return true;
}

bool GetTypedField(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    EJson Type,
    TSharedPtr<FJsonValue>& OutValue,
    FString& OutError)
{
    OutValue = Object->TryGetField(Name);
    if (!OutValue.IsValid() || OutValue->Type != Type)
    {
        OutError = FString::Printf(TEXT("Field '%s' has the wrong JSON type."), Name);
        return false;
    }
    return true;
}

bool GetStringField(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    FString& OutValue,
    FString& OutError)
{
    TSharedPtr<FJsonValue> Value;
    return GetTypedField(Object, Name, EJson::String, Value, OutError)
        && Value->TryGetString(OutValue);
}

bool GetArrayField(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    const TArray<TSharedPtr<FJsonValue>>*& OutValue,
    FString& OutError)
{
    TSharedPtr<FJsonValue> Value;
    return GetTypedField(Object, Name, EJson::Array, Value, OutError)
        && Value->TryGetArray(OutValue);
}

bool GetExactInt(const TSharedPtr<FJsonValue>& Value, int32& OutValue)
{
    double Number = 0.0;
    if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
        || !FMath::IsFinite(Number) || Number < MIN_int32 || Number > MAX_int32)
    {
        return false;
    }
    OutValue = static_cast<int32>(Number);
    return static_cast<double>(OutValue) == Number;
}

bool GetNumber(const TSharedPtr<FJsonValue>& Value, double& OutValue)
{
    return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(OutValue);
}

TArray<uint8> EncodeObject(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Object, Writer);

    FTCHARToUTF8 Converted(*Text);
    const uint64 PayloadLength = static_cast<uint64>(Converted.Length());
    TArray<uint8> Packet;
    Packet.SetNumUninitialized(8);
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Packet[Index] = static_cast<uint8>(PayloadLength >> ((7 - Index) * 8));
    }
    Packet.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Packet;
}

TMap<FName, FName> ParentMap(const TArray<FMtoUBone>& Bones)
{
    TMap<FName, FName> Result;
    for (const FMtoUBone& Bone : Bones)
    {
        const FName Parent = Bones.IsValidIndex(Bone.ParentIndex)
            ? Bones[Bone.ParentIndex].Name
            : NAME_None;
        Result.Add(Bone.Name, Parent);
    }
    return Result;
}

void AppendSection(FString& Diagnostic, const TCHAR* Heading, const TArray<FString>& Lines)
{
    Diagnostic += Heading;
    Diagnostic += TEXT(":\n");
    if (Lines.IsEmpty())
    {
        Diagnostic += TEXT("  (none)\n");
        return;
    }
    for (const FString& Line : Lines)
    {
        Diagnostic += TEXT("  ");
        Diagnostic += Line;
        Diagnostic += TEXT("\n");
    }
}
}

void FMtoUFrameDecoder::Append(const uint8* Data, int32 Num)
{
    if (Data && Num > 0)
    {
        Buffer.Append(Data, Num);
    }
}

EMtoUDecodeResult FMtoUFrameDecoder::Pop(TArray<uint8>& OutPayload, FString& OutError)
{
    OutPayload.Reset();
    OutError.Reset();
    if (Buffer.Num() < 8)
    {
        return EMtoUDecodeResult::NeedMore;
    }

    uint64 PayloadLength = 0;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        PayloadLength = (PayloadLength << 8) | Buffer[Index];
    }
    if (PayloadLength > MAX_int32)
    {
        OutError = TEXT("Payload length exceeds Unreal's int32 container range.");
        return EMtoUDecodeResult::Error;
    }
    if (static_cast<uint64>(Buffer.Num()) < PayloadLength + 8)
    {
        return EMtoUDecodeResult::NeedMore;
    }

    const int32 PayloadNum = static_cast<int32>(PayloadLength);
    OutPayload.Append(Buffer.GetData() + 8, PayloadNum);
    Buffer.RemoveAt(0, PayloadNum + 8, EAllowShrinking::No);
    return EMtoUDecodeResult::Message;
}

bool FMtoUProtocol::ParseInit(
    const TArray<uint8>& Payload,
    FMtoUInitMessage& OutMessage,
    FString& OutError)
{
    OutMessage = FMtoUInitMessage();
    OutError.Reset();
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }

    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("init"))
    {
        OutError = TEXT("Message type must be 'init'.");
        return false;
    }

    TSharedPtr<FJsonValue> VersionValue;
    int32 Version = 0;
    if (!GetTypedField(Object, TEXT("version"), EJson::Number, VersionValue, OutError)
        || !GetExactInt(VersionValue, Version) || Version != 1)
    {
        OutError = TEXT("Unsupported protocol version; expected 1.");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* CurveValues = nullptr;
    if (!GetArrayField(Object, TEXT("bones"), BoneValues, OutError)
        || !GetArrayField(Object, TEXT("curves"), CurveValues, OutError))
    {
        return false;
    }
    if (BoneValues->IsEmpty())
    {
        OutError = TEXT("Skeleton must contain exactly one root bone.");
        return false;
    }

    TSet<FName> BoneNames;
    OutMessage.Bones.Reserve(BoneValues->Num());
    for (int32 Index = 0; Index < BoneValues->Num(); ++Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Record = nullptr;
        if (!(*BoneValues)[Index].IsValid() || (*BoneValues)[Index]->Type != EJson::Array
            || !(*BoneValues)[Index]->TryGetArray(Record) || Record->Num() != 2)
        {
            OutError = FString::Printf(TEXT("Bone %d must be [name, parent_index]."), Index);
            return false;
        }

        FString NameText;
        int32 ParentIndex = INDEX_NONE;
        if (!(*Record)[0].IsValid() || (*Record)[0]->Type != EJson::String
            || !(*Record)[0]->TryGetString(NameText) || NameText.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Bone %d requires a non-empty string name."), Index);
            return false;
        }
        if (!GetExactInt((*Record)[1], ParentIndex))
        {
            OutError = FString::Printf(TEXT("Bone %d requires an integer parent index."), Index);
            return false;
        }
        if ((Index == 0 && ParentIndex != INDEX_NONE)
            || (Index > 0 && (ParentIndex < 0 || ParentIndex >= Index)))
        {
            OutError = FString::Printf(TEXT("Bone %d has an invalid parent-before-child index."), Index);
            return false;
        }

        const FName Name(*NameText);
        if (BoneNames.Contains(Name))
        {
            OutError = FString::Printf(TEXT("Duplicate bone name: %s."), *NameText);
            return false;
        }
        BoneNames.Add(Name);
        OutMessage.Bones.Add({Name, ParentIndex});
    }

    TSet<FName> CurveNames;
    OutMessage.Curves.Reserve(CurveValues->Num());
    for (int32 Index = 0; Index < CurveValues->Num(); ++Index)
    {
        FString NameText;
        if (!(*CurveValues)[Index].IsValid() || (*CurveValues)[Index]->Type != EJson::String
            || !(*CurveValues)[Index]->TryGetString(NameText) || NameText.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Curve %d requires a non-empty string name."), Index);
            return false;
        }
        const FName Name(*NameText);
        if (CurveNames.Contains(Name))
        {
            OutError = FString::Printf(TEXT("Duplicate curve name: %s."), *NameText);
            return false;
        }
        CurveNames.Add(Name);
        OutMessage.Curves.Add(Name);
    }
    return true;
}

bool FMtoUProtocol::ParseFrame(
    const TArray<uint8>& Payload,
    FMtoUFrameMessage& OutMessage,
    FString& OutError)
{
    OutMessage = FMtoUFrameMessage();
    OutError.Reset();
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }

    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("frame"))
    {
        OutError = TEXT("Message type must be 'frame'.");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* TransformValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* CurveValues = nullptr;
    if (!GetArrayField(Object, TEXT("transforms"), TransformValues, OutError)
        || !GetArrayField(Object, TEXT("curves"), CurveValues, OutError))
    {
        return false;
    }

    OutMessage.Transforms.Reserve(TransformValues->Num());
    for (int32 Index = 0; Index < TransformValues->Num(); ++Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!(*TransformValues)[Index].IsValid() || (*TransformValues)[Index]->Type != EJson::Array
            || !(*TransformValues)[Index]->TryGetArray(Values) || Values->Num() != 10)
        {
            OutError = FString::Printf(TEXT("Transform %d must contain ten numbers."), Index);
            return false;
        }
        double Numbers[10];
        for (int32 NumberIndex = 0; NumberIndex < 10; ++NumberIndex)
        {
            if (!GetNumber((*Values)[NumberIndex], Numbers[NumberIndex]))
            {
                OutError = FString::Printf(
                    TEXT("Transform %d value %d must have numeric JSON type."), Index, NumberIndex);
                return false;
            }
        }
        OutMessage.Transforms.Add({
            FVector(Numbers[0], Numbers[1], Numbers[2]),
            FQuat(Numbers[3], Numbers[4], Numbers[5], Numbers[6]),
            FVector(Numbers[7], Numbers[8], Numbers[9]),
        });
    }

    OutMessage.Curves.Reserve(CurveValues->Num());
    for (int32 Index = 0; Index < CurveValues->Num(); ++Index)
    {
        double Number = 0.0;
        if (!GetNumber((*CurveValues)[Index], Number))
        {
            OutError = FString::Printf(TEXT("Curve %d must have numeric JSON type."), Index);
            return false;
        }
        OutMessage.Curves.Add(Number);
    }
    return true;
}

bool FMtoUProtocol::ValidateFrame(
    const FMtoUFrameMessage& Frame,
    int32 ExpectedTransformCount,
    int32 ExpectedCurveCount,
    FString& OutError,
    bool& bOutStructuralError)
{
    OutError.Reset();
    bOutStructuralError = true;
    if (Frame.Transforms.Num() != ExpectedTransformCount)
    {
        OutError = FString::Printf(
            TEXT("Transform count mismatch: expected %d, got %d."),
            ExpectedTransformCount,
            Frame.Transforms.Num());
        return false;
    }
    if (Frame.Curves.Num() != ExpectedCurveCount)
    {
        OutError = FString::Printf(
            TEXT("Curve count mismatch: expected %d, got %d."),
            ExpectedCurveCount,
            Frame.Curves.Num());
        return false;
    }

    for (int32 Index = 0; Index < Frame.Transforms.Num(); ++Index)
    {
        const FMtoUTransform& Transform = Frame.Transforms[Index];
        const double Values[] = {
            Transform.Translation.X, Transform.Translation.Y, Transform.Translation.Z,
            Transform.Rotation.X, Transform.Rotation.Y, Transform.Rotation.Z, Transform.Rotation.W,
            Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z,
        };
        for (int32 ValueIndex = 0; ValueIndex < UE_ARRAY_COUNT(Values); ++ValueIndex)
        {
            if (!FMath::IsFinite(Values[ValueIndex]))
            {
                bOutStructuralError = false;
                OutError = FString::Printf(
                    TEXT("Non-finite bone %d transform value %d."), Index, ValueIndex);
                return false;
            }
        }
        if (!Transform.Rotation.IsNormalized())
        {
            OutError = FString::Printf(TEXT("Bone %d quaternion must be normalized and non-zero."), Index);
            return false;
        }
    }

    for (int32 Index = 0; Index < Frame.Curves.Num(); ++Index)
    {
        if (!FMath::IsFinite(Frame.Curves[Index]))
        {
            bOutStructuralError = false;
            OutError = FString::Printf(TEXT("Non-finite curve %d."), Index);
            return false;
        }
    }
    bOutStructuralError = false;
    return true;
}

FString FMtoUProtocol::CompareSkeletons(
    const TArray<FMtoUBone>& Maya,
    const TArray<FMtoUBone>& Unreal)
{
    const TMap<FName, FName> MayaParents = ParentMap(Maya);
    const TMap<FName, FName> UnrealParents = ParentMap(Unreal);
    TArray<FString> Missing;
    TArray<FString> Extra;
    TArray<FString> ParentMismatches;

    for (const TPair<FName, FName>& Pair : MayaParents)
    {
        const FName* UnrealParent = UnrealParents.Find(Pair.Key);
        if (!UnrealParent)
        {
            Missing.Add(Pair.Key.ToString());
        }
        else if (*UnrealParent != Pair.Value)
        {
            ParentMismatches.Add(FString::Printf(
                TEXT("%s: Maya=%s, Unreal=%s"),
                *Pair.Key.ToString(),
                *Pair.Value.ToString(),
                *UnrealParent->ToString()));
        }
    }
    for (const TPair<FName, FName>& Pair : UnrealParents)
    {
        if (!MayaParents.Contains(Pair.Key))
        {
            Extra.Add(Pair.Key.ToString());
        }
    }
    if (Missing.IsEmpty() && Extra.IsEmpty() && ParentMismatches.IsEmpty())
    {
        return FString();
    }

    Missing.Sort();
    Extra.Sort();
    ParentMismatches.Sort();
    FString Diagnostic;
    AppendSection(Diagnostic, TEXT("Missing in Unreal"), Missing);
    AppendSection(Diagnostic, TEXT("Extra in Unreal"), Extra);
    AppendSection(Diagnostic, TEXT("Parent mismatches"), ParentMismatches);
    return Diagnostic;
}

TArray<uint8> FMtoUProtocol::EncodeReady(const TArray<FName>& MissingCurves)
{
    TArray<TSharedPtr<FJsonValue>> Curves;
    Curves.Reserve(MissingCurves.Num());
    for (const FName& Name : MissingCurves)
    {
        Curves.Add(MakeShared<FJsonValueString>(Name.ToString()));
    }
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("ready"));
    Object->SetArrayField(TEXT("missing_curves"), Curves);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeError(const FString& Message)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("error"));
    Object->SetStringField(TEXT("message"), Message);
    return EncodeObject(Object);
}

FLiveLinkStaticDataStruct FMtoUProtocol::MakeStaticData(
    const FMtoUInitMessage& Init,
    const TArray<FName>& AcceptedCurveNames)
{
    FLiveLinkStaticDataStruct StaticData(FLiveLinkSkeletonStaticData::StaticStruct());
    FLiveLinkSkeletonStaticData* Skeleton = StaticData.Cast<FLiveLinkSkeletonStaticData>();
    Skeleton->BoneNames.Reserve(Init.Bones.Num());
    Skeleton->BoneParents.Reserve(Init.Bones.Num());
    for (const FMtoUBone& Bone : Init.Bones)
    {
        Skeleton->BoneNames.Add(Bone.Name);
        Skeleton->BoneParents.Add(Bone.ParentIndex);
    }
    Skeleton->PropertyNames = AcceptedCurveNames;
    return StaticData;
}

FLiveLinkFrameDataStruct FMtoUProtocol::MakeFrameData(
    const FMtoUFrameMessage& Frame,
    const TArray<int32>& AcceptedCurveIndices)
{
    FLiveLinkFrameDataStruct FrameData(FLiveLinkAnimationFrameData::StaticStruct());
    FLiveLinkAnimationFrameData* Animation = FrameData.Cast<FLiveLinkAnimationFrameData>();
    Animation->Transforms.Reserve(Frame.Transforms.Num());
    for (const FMtoUTransform& Transform : Frame.Transforms)
    {
        Animation->Transforms.Add(FTransform(Transform.Rotation, Transform.Translation, Transform.Scale));
    }
    Animation->PropertyValues.Reserve(AcceptedCurveIndices.Num());
    for (const int32 Index : AcceptedCurveIndices)
    {
        if (Frame.Curves.IsValidIndex(Index))
        {
            Animation->PropertyValues.Add(static_cast<float>(Frame.Curves[Index]));
        }
    }
    Animation->WorldTime = FLiveLinkWorldTime();
    return FrameData;
}
