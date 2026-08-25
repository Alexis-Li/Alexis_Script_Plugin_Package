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
    FTCHARToUTF8 RoundTrip(Converted.Get(), Converted.Length());
    if (RoundTrip.Length() != Payload.Num()
        || FMemory::Memcmp(RoundTrip.Get(), Payload.GetData(), Payload.Num()) != 0)
    {
        OutError = TEXT("Payload is not valid UTF-8.");
        return false;
    }
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

bool GetExactInt64(const TSharedPtr<FJsonValue>& Value, int64& OutValue)
{
    double Number = 0.0;
    if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
        || !FMath::IsFinite(Number) || Number < MIN_int64 || Number > MAX_int64)
    {
        return false;
    }
    OutValue = static_cast<int64>(Number);
    return static_cast<double>(OutValue) == Number;
}

bool GetNumber(const TSharedPtr<FJsonValue>& Value, double& OutValue)
{
    return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(OutValue);
}

bool ValidateNameText(const FString& Name, const TCHAR* Kind, int32 Index, FString& OutError)
{
    if (Name.IsEmpty())
    {
        OutError = FString::Printf(TEXT("%s %d requires a non-empty string name."), Kind, Index);
        return false;
    }
    int32 NullIndex = INDEX_NONE;
    if (Name.FindChar(TEXT('\0'), NullIndex) && NullIndex < Name.Len())
    {
        OutError = FString::Printf(TEXT("%s %d name contains U+0000."), Kind, Index);
        return false;
    }
    if (Name.Len() >= NAME_SIZE)
    {
        OutError = FString::Printf(
            TEXT("%s %d name length %d exceeds the NAME_SIZE limit of %d characters."),
            Kind,
            Index,
            Name.Len(),
            NAME_SIZE - 1);
        return false;
    }
    return true;
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

bool ParseFrameBody(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* ExpectedType,
    FMtoUFrameMessage& OutMessage,
    FString& OutError)
{
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != ExpectedType)
    {
        OutError = FString::Printf(TEXT("Message type must be '%s'."), ExpectedType);
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
    if (PayloadLength > static_cast<uint64>(MAX_int32) - 8)
    {
        OutError = TEXT("Packet length exceeds Unreal's int32 container range.");
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
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUInitMessage();
    OutError.Reset();
    if (OutErrorCode)
    {
        *OutErrorCode = TEXT("INVALID_MESSAGE");
    }
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
    int32 MessageVersion = 0;
    if (!GetTypedField(Object, TEXT("version"), EJson::Number, VersionValue, OutError)
        || !GetExactInt(VersionValue, MessageVersion))
    {
        OutError = TEXT("Field 'version' must be an integer JSON number.");
        return false;
    }
    if (MessageVersion != FMtoUProtocol::Version)
    {
        OutError = FString::Printf(
            TEXT("Unsupported protocol version; expected %d."), FMtoUProtocol::Version);
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("PROTOCOL_VERSION_MISMATCH");
        }
        return false;
    }

    FString Workflow;
    if (!GetStringField(Object, TEXT("workflow"), Workflow, OutError))
    {
        OutError = TEXT("Field 'workflow' must be a string.");
        return false;
    }
    if (!FMtoUWorkflows::IsValid(Workflow))
    {
        OutError = TEXT("Field 'workflow' must be 'animation' or 'model'.");
        return false;
    }

    // The character snapshot revision is established here during connection
    // negotiation and echoed in the ready outcome.
    TSharedPtr<FJsonValue> RevisionValue;
    int32 NegotiatedRevision = 0;
    if (!GetTypedField(Object, TEXT("revision"), EJson::Number, RevisionValue, OutError)
        || !GetExactInt(RevisionValue, NegotiatedRevision))
    {
        OutError = TEXT("Field 'revision' must be an integer JSON number.");
        return false;
    }
    if (NegotiatedRevision < 0)
    {
        OutError = TEXT("Field 'revision' must not be negative.");
        return false;
    }
    OutMessage.Revision = NegotiatedRevision;

    TSharedPtr<FJsonValue> BlendshapesValue;
    bool bBlendshapesEnabled = false;
    if (!GetTypedField(Object, TEXT("blendshapes_enabled"), EJson::Boolean, BlendshapesValue, OutError)
        || !BlendshapesValue->TryGetBool(bBlendshapesEnabled))
    {
        OutError = TEXT("Field 'blendshapes_enabled' must be a JSON boolean.");
        return false;
    }
    OutMessage.Workflow = Workflow;
    OutMessage.bBlendshapesEnabled = bBlendshapesEnabled;

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

    OutMessage.Bones.Reserve(BoneValues->Num());
    for (int32 Index = 0; Index < BoneValues->Num(); ++Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Record = nullptr;
        if (!(*BoneValues)[Index].IsValid() || (*BoneValues)[Index]->Type != EJson::Array
            || !(*BoneValues)[Index]->TryGetArray(Record) || Record->Num() != 3)
        {
            OutError = FString::Printf(
                TEXT("Bone %d must be [name, parent_index, bind_local_transform]."), Index);
            return false;
        }

        FString NameText;
        int32 ParentIndex = INDEX_NONE;
        if (!(*Record)[0].IsValid() || (*Record)[0]->Type != EJson::String
            || !(*Record)[0]->TryGetString(NameText))
        {
            OutError = FString::Printf(TEXT("Bone %d name must have string JSON type."), Index);
            return false;
        }
        if (!ValidateNameText(NameText, TEXT("Bone"), Index, OutError))
        {
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

        OutMessage.Bones.Add({FName(*NameText), ParentIndex});

        const TArray<TSharedPtr<FJsonValue>>* TransformValues = nullptr;
        if (!(*Record)[2].IsValid() || (*Record)[2]->Type != EJson::Array
            || !(*Record)[2]->TryGetArray(TransformValues) || TransformValues->Num() != 10)
        {
            OutError = FString::Printf(
                TEXT("Bone %d bind local transform must contain ten numbers."), Index);
            return false;
        }
        double Numbers[10];
        for (int32 NumberIndex = 0; NumberIndex < 10; ++NumberIndex)
        {
            if (!GetNumber((*TransformValues)[NumberIndex], Numbers[NumberIndex])
                || !FMath::IsFinite(Numbers[NumberIndex]))
            {
                OutError = FString::Printf(
                    TEXT("Bone %d bind local transform value %d must be finite."),
                    Index,
                    NumberIndex);
                return false;
            }
        }
        const FQuat Rotation(Numbers[3], Numbers[4], Numbers[5], Numbers[6]);
        if (!Rotation.IsNormalized())
        {
            OutError = FString::Printf(
                TEXT("Bone %d bind local quaternion must be normalized and non-zero."), Index);
            return false;
        }
        const FTransform BindTransform(
            Rotation,
            FVector(Numbers[0], Numbers[1], Numbers[2]),
            FVector(Numbers[7], Numbers[8], Numbers[9]));
        if (FMath::IsNearlyZero(BindTransform.ToMatrixWithScale().Determinant()))
        {
            OutError = FString::Printf(
                TEXT("Bone %d bind local transform must be invertible."), Index);
            return false;
        }
        OutMessage.SourceBindLocalPose.Add(BindTransform);
    }

    TSet<FName> CurveNames;
    OutMessage.Curves.Reserve(CurveValues->Num());
    for (int32 Index = 0; Index < CurveValues->Num(); ++Index)
    {
        FString NameText;
        if (!(*CurveValues)[Index].IsValid() || (*CurveValues)[Index]->Type != EJson::String
            || !(*CurveValues)[Index]->TryGetString(NameText))
        {
            OutError = FString::Printf(TEXT("Curve %d name must have string JSON type."), Index);
            return false;
        }
        if (!ValidateNameText(NameText, TEXT("Curve"), Index, OutError))
        {
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
    return ParseFrameBody(Object, TEXT("frame"), OutMessage, OutError);
}

bool FMtoUProtocol::PeekType(
    const TArray<uint8>& Payload,
    FString& OutType,
    FString& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    return GetStringField(Object, TEXT("type"), OutType, OutError);
}

bool FMtoUProtocol::ParseCacheBegin(
    const TArray<uint8>& Payload,
    FMtoUCacheBeginMessage& OutMessage,
    FString& OutError,
    FString& OutErrorCode)
{
    auto Fail = [&](const TCHAR* Code, FString Error)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Error);
        return false;
    };

    OutMessage = FMtoUCacheBeginMessage();
    OutError.Reset();
    OutErrorCode = TEXT("INVALID_MESSAGE");
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }

    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("cache_begin"))
    {
        return Fail(TEXT("INVALID_MESSAGE"), TEXT("Message type must be 'cache_begin'."));
    }

    // Upload identity is message structure: a mistyped or missing field is
    // structural garbage, while a well-typed out-of-range value is a
    // recoverable metadata rejection.
    TSharedPtr<FJsonValue> UploadIdValue;
    if (!GetTypedField(Object, TEXT("upload_id"), EJson::Number, UploadIdValue, OutError)
        || !GetExactInt(UploadIdValue, OutMessage.UploadId))
    {
        return Fail(TEXT("INVALID_MESSAGE"),
                    TEXT("Field 'upload_id' must be an integer JSON number."));
    }
    if (OutMessage.UploadId < 1)
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"),
                    TEXT("Field 'upload_id' must be a positive integer."));
    }

    const auto RequireExactInt32 = [&](const TCHAR* Name, int32& OutValue) -> bool
    {
        TSharedPtr<FJsonValue> Value;
        int32 Parsed = 0;
        if (!GetTypedField(Object, Name, EJson::Number, Value, OutError)
            || !GetExactInt(Value, Parsed))
        {
            OutError = FString::Printf(
                TEXT("Field '%s' must be an integer JSON number."), Name);
            return false;
        }
        OutValue = Parsed;
        return true;
    };
    const auto RequireInt64 = [&](const TCHAR* Name, int64& OutValue) -> bool
    {
        TSharedPtr<FJsonValue> Value;
        int64 Parsed = 0;
        if (!GetTypedField(Object, Name, EJson::Number, Value, OutError)
            || !GetExactInt64(Value, Parsed))
        {
            OutError = FString::Printf(
                TEXT("Field '%s' must be an integer JSON number."), Name);
            return false;
        }
        OutValue = Parsed;
        return true;
    };

    if (!RequireExactInt32(TEXT("revision"), OutMessage.Revision))
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"), OutError);
    }
    TSharedPtr<FJsonValue> FpsValue;
    if (!GetTypedField(Object, TEXT("fps"), EJson::Number, FpsValue, OutError)
        || !FpsValue->TryGetNumber(OutMessage.Fps) || !FMath::IsFinite(OutMessage.Fps))
    {
        return Fail(
            TEXT("CACHE_METADATA_INVALID"),
            TEXT("Field 'fps' must be a finite JSON number."));
    }
    if (OutMessage.Fps < MinCacheFps || OutMessage.Fps > MaxCacheFps)
    {
        return Fail(
            TEXT("CACHE_METADATA_INVALID"),
            FString::Printf(
                TEXT("Field 'fps' must be between %g and %g."),
                MinCacheFps,
                MaxCacheFps));
    }
    if (!RequireExactInt32(TEXT("start_frame"), OutMessage.StartFrame))
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"), OutError);
    }
    if (!RequireExactInt32(TEXT("end_frame"), OutMessage.EndFrame))
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"), OutError);
    }
    if (!RequireExactInt32(TEXT("frame_count"), OutMessage.FrameCount))
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"), OutError);
    }
    if (OutMessage.EndFrame < OutMessage.StartFrame
        || OutMessage.FrameCount != OutMessage.EndFrame - OutMessage.StartFrame + 1)
    {
        return Fail(
            TEXT("CACHE_METADATA_INVALID"),
            TEXT("Field 'frame_count' must cover the inclusive capture range."));
    }
    if (OutMessage.FrameCount < 1 || OutMessage.FrameCount > MaxCacheFrameCount)
    {
        return Fail(
            TEXT("CACHE_PAYLOAD_TOO_LARGE"),
            FString::Printf(
                TEXT("Field 'frame_count' must be between 1 and %d."),
                MaxCacheFrameCount));
    }
    if (!RequireInt64(TEXT("payload_size"), OutMessage.PayloadSize))
    {
        return Fail(TEXT("CACHE_METADATA_INVALID"), OutError);
    }
    if (OutMessage.PayloadSize < 1 || OutMessage.PayloadSize > MaxCachePayloadBytes)
    {
        return Fail(
            TEXT("CACHE_PAYLOAD_TOO_LARGE"),
            FString::Printf(
                TEXT("Declared payload_size %lld exceeds the transient limit of %lld bytes."),
                OutMessage.PayloadSize,
                MaxCachePayloadBytes));
    }
    OutErrorCode = TEXT("");
    return true;
}

