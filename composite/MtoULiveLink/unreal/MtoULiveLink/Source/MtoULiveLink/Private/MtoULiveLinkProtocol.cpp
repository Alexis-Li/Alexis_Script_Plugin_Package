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
// Absolute determinant tolerances reject valid small-scale rigs (for example
// pupil joints at 1e-12). Use the engine's checked inverse, bypassing Inverse()'s
// nearly-zero-axis identity fallback, and reject non-finite results explicitly.
bool TryInverse(const FMatrix& Matrix, FMatrix& Inverse)
{
    return !Matrix.ContainsNaN() && VectorMatrixInverse(&Inverse, &Matrix)
        && !Inverse.ContainsNaN();
}

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
        || !FMath::IsFinite(Number))
    {
        return false;
    }
    // Keep the comparison inside the exactly representable int64 window:
    // 2^63 is representable as a double but not as an int64, so casting a
    // value equal to MAX_int64 rounded-up would be undefined conversion.
    constexpr double MaxInt64Exclusive = 9223372036854775808.0;
    if (!(Number >= -MaxInt64Exclusive && Number < MaxInt64Exclusive))
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

// v10 identity helpers. A linkage message that cannot name its identity is
// structurally invalid, because no later message could be matched to it.
bool GetIdentityInt(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    int32& OutValue,
    FString& OutError)
{
    TSharedPtr<FJsonValue> Value;
    if (!GetTypedField(Object, Name, EJson::Number, Value, OutError)
        || !GetExactInt(Value, OutValue))
    {
        OutError = FString::Printf(
            TEXT("Field '%s' must be an integer JSON number."), Name);
        return false;
    }
    return true;
}

bool GetOptionalIdentityInt(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    int32& OutValue,
    FString& OutError)
{
    if (!Object->HasField(Name))
    {
        return true;
    }
    return GetIdentityInt(Object, Name, OutValue, OutError);
}

