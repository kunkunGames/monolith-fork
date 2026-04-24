#include "MonolithBlueprintEditCradle.h"
#include "UObject/UnrealType.h"

namespace MonolithEditCradle
{

bool MayContainObjectRef(FProperty* Prop)
{
    if (!Prop) return false;
    if (CastField<FObjectProperty>(Prop))     return true;
    if (CastField<FSoftObjectProperty>(Prop)) return true;
    if (CastField<FWeakObjectProperty>(Prop)) return true;
    if (CastField<FInterfaceProperty>(Prop))  return true;
    if (CastField<FStructProperty>(Prop))     return true;  // may contain nested refs
    if (CastField<FArrayProperty>(Prop))      return true;  // inner may be object ref
    if (CastField<FMapProperty>(Prop))        return true;
    if (CastField<FSetProperty>(Prop))        return true;
    return false;
}

/** Fire Pre+Post for a single property chain */
static void FireEditNotification(UObject* Obj, const TArray<FProperty*>& PropertyChain)
{
    if (PropertyChain.Num() == 0 || !Obj) return;

    // Build FEditPropertyChain: root at head, leaf at tail.
    // AddTail in forward iteration produces [Root -> Struct -> Leaf] which is
    // the ordering UE expects (outermost at head, innermost at tail).
    FEditPropertyChain Chain;
    for (FProperty* P : PropertyChain)
    {
        Chain.AddTail(P);
    }
    Chain.SetActivePropertyNode(PropertyChain.Last());

    Obj->PreEditChange(Chain);

    FPropertyChangedEvent ChangedEvent(PropertyChain.Last(), EPropertyChangeType::ValueSet);
    FPropertyChangedChainEvent ChainEvent(Chain, ChangedEvent);
    Obj->PostEditChangeChainProperty(ChainEvent);
}

/** Returns true if this property IS an object-ref leaf */
static bool IsObjectRefLeaf(FProperty* Prop)
{
    return CastField<FObjectProperty>(Prop)
        || CastField<FSoftObjectProperty>(Prop)
        || CastField<FWeakObjectProperty>(Prop)
        || CastField<FInterfaceProperty>(Prop);
}

void FireCradleRecursive(
    UObject* Obj,
    FProperty* Prop,
    const void* ContainerPtr,
    TArray<FProperty*>& ParentChain)
{
    // IMPORTANT: ContainerPtr is the container of Prop, not the UObject.
    // For top-level: ContainerPtr == UObject*.
    // For nested struct fields: ContainerPtr == struct value pointer.
    const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);

    // --- Struct: iterate members ---
    if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
    {
        for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
        {
            FProperty* Inner = *It;

            ParentChain.Add(Inner);

            if (IsObjectRefLeaf(Inner))
            {
                FireEditNotification(Obj, ParentChain);
            }

            if (MayContainObjectRef(Inner) && !IsObjectRefLeaf(Inner))
            {
                // NOTE: ValuePtr is the struct's value — it becomes the
                // container for inner properties (their offsets are relative
                // to the struct, not the UObject).
                FireCradleRecursive(Obj, Inner, ValuePtr, ParentChain);
            }

            ParentChain.Pop();
        }
    }

    // --- Array: iterate elements ---
    else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
    {
        FScriptArrayHelper Helper(ArrayProp, ValuePtr);
        FProperty* Inner = ArrayProp->Inner;

        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            const void* ElemPtr = Helper.GetRawPtr(i);

            ParentChain.Add(Inner);

            if (IsObjectRefLeaf(Inner))
            {
                FireEditNotification(Obj, ParentChain);
            }

            if (MayContainObjectRef(Inner) && !IsObjectRefLeaf(Inner))
            {
                // ElemPtr is the element value — container for inner's sub-props
                FireCradleRecursive(Obj, Inner, ElemPtr, ParentChain);
            }

            ParentChain.Pop();
        }
    }

    // --- Map: iterate entries (key + value) ---
    else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
    {
        FScriptMapHelper Helper(MapProp, ValuePtr);

        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;

            // Process both key and value properties
            for (FProperty* Inner : { MapProp->KeyProp, MapProp->ValueProp })
            {
                const void* Ptr = (Inner == MapProp->KeyProp)
                    ? Helper.GetKeyPtr(i) : Helper.GetValuePtr(i);

                ParentChain.Add(Inner);

                if (IsObjectRefLeaf(Inner))
                {
                    FireEditNotification(Obj, ParentChain);
                }

                if (MayContainObjectRef(Inner) && !IsObjectRefLeaf(Inner))
                {
                    FireCradleRecursive(Obj, Inner, Ptr, ParentChain);
                }

                ParentChain.Pop();
            }
        }
    }

    // --- Set: iterate elements ---
    else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
    {
        FScriptSetHelper Helper(SetProp, ValuePtr);
        FProperty* Inner = SetProp->ElementProp;

        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;

            const void* ElemPtr = Helper.GetElementPtr(i);

            ParentChain.Add(Inner);

            if (IsObjectRefLeaf(Inner))
            {
                FireEditNotification(Obj, ParentChain);
            }

            if (MayContainObjectRef(Inner) && !IsObjectRefLeaf(Inner))
            {
                FireCradleRecursive(Obj, Inner, ElemPtr, ParentChain);
            }

            ParentChain.Pop();
        }
    }
}

void FireFullCradle(UObject* Obj, FProperty* Prop)
{
    if (!Obj || !Prop) return;

    // If the property itself is an object-ref leaf, fire once for it
    if (IsObjectRefLeaf(Prop))
    {
        TArray<FProperty*> Chain = { Prop };
        FireEditNotification(Obj, Chain);
        return;
    }

    // Otherwise recurse into struct/array/map/set looking for nested object refs
    if (MayContainObjectRef(Prop))
    {
        TArray<FProperty*> Chain = { Prop };
        FireCradleRecursive(Obj, Prop, Obj, Chain);
    }
}

} // namespace MonolithEditCradle