bool FMtoUProtocol::ParseCacheFrame(
    const TArray<uint8>& Payload,
    int32& OutIndex,
    FMtoUFrameMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutIndex = INDEX_NONE;
    OutMessage = FMtoUFrameMessage();
    OutError.Reset();
    if (OutErrorCode)
    {
        *OutErrorCode = TEXT("INVALID_MESSAGE");
    }
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }

    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("cache_frame"))
    {
        OutError = TEXT("Message type must be 'cache_frame'.");
        return false;
    }

    TSharedPtr<FJsonValue> IndexValue;
    if (!GetTypedField(Object, TEXT("index"), EJson::Number, IndexValue, OutError)
        || !GetExactInt(IndexValue, OutIndex))
    {
        OutError = TEXT("Field 'index' must be an integer JSON number.");
        return false;
    }
    if (OutIndex < 0)
    {
        OutError = TEXT("Field 'index' must not be negative.");
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("CACHE_FRAME_INDEX_INVALID");
        }
        return false;
    }
    if (!ParseFrameBody(Object, TEXT("cache_frame"), OutMessage, OutError))
    {
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseCacheEnd(const TArray<uint8>& Payload, FString& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("cache_end"))
    {
        OutError = TEXT("Message type must be 'cache_end'.");
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseCachePlay(
    const TArray<uint8>& Payload,
    int32& OutPlayId,
    FString& OutError)
{
    OutPlayId = INDEX_NONE;
    OutError.Reset();
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("cache_play"))
    {
        OutError = TEXT("Message type must be 'cache_play'.");
        return false;
    }
    TSharedPtr<FJsonValue> PlayIdValue;
    if (!GetTypedField(Object, TEXT("play_id"), EJson::Number, PlayIdValue, OutError)
        || !GetExactInt(PlayIdValue, OutPlayId))
    {
        OutError = TEXT("Field 'play_id' must be an integer JSON number.");
        return false;
    }
    return true;
}

bool ParseCacheTypeOnly(const TArray<uint8>& Payload, const TCHAR* ExpectedType, FString& OutError)
{
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != ExpectedType)
    {
        OutError = FString::Printf(TEXT("Message type must be '%s'."), ExpectedType);
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseCacheEnter(const TArray<uint8>& Payload, FString& OutError)
{
    return ParseCacheTypeOnly(Payload, TEXT("cache_enter"), OutError);
}

bool FMtoUProtocol::ParseCacheStop(const TArray<uint8>& Payload, FString& OutError)
{
    return ParseCacheTypeOnly(Payload, TEXT("cache_stop"), OutError);
}

bool FMtoUProtocol::ParseCacheClear(const TArray<uint8>& Payload, FString& OutError)
{
    return ParseCacheTypeOnly(Payload, TEXT("cache_clear"), OutError);
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
        if (FMath::Abs(Frame.Curves[Index]) > TNumericLimits<float>::Max())
        {
            bOutStructuralError = false;
            OutError = FString::Printf(
                TEXT("Curve %d cannot be represented as a finite Live Link float."), Index);
            return false;
        }
    }
    bOutStructuralError = false;
    return true;
}

TArray<uint8> FMtoUProtocol::EncodeReady(
    const TArray<FName>& MissingInUnreal,
    const TArray<FName>& MissingInMaya,
    const TArray<FString>& BoneNameRemaps,
    const FString& Workflow,
    int32 TargetMorphCount,
    int32 AcceptedMorphCount,
    int32 NegotiatedRevision)
{
    auto EncodeNames = [](const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Names.Num());
        for (const FName& Name : Names)
        {
            Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        return Values;
    };
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("ready"));
    Object->SetNumberField(TEXT("revision"), NegotiatedRevision);
    Object->SetArrayField(TEXT("missing_in_unreal"), EncodeNames(MissingInUnreal));
    Object->SetArrayField(TEXT("missing_in_maya"), EncodeNames(MissingInMaya));
    TArray<TSharedPtr<FJsonValue>> RemapValues;
    RemapValues.Reserve(BoneNameRemaps.Num());
    for (const FString& Remap : BoneNameRemaps)
    {
        RemapValues.Add(MakeShared<FJsonValueString>(Remap));
    }
    Object->SetArrayField(TEXT("bone_name_remaps"), RemapValues);
    Object->SetStringField(TEXT("workflow"), Workflow);
    Object->SetNumberField(TEXT("target_morph_count"), TargetMorphCount);
    Object->SetNumberField(TEXT("accepted_morph_count"), AcceptedMorphCount);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheReady(int32 UploadId, int32 NegotiatedRevision, int32 FrameCount)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_ready"));
    Object->SetNumberField(TEXT("upload_id"), UploadId);
    Object->SetNumberField(TEXT("revision"), NegotiatedRevision);
    Object->SetNumberField(TEXT("frame_count"), FrameCount);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheProgress(int32 PlayId, int32 AppliedFrames)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_progress"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("applied"), AppliedFrames);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheComplete(
    int32 PlayId, int32 AppliedFrameCount, double ElapsedSeconds)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_complete"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("applied_frame_count"), AppliedFrameCount);
    Object->SetNumberField(TEXT("elapsed_seconds"), ElapsedSeconds);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheStopped(int32 PlayId)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_stopped"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheCleared()
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_cleared"));
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeError(
    const FString& Code,
    const FString& Message,
    const FString& Details,
    int32 UploadId,
    int32 PlayId)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("error"));
    Object->SetStringField(TEXT("code"), Code);
    Object->SetStringField(TEXT("message"), Message);
    Object->SetStringField(TEXT("details"), Details.IsEmpty() ? Message : Details);
    // Cache-operation errors echo the owning identity so Maya can discard
    // late errors from older uploads or play attempts.
    if (UploadId > 0)
    {
        Object->SetNumberField(TEXT("upload_id"), UploadId);
    }
    if (PlayId > 0)
    {
        Object->SetNumberField(TEXT("play_id"), PlayId);
    }
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
    for (const FMtoUDescriptionBone& Bone : Init.Bones)
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