bool GetOptionalObject(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Name,
    TSharedPtr<FJsonObject>& OutValue,
    FString& OutError)
{
    OutValue.Reset();
    if (!Object->HasField(Name))
    {
        return true;
    }
    TSharedPtr<FJsonValue> Value;
    if (!GetTypedField(Object, Name, EJson::Object, Value, OutError))
    {
        return false;
    }
    OutValue = Value->AsObject();
    return OutValue.IsValid();
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
    int32 ExpectedTransformCount,
    int32 ExpectedCurveCount,
    FMtoUFrameMessage& OutMessage,
    FString& OutError,
    bool* bOutCountMismatch = nullptr)
{
    if (bOutCountMismatch)
    {
        *bOutCountMismatch = false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != ExpectedType)
    {
        OutError = FString::Printf(TEXT("Message type must be '%s'."), ExpectedType);
        return false;
    }

    // The frozen contract refuses a message whose version differs before any
    // state changes, for every message family and not only the greeting and
    // the object description, so a mixed pair never half-streams.
    double VersionValue = 0.0;
    if (Object->TryGetNumberField(TEXT("version"), VersionValue)
        && static_cast<int32>(VersionValue) != FMtoUProtocol::Version)
    {
        OutError = FString::Printf(
            TEXT("Unsupported protocol version; expected %d."),
            FMtoUProtocol::Version);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* TransformValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* CurveValues = nullptr;
    if (!GetArrayField(Object, TEXT("transforms"), TransformValues, OutError)
        || !GetArrayField(Object, TEXT("curves"), CurveValues, OutError))
    {
        return false;
    }

    // Cached frames use the negotiated cardinalities before Reserve or any
    // numeric conversion, so surplus localhost input never gains a parsed
    // transform/curve allocation. Real-time frames retain their existing
    // post-parse validation by passing INDEX_NONE.
    if (ExpectedTransformCount != INDEX_NONE
        && TransformValues->Num() != ExpectedTransformCount)
    {
        OutError = FString::Printf(
            TEXT("Transform count mismatch: expected %d, got %d."),
            ExpectedTransformCount,
            TransformValues->Num());
        if (bOutCountMismatch)
        {
            *bOutCountMismatch = true;
        }
        return false;
    }
    if (ExpectedCurveCount != INDEX_NONE && CurveValues->Num() != ExpectedCurveCount)
    {
        OutError = FString::Printf(
            TEXT("Curve count mismatch: expected %d, got %d."),
            ExpectedCurveCount,
            CurveValues->Num());
        if (bOutCountMismatch)
        {
            *bOutCountMismatch = true;
        }
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
    // Enforce the frozen message ceiling (protocol corpus limits.max_message_bytes)
    // at the length header, before any payload accumulation or int32 cast.
    if (PayloadLength > static_cast<uint64>(FMtoUProtocol::MaxMessageBytes))
    {
        OutError = TEXT("Packet length exceeds the maximum message length.");
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

    // v10 identity: the explicitly paired object, its set revision and the
    // common space both adapters agreed on. A message that cannot name them
    // is structurally invalid, because a v10 group or mode message that later
    // answers this negotiation could not be matched to it.
    if (!GetStringField(Object, TEXT("object_id"), OutMessage.ObjectId, OutError)
        || OutMessage.ObjectId.IsEmpty())
    {
        OutError = TEXT("Field 'object_id' must be a non-empty string.");
        return false;
    }
    const auto RequirePositiveRevision = [&](const TCHAR* Name, int32& OutValue) -> bool
    {
        TSharedPtr<FJsonValue> Value;
        int32 Parsed = 0;
        if (!GetTypedField(Object, Name, EJson::Number, Value, OutError)
            || !GetExactInt(Value, Parsed)
            || Parsed < 1)
        {
            OutError = FString::Printf(
                TEXT("Field '%s' must be a positive integer JSON number."), Name);
            return false;
        }
        OutValue = Parsed;
        return true;
    };
    if (!RequirePositiveRevision(TEXT("set_revision"), OutMessage.SetRevision)
        || !RequirePositiveRevision(TEXT("space_revision"), OutMessage.SpaceRevision))
    {
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
        FMatrix BindInverse;
        if (!TryInverse(BindTransform.ToMatrixWithScale(), BindInverse))
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
    return ParseFrameBody(
        Object, TEXT("frame"), INDEX_NONE, INDEX_NONE, OutMessage, OutError);
}

bool FMtoUProtocol::ParseHello(
    const TArray<uint8>& Payload,
    FMtoUHelloMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUHelloMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("hello"))
    {
        OutError = TEXT("Message type must be 'hello'.");
        return false;
    }

    // The greeting is answered before anything else is discovered, so a peer
    // with a different protocol version never borrows a camera, mutes a
    // writer or reads a handoff directory.
    if (!GetIdentityInt(Object, TEXT("version"), OutMessage.Version, OutError))
    {
        OutError = TEXT("Field 'version' must be an integer JSON number.");
        return false;
    }
    if (OutMessage.Version != FMtoUProtocol::Version)
    {
        OutError = FString::Printf(
            TEXT("Unsupported protocol version; expected %d."), FMtoUProtocol::Version);
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("PROTOCOL_VERSION_MISMATCH");
        }
        return false;
    }

    if (!GetStringField(Object, TEXT("host"), OutMessage.Host, OutError)
        || !GetStringField(Object, TEXT("maya_version"), OutMessage.MayaVersion, OutError))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Scene;
    if (!GetOptionalObject(Object, TEXT("scene"), Scene, OutError) || !Scene.IsValid())
    {
        OutError = TEXT("Field 'scene' must be an object.");
        return false;
    }
    if (!GetStringField(Scene, TEXT("linear_unit"), OutMessage.LinearUnit, OutError))
    {
        return false;
    }
    TSharedPtr<FJsonValue> ScaleValue;
    if (!GetTypedField(Scene, TEXT("unit_scale_cm"), EJson::Number, ScaleValue, OutError)
        || !ScaleValue->TryGetNumber(OutMessage.UnitScaleCm)
        || !FMath::IsFinite(OutMessage.UnitScaleCm)
        || OutMessage.UnitScaleCm <= 0.0)
    {
        OutError = TEXT("Field 'scene.unit_scale_cm' must be a positive finite number.");
        return false;
    }
    TSharedPtr<FJsonValue> FpsValue;
    if (!GetTypedField(Scene, TEXT("fps"), EJson::Number, FpsValue, OutError)
        || !FpsValue->TryGetNumber(OutMessage.Fps)
        || !FMath::IsFinite(OutMessage.Fps)
        || OutMessage.Fps <= 0.0)
    {
        OutError = TEXT("Field 'scene.fps' must be a positive finite number.");
        return false;
    }

    TSharedPtr<FJsonObject> Capabilities;
    if (!GetOptionalObject(Object, TEXT("capabilities"), Capabilities, OutError)
        || !Capabilities.IsValid())
    {
        OutError = TEXT("Field 'capabilities' must be an object.");
        return false;
    }
    const auto ReadCapability = [&](const TCHAR* Name, bool& bOutValue) -> bool
    {
        if (!Capabilities->HasField(Name))
        {
            return true;
        }
        TSharedPtr<FJsonValue> Value;
        if (!GetTypedField(Capabilities, Name, EJson::Boolean, Value, OutError)
            || !Value->TryGetBool(bOutValue))
        {
            OutError = FString::Printf(
                TEXT("Field 'capabilities.%s' must be a JSON boolean."), Name);
            return false;
        }
        return true;
    };
    if (!ReadCapability(TEXT("linkage"), OutMessage.bLinkage)
        || !ReadCapability(TEXT("reference"), OutMessage.bReference)
        || !ReadCapability(TEXT("cache"), OutMessage.bCache)
        || !ReadCapability(TEXT("model"), OutMessage.bModel))
    {
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseMode(
    const TArray<uint8>& Payload,
    FMtoUModeMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUModeMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("mode"))
    {
        OutError = TEXT("Message type must be 'mode'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError)
        || !GetIdentityInt(Object, TEXT("mode_revision"), OutMessage.ModeRevision, OutError)
        || !GetStringField(Object, TEXT("mode"), OutMessage.Mode, OutError)
        || !GetStringField(Object, TEXT("control"), OutMessage.Control, OutError)
        || !GetStringField(Object, TEXT("transition"), OutMessage.Transition, OutError))
    {
        return false;
    }
    if (OutMessage.SessionId < 1 || OutMessage.ModeRevision < 1)
    {
        OutError = TEXT("Fields 'session_id' and 'mode_revision' must be positive integers.");
        return false;
    }
    if (OutMessage.Mode != TEXT("realtime") && OutMessage.Mode != TEXT("cached")
        && OutMessage.Mode != TEXT("linkage"))
    {
        OutError = TEXT("Field 'mode' must be 'realtime', 'cached' or 'linkage'.");
        return false;
    }
    if (OutMessage.Control != TEXT("maya") && OutMessage.Control != TEXT("unreal"))
    {
        OutError = TEXT("Field 'control' must be 'maya' or 'unreal'.");
        return false;
    }
    if (OutMessage.Transition != TEXT("ready") && OutMessage.Transition != TEXT("failed"))
    {
        OutError = TEXT("Field 'transition' must be 'ready' or 'failed'.");
        return false;
    }
    // detail is diagnostic text; an absent one names no failure, but a
    // transition that failed must name its reason.
    if (Object->HasField(TEXT("detail")))
    {
        if (!GetStringField(Object, TEXT("detail"), OutMessage.Detail, OutError))
        {
            return false;
        }
    }
    if (OutMessage.Transition == TEXT("failed") && OutMessage.Detail.IsEmpty())
    {
        OutError = TEXT("Field 'detail' must name the reason a transition failed.");
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseLeaveLinkage(
    const TArray<uint8>& Payload,
    FMtoULeaveLinkageMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoULeaveLinkageMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("leave_linkage"))
    {
        OutError = TEXT("Message type must be 'leave_linkage'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError)
        || !GetIdentityInt(Object, TEXT("mode_revision"), OutMessage.ModeRevision, OutError)
        || !GetStringField(Object, TEXT("target"), OutMessage.Target, OutError))
    {
        return false;
    }
    if (OutMessage.SessionId < 1 || OutMessage.ModeRevision < 1)
    {
        OutError = TEXT("Fields 'session_id' and 'mode_revision' must be positive integers.");
        return false;
    }
    if (OutMessage.Target != TEXT("realtime") && OutMessage.Target != TEXT("end"))
    {
        OutError = TEXT("Field 'target' must be 'realtime' or 'end'.");
        return false;
    }
    if (Object->HasField(TEXT("detail"))
        && !GetStringField(Object, TEXT("detail"), OutMessage.Detail, OutError))
    {
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseGroup(
    const TArray<uint8>& Payload,
    int64 EncodedBytes,
    FMtoUPoseGroupMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUPoseGroupMessage();
    OutError.Reset();
    if (OutErrorCode)
    {
        *OutErrorCode = TEXT("INVALID_MESSAGE");
    }
    // The group budget meters the complete message, not one object at a time,
    // and is checked before the group gains any staged ownership.
    if (EncodedBytes > MaxLinkageGroupBytes)
    {
        OutError = FString::Printf(
            TEXT("Linkage group of %lld bytes exceeds the frozen %lld byte budget."),
            EncodedBytes,
            MaxLinkageGroupBytes);
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("RESOURCE_LIMIT");
        }
        return false;
    }
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("group"))
    {
        OutError = TEXT("Message type must be 'group'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError)
        || !GetIdentityInt(Object, TEXT("set_revision"), OutMessage.SetRevision, OutError)
        || !GetIdentityInt(Object, TEXT("mode_revision"), OutMessage.ModeRevision, OutError)
        || !GetIdentityInt(Object, TEXT("space_revision"), OutMessage.SpaceRevision, OutError)
        || !GetStringField(Object, TEXT("eval_id"), OutMessage.EvalId, OutError)
        || !GetIdentityInt(Object, TEXT("transport_serial"), OutMessage.TransportSerial, OutError)
        || !GetIdentityInt(Object, TEXT("sample_serial"), OutMessage.SampleSerial, OutError))
    {
        return false;
    }
    // The first revision of every identity is 1, so a value below it can never
    // name the current session, set, space or mode. The refusal keeps the
    // frozen code instead of collapsing into a shape error, because the shape
    // is fine and only the named identity is stale.
    if (OutMessage.SessionId < 1)
    {
        OutError = TEXT("A group must name a positive session id.");
        return false;
    }
    if (OutMessage.SetRevision < 1)
    {
        OutError = TEXT("A group named an older object set: the set revision is below the first set.");
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("SET_REVISION_STALE");
        }
        return false;
    }
    if (OutMessage.SpaceRevision < 1)
    {
        OutError = TEXT("A group named an older common space: the space revision is below the first space.");
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("SPACE_REVISION_STALE");
        }
        return false;
    }
    if (OutMessage.ModeRevision < 1)
    {
        OutError = TEXT("A group named an older mode: the mode revision is below the first mode.");
        if (OutErrorCode)
        {
            *OutErrorCode = TEXT("MODE_CONFLICT");
        }
        return false;
    }
    if (OutMessage.EvalId.IsEmpty() || OutMessage.TransportSerial < 0
        || OutMessage.SampleSerial < 0)
    {
        OutError = TEXT("Field 'eval_id' must be non-empty and the serials must not be negative.");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* ObjectValues = nullptr;
    if (!GetArrayField(Object, TEXT("objects"), ObjectValues, OutError)
        || ObjectValues->IsEmpty())
    {
        OutError = TEXT("Field 'objects' must be a non-empty array.");
        return false;
    }
    OutMessage.Objects.Reserve(ObjectValues->Num());
    for (int32 Index = 0; Index < ObjectValues->Num(); ++Index)
    {
        FMtoUPoseGroup Row;
        const TSharedPtr<FJsonObject> RowObject = (*ObjectValues)[Index]->AsObject();
        if (!RowObject.IsValid())
        {
            OutError = FString::Printf(TEXT("Group object %d must be an object."), Index);
            return false;
        }
        if (!GetStringField(RowObject, TEXT("object_id"), Row.ObjectId, OutError)
            || Row.ObjectId.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Group object %d requires a non-empty 'object_id'."), Index);
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* TransformValues = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* CurveValues = nullptr;
        if (!GetArrayField(RowObject, TEXT("transforms"), TransformValues, OutError)
            || !GetArrayField(RowObject, TEXT("curves"), CurveValues, OutError))
        {
            return false;
        }
        Row.Transforms.Reserve(TransformValues->Num());
        for (int32 TransformIndex = 0; TransformIndex < TransformValues->Num(); ++TransformIndex)
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!(*TransformValues)[TransformIndex].IsValid()
                || (*TransformValues)[TransformIndex]->Type != EJson::Array
                || !(*TransformValues)[TransformIndex]->TryGetArray(Values)
                || Values->Num() != 10)
            {
                OutError = FString::Printf(
                    TEXT("Group object %d transform %d must contain ten numbers."),
                    Index,
                    TransformIndex);
                return false;
            }
            double Numbers[10];
            for (int32 NumberIndex = 0; NumberIndex < 10; ++NumberIndex)
            {
                if (!GetNumber((*Values)[NumberIndex], Numbers[NumberIndex])
                    || !FMath::IsFinite(Numbers[NumberIndex]))
                {
                    OutError = FString::Printf(
                        TEXT("Group object %d transform %d value %d must be finite."),
                        Index,
                        TransformIndex,
                        NumberIndex);
                    return false;
                }
            }
            const FQuat Rotation(Numbers[3], Numbers[4], Numbers[5], Numbers[6]);
            if (!Rotation.IsNormalized())
            {
                OutError = FString::Printf(
                    TEXT("Group object %d transform %d quaternion must be normalized."),
                    Index,
                    TransformIndex);
                return false;
            }
            Row.Transforms.Add(FTransform(
                Rotation,
                FVector(Numbers[0], Numbers[1], Numbers[2]),
                FVector(Numbers[7], Numbers[8], Numbers[9])));
        }
        Row.Curves.Reserve(CurveValues->Num());
        for (int32 CurveIndex = 0; CurveIndex < CurveValues->Num(); ++CurveIndex)
        {
            double Number = 0.0;
            if (!GetNumber((*CurveValues)[CurveIndex], Number) || !FMath::IsFinite(Number))
            {
                OutError = FString::Printf(
                    TEXT("Group object %d curve %d must be a finite number."),
                    Index,
                    CurveIndex);
                return false;
            }
            Row.Curves.Add(Number);
        }
        OutMessage.Objects.Add(MoveTemp(Row));
    }

    if (!GetOptionalObject(Object, TEXT("camera"), OutMessage.Camera, OutError)
        || !OutMessage.Camera.IsValid())
    {
        OutError = TEXT("Field 'camera' must be an object.");
        return false;
    }
    if (!GetOptionalObject(Object, TEXT("time"), OutMessage.Time, OutError)
        || !OutMessage.Time.IsValid())
    {
        OutError = TEXT("Field 'time' must be an object.");
        return false;
    }
    TSharedPtr<FJsonValue> FrameValue;
    if (!GetTypedField(OutMessage.Time, TEXT("ue_display_frame"), EJson::Number, FrameValue, OutError)
        || !FrameValue->TryGetNumber(OutMessage.UeDisplayFrame)
        || !FMath::IsFinite(OutMessage.UeDisplayFrame))
    {
        OutError = TEXT("Field 'time.ue_display_frame' must be a finite number.");
        return false;
    }
    TSharedPtr<FJsonValue> MayaFrameValue;
    if (OutMessage.Time->HasField(TEXT("maya_frame")))
    {
        if (!GetTypedField(OutMessage.Time, TEXT("maya_frame"), EJson::Number, MayaFrameValue, OutError)
            || !MayaFrameValue->TryGetNumber(OutMessage.MayaFrame)
            || !FMath::IsFinite(OutMessage.MayaFrame))
        {
            OutError = TEXT("Field 'time.maya_frame' must be a finite number.");
            return false;
        }
    }
    else
    {
        OutMessage.MayaFrame = OutMessage.UeDisplayFrame;
    }
    // Every row of the group reports the same applied time, because the whole
    // group answers one target.
    for (FMtoUPoseGroup& Row : OutMessage.Objects)
    {
        Row.UeDisplayFrame = OutMessage.UeDisplayFrame;
        Row.MayaFrame = OutMessage.MayaFrame;
    }
    return true;
}

bool FMtoUProtocol::ParseReferenceResult(
    const TArray<uint8>& Payload,
    FMtoUReferenceResultMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUReferenceResultMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError)
        || Type != TEXT("reference_result"))
    {
        OutError = TEXT("Message type must be 'reference_result'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError)
        || !GetStringField(Object, TEXT("transfer_id"), OutMessage.TransferId, OutError)
        || !GetStringField(Object, TEXT("detail"), OutMessage.Detail, OutError))
    {
        return false;
    }
    if (Object->HasField(TEXT("code"))
        && !GetStringField(Object, TEXT("code"), OutMessage.Code, OutError))
    {
        return false;
    }
    if (OutMessage.SessionId < 1 || OutMessage.TransferId.IsEmpty())
    {
        OutError = TEXT("Fields 'session_id' must be positive and 'transfer_id' non-empty.");
        return false;
    }
    TSharedPtr<FJsonValue> AcceptedValue;
    if (!GetTypedField(Object, TEXT("accepted"), EJson::Boolean, AcceptedValue, OutError)
        || !AcceptedValue->TryGetBool(OutMessage.bAccepted))
    {
        OutError = TEXT("Field 'accepted' must be a JSON boolean.");
        return false;
    }
    TSharedPtr<FJsonObject> Counts;
    if (Object->HasField(TEXT("counts"))
        && (!GetOptionalObject(Object, TEXT("counts"), Counts, OutError) || !Counts.IsValid()))
    {
        OutError = TEXT("Field 'counts' must be an object.");
        return false;
    }
    if (Counts.IsValid())
    {
        const auto ReadCount = [&](const TCHAR* Name, int64& OutValue) -> bool
        {
            if (!Counts->HasField(Name))
            {
                return true;
            }
            TSharedPtr<FJsonValue> Value;
            int64 Parsed = 0;
            if (!GetTypedField(Counts, Name, EJson::Number, Value, OutError)
                || !GetExactInt64(Value, Parsed)
                || Parsed < 0)
            {
                OutError = FString::Printf(
                    TEXT("Field 'counts.%s' must be a non-negative integer."), Name);
                return false;
            }
            OutValue = Parsed;
            return true;
        };
        if (!ReadCount(TEXT("objects"), OutMessage.ObjectCount)
            || !ReadCount(TEXT("triangles"), OutMessage.TriangleCount)
            || !ReadCount(TEXT("bytes"), OutMessage.ByteCount))
        {
            return false;
        }
    }
    if (Object->HasField(TEXT("container"))
        && !GetStringField(Object, TEXT("container"), OutMessage.Container, OutError))
    {
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseReferenceRemoved(
    const TArray<uint8>& Payload,
    FMtoUReferenceRemovedMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUReferenceRemovedMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError)
        || Type != TEXT("reference_removed"))
    {
        OutError = TEXT("Message type must be 'reference_removed'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError))
    {
        return false;
    }
    if (OutMessage.SessionId < 1)
    {
        OutError = TEXT("Field 'session_id' must be a positive integer.");
        return false;
    }
    if (Object->HasField(TEXT("detail"))
        && !GetStringField(Object, TEXT("detail"), OutMessage.Detail, OutError))
    {
        return false;
    }
    return true;
}

bool FMtoUProtocol::ParseReferenceQuery(
    const TArray<uint8>& Payload,
    FMtoUReferenceQueryMessage& OutMessage,
    FString& OutError,
    FString* OutErrorCode)
{
    OutMessage = FMtoUReferenceQueryMessage();
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
    if (!GetStringField(Object, TEXT("type"), Type, OutError)
        || Type != TEXT("reference_query"))
    {
        OutError = TEXT("Message type must be 'reference_query'.");
        return false;
    }
    if (!GetIdentityInt(Object, TEXT("session_id"), OutMessage.SessionId, OutError)
        || !GetStringField(Object, TEXT("transfer_id"), OutMessage.TransferId, OutError))
    {
        return false;
    }
    if (OutMessage.SessionId < 1 || OutMessage.TransferId.IsEmpty())
    {
        OutError = TEXT("Fields 'session_id' must be positive and 'transfer_id' non-empty.");
        return false;
    }
    TSharedPtr<FJsonObject> Scope;
    if (!GetOptionalObject(Object, TEXT("scope"), Scope, OutError) || !Scope.IsValid())
    {
        OutError = TEXT("Field 'scope' must be an object.");
        return false;
    }
    FString Level;
    if (!GetStringField(Scope, TEXT("level"), Level, OutError) || Level.IsEmpty())
    {
        OutError = TEXT("Field 'scope.level' must be a non-empty string.");
        return false;
    }
    OutMessage.Scope = Scope;
    return true;
}

bool FMtoUProtocol::PeekType(
    const TArray<uint8>& Payload,
    FString& OutType,
    FString& OutError)
{
    OutType.Reset();
    OutError.Reset();
    if (Payload.IsEmpty())
    {
        OutError = TEXT("Empty JSON payload.");
        return false;
    }

    // Route before cache-frame admission with a streaming token scan. This
    // does not build a JSON DOM; the selected message parser performs the one
    // full deserialization only after its resource gate accepts the payload.
    const FAnsiStringView Text(
        reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
    const TSharedRef<TJsonReader<ANSICHAR>> Reader =
        TJsonReaderFactory<ANSICHAR>::CreateFromView(Text);
    EJsonNotation Notation = EJsonNotation::Null;
    if (!Reader->ReadNext(Notation) || Notation != EJsonNotation::ObjectStart)
    {
        OutError = TEXT("Payload must be a JSON object.");
        return false;
    }
    while (Reader->ReadNext(Notation))
    {
        if (Notation == EJsonNotation::Error)
        {
            OutError = Reader->GetErrorMessage();
            return false;
        }
        if (Reader->GetIdentifier() == TEXT("type"))
        {
            if (Notation != EJsonNotation::String)
            {
                OutError = TEXT("Field 'type' has the wrong JSON type.");
                return false;
            }
            OutType = Reader->GetValueAsString();
            return true;
        }
        if (Notation == EJsonNotation::ObjectStart && !Reader->SkipObject())
        {
            OutError = Reader->GetErrorMessage();
            return false;
        }
        if (Notation == EJsonNotation::ArrayStart && !Reader->SkipArray())
        {
            OutError = Reader->GetErrorMessage();
            return false;
        }
        if (Notation == EJsonNotation::ObjectEnd)
        {
            break;
        }
    }
    OutError = TEXT("Field 'type' has the wrong JSON type.");
    return false;
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

    // A version that differs is refused with its own code before any upload
    // state exists, so a mixed pair never starts a cache transfer.
    double VersionValue = 0.0;
    if (Object->TryGetNumberField(TEXT("version"), VersionValue)
        && static_cast<int32>(VersionValue) != FMtoUProtocol::Version)
    {
        return Fail(TEXT("PROTOCOL_VERSION_MISMATCH"),
                    FString::Printf(TEXT("Unsupported protocol version; expected %d."),
                                    FMtoUProtocol::Version));
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
    // Overflow-safe range validation before any allocation: compute the span
    // in int64 so extreme start/end frames can never wrap in int32.
    const int64 FrameSpan = static_cast<int64>(OutMessage.EndFrame)
        - static_cast<int64>(OutMessage.StartFrame) + 1;
    if (FrameSpan < 1
        || FrameSpan != static_cast<int64>(OutMessage.FrameCount))
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
    int32 ExpectedTransformCount,
    int32 ExpectedCurveCount,
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
    bool bCountMismatch = false;
    if (!ParseFrameBody(
            Object,
            TEXT("cache_frame"),
            ExpectedTransformCount,
            ExpectedCurveCount,
            OutMessage,
            OutError,
            &bCountMismatch))
    {
        if (bCountMismatch && OutErrorCode)
        {
            *OutErrorCode = TEXT("CACHE_FRAME_CONTENTS_INVALID");
        }
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

bool FMtoUProtocol::ParseCacheClear(
    const TArray<uint8>& Payload, int32& OutClearId, FString& OutError)
{
    OutClearId = INDEX_NONE;
    OutError.Reset();
    TSharedPtr<FJsonObject> Object;
    if (!ParseObject(Payload, Object, OutError))
    {
        return false;
    }
    FString Type;
    if (!GetStringField(Object, TEXT("type"), Type, OutError) || Type != TEXT("cache_clear"))
    {
        OutError = TEXT("Message type must be 'cache_clear'.");
        return false;
    }
    TSharedPtr<FJsonValue> ClearIdValue;
    if (!GetTypedField(Object, TEXT("clear_id"), EJson::Number, ClearIdValue, OutError)
        || !GetExactInt(ClearIdValue, OutClearId) || OutClearId < 1)
    {
        OutError = TEXT("Field 'clear_id' must be a positive int32 JSON number.");
        return false;
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
    int32 NegotiatedRevision,
    int32 SessionId,
    const FString& ObjectId,
    int32 SetRevision,
    int32 SpaceRevision)
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
    // v10 echoes the negotiated identity so a peer can refuse a reply that
    // belongs to another session, object set or common space.
    if (SessionId != INDEX_NONE)
    {
        Object->SetNumberField(TEXT("session_id"), SessionId);
    }
    if (!ObjectId.IsEmpty())
    {
        Object->SetStringField(TEXT("object_id"), ObjectId);
    }
    if (SetRevision != INDEX_NONE)
    {
        Object->SetNumberField(TEXT("set_revision"), SetRevision);
    }
    if (SpaceRevision != INDEX_NONE)
    {
        Object->SetNumberField(TEXT("space_revision"), SpaceRevision);
    }
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

TArray<uint8> FMtoUProtocol::EncodeHelloAck(
    int32 SessionId,
    bool bLinkage,
    bool bReference,
    bool bCache,
    bool bModel,
    const FString& TargetObjectId,
    const FString& TargetKind,
    const FString& TargetBindingPath,
    const FString& TargetGeneration,
    int32 SpaceRevision,
    const TSharedPtr<FJsonObject>& Anchor)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("hello_ack"));
    Object->SetNumberField(TEXT("version"), FMtoUProtocol::Version);
    Object->SetNumberField(TEXT("session_id"), SessionId);

    const TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetBoolField(TEXT("linkage"), bLinkage);
    Capabilities->SetBoolField(TEXT("reference"), bReference);
    Capabilities->SetBoolField(TEXT("cache"), bCache);
    Capabilities->SetBoolField(TEXT("model"), bModel);
    Object->SetObjectField(TEXT("capabilities"), Capabilities);

    // The target describes the explicitly selected target and stays empty
    // until the user pairs one; an empty descriptor never names a takeover.
    const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("object_id"), TargetObjectId);
    Target->SetStringField(TEXT("kind"), TargetKind);
    Target->SetStringField(TEXT("binding_path"), TargetBindingPath);
    Target->SetStringField(TEXT("generation"), TargetGeneration);
    Object->SetObjectField(TEXT("target"), Target);

    const TSharedRef<FJsonObject> Space = MakeShared<FJsonObject>();
    Space->SetNumberField(TEXT("space_revision"), SpaceRevision);
    Space->SetObjectField(TEXT("anchor"), Anchor.IsValid()
        ? Anchor.ToSharedRef()
        : MakeShared<FJsonObject>());
    Object->SetObjectField(TEXT("space"), Space);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeModeState(
    int32 SessionId,
    int32 ModeRevision,
    const FString& Mode,
    const FString& Control,
    const FString& Detail)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("mode_state"));
    Object->SetNumberField(TEXT("session_id"), SessionId);
    Object->SetNumberField(TEXT("mode_revision"), ModeRevision);
    Object->SetStringField(TEXT("mode"), Mode);
    Object->SetStringField(TEXT("control"), Control);
    Object->SetStringField(TEXT("detail"), Detail);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCameraTime(
    int32 SessionId,
    int32 SetRevision,
    int32 ModeRevision,
    int32 SpaceRevision,
    const FMtoUCameraTimeTarget& Target)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("camera_time"));
    Object->SetNumberField(TEXT("session_id"), SessionId);
    Object->SetNumberField(TEXT("set_revision"), SetRevision);
    Object->SetNumberField(TEXT("mode_revision"), ModeRevision);
    Object->SetNumberField(TEXT("space_revision"), SpaceRevision);
    Object->SetStringField(TEXT("eval_id"), Target.EvalId);
    Object->SetNumberField(TEXT("eval_serial"), Target.EvalSerial);
    Object->SetNumberField(TEXT("transport_serial"), Target.TransportSerial);
    Object->SetNumberField(TEXT("frame_serial"), Target.FrameSerial);

    const TSharedRef<FJsonObject> Time = MakeShared<FJsonObject>();
    Time->SetNumberField(TEXT("display_frame"), Target.DisplayFrame);
    Time->SetNumberField(TEXT("source_frame"), Target.DisplayFrame);
    Time->SetNumberField(TEXT("seconds"), Target.Seconds);
    Time->SetNumberField(TEXT("tick"), Target.DisplayFrame);
    {
        const TSharedRef<FJsonObject> DisplayRate = MakeShared<FJsonObject>();
        DisplayRate->SetNumberField(TEXT("numerator"), Target.DisplayRateNumerator);
        DisplayRate->SetNumberField(TEXT("denominator"), Target.DisplayRateDenominator);
        Time->SetObjectField(TEXT("display_rate"), DisplayRate);
        const TSharedRef<FJsonObject> TickResolution = MakeShared<FJsonObject>();
        TickResolution->SetNumberField(TEXT("numerator"), Target.TickNumerator);
        TickResolution->SetNumberField(TEXT("denominator"), Target.TickDenominator);
        Time->SetObjectField(TEXT("tick_resolution"), TickResolution);
        const TSharedRef<FJsonObject> PlaybackRange = MakeShared<FJsonObject>();
        PlaybackRange->SetNumberField(TEXT("start"), Target.PlaybackRangeStart);
        PlaybackRange->SetNumberField(TEXT("end"), Target.PlaybackRangeEnd);
        Time->SetObjectField(TEXT("playback_range"), PlaybackRange);
    }
    Object->SetObjectField(TEXT("time"), Time);
    Object->SetObjectField(TEXT("camera"), Target.Camera.IsValid()
        ? Target.Camera.ToSharedRef()
        : MakeShared<FJsonObject>());
    Object->SetObjectField(TEXT("gate"), Target.Gate.IsValid()
        ? Target.Gate.ToSharedRef()
        : MakeShared<FJsonObject>());
    if (!Target.CutStage.IsEmpty() || !Target.CutName.IsEmpty())
    {
        const TSharedRef<FJsonObject> Cut = MakeShared<FJsonObject>();
        Cut->SetStringField(TEXT("stage"), Target.CutStage);
        Cut->SetStringField(TEXT("name"), Target.CutName);
        Object->SetObjectField(TEXT("cut"), Cut);
    }
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeApplied(
    int32 SessionId,
    int32 ModeRevision,
    const FString& EvalId,
    int32 SampleSerial,
    bool bApplied,
    double AppliedDisplayFrame,
    double LagSeconds,
    const TArray<FMtoUAppliedComponent>& Components,
    const FString& Reason)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("applied"));
    Object->SetNumberField(TEXT("session_id"), SessionId);
    Object->SetNumberField(TEXT("mode_revision"), ModeRevision);
    Object->SetStringField(TEXT("eval_id"), EvalId);
    Object->SetNumberField(TEXT("sample_serial"), SampleSerial);
    Object->SetBoolField(TEXT("applied"), bApplied);
    Object->SetNumberField(TEXT("applied_display_frame"), AppliedDisplayFrame);
    Object->SetNumberField(TEXT("lag_seconds"), LagSeconds);
    TArray<TSharedPtr<FJsonValue>> ComponentValues;
    ComponentValues.Reserve(Components.Num());
    for (const FMtoUAppliedComponent& Component : Components)
    {
        const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("object_id"), Component.ObjectId);
        Row->SetNumberField(TEXT("bones"), Component.BoneCount);
        Row->SetNumberField(TEXT("morphs"), Component.MorphCount);
        Row->SetNumberField(TEXT("parts"), Component.PartCount);
        ComponentValues.Add(MakeShared<FJsonValueObject>(Row));
    }
    Object->SetArrayField(TEXT("components"), ComponentValues);
    // A failed application must name its reason; an applied one reports none.
    Object->SetStringField(TEXT("reason"), Reason);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeReferenceOffer(
    int32 SessionId,
    const FString& TransferId,
    const FString& Directory,
    const FString& ManifestDigest,
    const TSharedPtr<FJsonObject>& Scope,
    int32 SpaceRevision,
    int64 ByteCount,
    int32 ObjectCount,
    int32 TriangleCount)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("reference_offer"));
    Object->SetNumberField(TEXT("session_id"), SessionId);
    Object->SetStringField(TEXT("transfer_id"), TransferId);
    Object->SetStringField(TEXT("directory"), Directory);
    Object->SetStringField(TEXT("manifest_digest"), ManifestDigest);
    Object->SetObjectField(TEXT("scope"), Scope.IsValid()
        ? Scope.ToSharedRef()
        : MakeShared<FJsonObject>());
    Object->SetNumberField(TEXT("space_revision"), SpaceRevision);

    const TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
    Counts->SetNumberField(TEXT("objects"), ObjectCount);
    Counts->SetNumberField(TEXT("triangles"), TriangleCount);
    Counts->SetNumberField(TEXT("bytes"), ByteCount);
    Object->SetObjectField(TEXT("counts"), Counts);
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

TArray<uint8> FMtoUProtocol::EncodeCachePlaying(int32 UploadId, int32 PlayId, bool bLoopEnabled)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_playing"));
    Object->SetNumberField(TEXT("upload_id"), UploadId);
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetBoolField(TEXT("loop"), bLoopEnabled);
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

TArray<uint8> FMtoUProtocol::EncodeCachePaused(
    int32 PlayId, int32 AppliedFrames, int32 SourceFrame)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_paused"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("applied_frames"), AppliedFrames);
    if (SourceFrame != INDEX_NONE)
    {
        Object->SetNumberField(TEXT("source_frame"), SourceFrame);
    }
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheResumed(
    int32 PlayId, int32 AppliedFrames, int32 SourceFrame)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_resumed"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("applied_frames"), AppliedFrames);
    if (SourceFrame != INDEX_NONE)
    {
        Object->SetNumberField(TEXT("source_frame"), SourceFrame);
    }
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheSeeked(
    int32 PlayId, int32 SourceFrame, int32 AppliedFrames)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_seeked"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("source_frame"), SourceFrame);
    Object->SetNumberField(TEXT("applied_frames"), AppliedFrames);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheLooped(int32 PlayId, int32 Round, int32 SourceFrame)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_looped"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("round"), Round);
    Object->SetNumberField(TEXT("source_frame"), SourceFrame);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheLoopChanged(int32 PlayId, bool bLoopEnabled)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_loop_changed"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetBoolField(TEXT("loop"), bLoopEnabled);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheComplete(
    int32 PlayId,
    int32 AppliedFrameCount,
    double ElapsedSeconds,
    bool bWholeCacheScope,
    int32 StartFrame,
    int32 EndFrame)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_complete"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("applied_frame_count"), AppliedFrameCount);
    Object->SetNumberField(TEXT("elapsed_seconds"), ElapsedSeconds);
    Object->SetStringField(
        TEXT("scope"), bWholeCacheScope ? TEXT("cache") : TEXT("segment"));
    Object->SetNumberField(TEXT("start_frame"), StartFrame);
    Object->SetNumberField(TEXT("end_frame"), EndFrame);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheStopped(int32 PlayId)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_stopped"));
    Object->SetNumberField(TEXT("play_id"), PlayId);
    return EncodeObject(Object);
}

TArray<uint8> FMtoUProtocol::EncodeCacheCleared(int32 UploadId, int32 PlayId, int32 ClearId)
{
    const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("type"), TEXT("cache_cleared"));
    Object->SetNumberField(TEXT("upload_id"), UploadId);
    Object->SetNumberField(TEXT("play_id"), PlayId);
    Object->SetNumberField(TEXT("clear_id"), ClearId);
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

bool FMtoUProtocol::MakeRetargetedFrameData(
    const FMtoUFrameMessage& Frame,
    const TArray<int32>& AcceptedCurveIndices,
    const TArray<FTransform>& SourceBindLocalPose,
    const TArray<FTransform>& TargetRefLocalPose,
    const TArray<int32>& BoneParents,
    FLiveLinkFrameDataStruct& OutFrameData,
    FString& OutError)
{
    OutFrameData = FLiveLinkFrameDataStruct();
    OutError.Reset();
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
        // A component pose is a product of local transforms, so it is
        // invertible exactly when every local transform on its ancestor chain
        // is invertible, and every retargeted local decomposes from a
        // non-singular matrix. Inverse() silently substitutes identity for
        // singular input, so one zero-scale bone would corrupt every descendant
        // without a trace; fail closed before publishing anything.
        FMatrix CheckedInverse;
        if (!TryInverse(SourceCurrentLocal, CheckedInverse))
        {
            OutError = FString::Printf(
                TEXT("Bone %d source current transform is not invertible."), Index);
            return false;
        }
        if (!TryInverse(SourceBindLocal, CheckedInverse))
        {
            OutError = FString::Printf(
                TEXT("Bone %d source bind transform is not invertible."), Index);
            return false;
        }
        if (!TryInverse(TargetRefLocal, CheckedInverse))
        {
            OutError = FString::Printf(
                TEXT("Bone %d target reference transform is not invertible."), Index);
            return false;
        }

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
        FMatrix SourceBindInverse;
        if (!TryInverse(SourceBindComponentPose[Index], SourceBindInverse))
        {
            OutError = FString::Printf(
                TEXT("Bone %d source bind component transform is not invertible."), Index);
            return false;
        }
        TargetCurrentComponentPose[Index] = TargetRefComponentPose[Index]
            * SourceBindInverse * SourceCurrentComponentPose[Index];
    }

    FMtoUFrameMessage RetargetedFrame;
    RetargetedFrame.Transforms.Reserve(BoneCount);
    RetargetedFrame.Curves = Frame.Curves;
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 ParentIndex = BoneParents[Index];
        FMatrix ParentInverse = FMatrix::Identity;
        if (ParentIndex != INDEX_NONE
            && !TryInverse(TargetCurrentComponentPose[ParentIndex], ParentInverse))
        {
            OutError = FString::Printf(
                TEXT("Bone %d target current parent %d is not invertible."), Index, ParentIndex);
            return false;
        }
        const FMatrix TargetCurrentLocal = TargetCurrentComponentPose[Index] * ParentInverse;
        FMatrix RotationMatrix = TargetCurrentLocal;
        FVector Scale = RotationMatrix.ExtractScaling(0.0);
        if (TargetCurrentLocal.ContainsNaN() || Scale.ContainsNaN()
            || Scale.X == 0.0 || Scale.Y == 0.0 || Scale.Z == 0.0)
        {
            OutError = FString::Printf(
                TEXT("Bone %d target current local transform cannot be decomposed."), Index);
            return false;
        }
        if (TargetCurrentLocal.Determinant() < 0.0)
        {
            Scale.X *= -1.0;
            RotationMatrix.SetAxis(0, -RotationMatrix.GetScaledAxis(EAxis::X));
        }
        // FTransform(Matrix) uses an absolute scale threshold and collapses
        // valid tiny axes to zero. Normalize axes before constructing rotation.
        FQuat Rotation(RotationMatrix);
        Rotation.Normalize();
        const FTransform Transform(Rotation, TargetCurrentLocal.GetOrigin(), Scale);
        RetargetedFrame.Transforms.Add({
            Transform.GetTranslation(), Transform.GetRotation(), Transform.GetScale3D()});
    }
    OutFrameData = MakeFrameData(RetargetedFrame, AcceptedCurveIndices);
    return true;
}
