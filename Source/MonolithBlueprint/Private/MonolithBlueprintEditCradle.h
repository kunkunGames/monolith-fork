#pragma once

#include "CoreMinimal.h"

namespace MonolithEditCradle
{
    /**
     * Recursively walks a property tree and fires PreEditChange/PostEditChangeChainProperty
     * for every leaf FObjectProperty/FSoftObjectProperty/FWeakObjectProperty/FInterfaceProperty.
     * This ensures FOverridableManager marks each inner property as overridden.
     *
     * @param Obj           The UObject being edited (CDO or DataAsset)
     * @param Prop          Current property to inspect
     * @param ContainerPtr  Pointer to the CONTAINER of Prop's value. For top-level calls this
     *                      is the UObject*. For nested struct fields, this is the struct's
     *                      value pointer (NOT the UObject). ContainerPtrToValuePtr uses the
     *                      property's offset relative to this pointer.
     * @param ParentChain   Chain built so far (empty at top level, grows as we descend)
     */
    void FireCradleRecursive(
        UObject* Obj,
        FProperty* Prop,
        const void* ContainerPtr,
        TArray<FProperty*>& ParentChain
    );

    /**
     * Convenience wrapper: fires the cradle for a single top-level property.
     * Does NOT wrap in transaction/Modify — caller is expected to do that.
     */
    void FireFullCradle(UObject* Obj, FProperty* Prop);

    /** Returns true if the property holds or may contain an object reference. */
    bool MayContainObjectRef(FProperty* Prop);
}