FLiveLinkFrameDataStruct FMtoUProtocol::MakeRetargetedFrameData(
    const FMtoUFrameMessage& Frame,
    const TArray<int32>& AcceptedCurveIndices,
    const TArray<FTransform>& SourceBindLocalPose,
    const TArray<FTransform>& TargetRefLocalPose,
    const TArray<int32>& BoneParents)
{
    const int32 BoneCount = Frame.Transforms.Num();
    check(SourceBindLocalPose.Num() == BoneCount);
    check(TargetRefLocalPose.Num() == BoneCount);
    check(BoneParents.Num() == BoneCount);

    TArray<FMatrix> SourceBindComponentPose;
    TArray<FMatrix> SourceCurrentComponentPose;
    TArray<FMatrix> TargetRefComponentPose;
    TArray<FMatrix> TargetCurrentComponentPose;
    SourceBindComponentPose.SetNumUninitialized(BoneCount);
    SourceCurrentComponentPose.SetNumUninitialized(BoneCount);
    TargetRefComponentPose.SetNumUninitialized(BoneCount);
    TargetCurrentComponentPose.SetNumUninitialized(BoneCount);

    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = BoneParents[Index];
        check(ParentIndex == INDEX_NONE || ParentIndex < Index);
        const FMtoUTransform& SourceCurrent = Frame.Transforms[Index];
        const FMatrix SourceCurrentLocal = FTransform(
            SourceCurrent.Rotation,
            SourceCurrent.Translation,
            SourceCurrent.Scale).ToMatrixWithScale();
        const FMatrix SourceBindLocal = SourceBindLocalPose[Index].ToMatrixWithScale();
        const FMatrix TargetRefLocal = TargetRefLocalPose[Index].ToMatrixWithScale();

        if (ParentIndex == INDEX_NONE)
        {
            SourceCurrentComponentPose[Index] = SourceCurrentLocal;
            SourceBindComponentPose[Index] = SourceBindLocal;
            TargetRefComponentPose[Index] = TargetRefLocal;
        }
        else
        {
            SourceCurrentComponentPose[Index] =
                SourceCurrentLocal * SourceCurrentComponentPose[ParentIndex];
            SourceBindComponentPose[Index] =
                SourceBindLocal * SourceBindComponentPose[ParentIndex];
            TargetRefComponentPose[Index] =
                TargetRefLocal * TargetRefComponentPose[ParentIndex];
        }

        // Unreal matrices use row-vector composition. This maps the saved source
        // bind component transform onto the target reference component transform,
        // then applies the evaluated source component motion.
        TargetCurrentComponentPose[Index] = TargetRefComponentPose[Index]
            * SourceBindComponentPose[Index].Inverse()
            * SourceCurrentComponentPose[Index];
    }

    FMtoUFrameMessage RetargetedFrame;
    RetargetedFrame.Transforms.Reserve(BoneCount);
    RetargetedFrame.Curves = Frame.Curves;
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = BoneParents[Index];
        const FMatrix TargetCurrentLocal = ParentIndex == INDEX_NONE
            ? TargetCurrentComponentPose[Index]
            : TargetCurrentComponentPose[Index]
                * TargetCurrentComponentPose[ParentIndex].Inverse();
        FTransform Transform(TargetCurrentLocal);
        Transform.NormalizeRotation();
        RetargetedFrame.Transforms.Add({
            Transform.GetTranslation(), Transform.GetRotation(), Transform.GetScale3D()});
    }
    return MakeFrameData(RetargetedFrame, AcceptedCurveIndices);
}
