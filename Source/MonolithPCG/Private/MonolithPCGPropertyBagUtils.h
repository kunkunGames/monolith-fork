#pragma once

#include "Misc/EngineVersionComparison.h"
#include "StructUtils/PropertyBag.h"

namespace MonolithPCGPropertyBagUtils
{
inline bool AreExactlyEquivalent(
	const FInstancedPropertyBag& Expected,
	const FInstancedPropertyBag* Actual)
{
	if (!Actual)
	{
		return false;
	}

	const UPropertyBag* ExpectedStruct = Expected.GetPropertyBagStruct();
	const UPropertyBag* ActualStruct = Actual->GetPropertyBagStruct();
	if (!ExpectedStruct || !ActualStruct)
	{
		return ExpectedStruct == ActualStruct;
	}

	const TConstArrayView<FPropertyBagPropertyDesc> ExpectedDescs =
		ExpectedStruct->GetPropertyDescs();
	const TConstArrayView<FPropertyBagPropertyDesc> ActualDescs =
		ActualStruct->GetPropertyDescs();
	if (ExpectedDescs.Num() != ActualDescs.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < ExpectedDescs.Num(); ++Index)
	{
		const FPropertyBagPropertyDesc& ExpectedDesc = ExpectedDescs[Index];
		const FPropertyBagPropertyDesc& ActualDesc = ActualDescs[Index];
		if (ExpectedDesc.ID != ActualDesc.ID ||
			ExpectedDesc.Name != ActualDesc.Name ||
			ExpectedDesc.ValueType != ActualDesc.ValueType ||
			ExpectedDesc.ValueTypeObject.Get() != ActualDesc.ValueTypeObject.Get() ||
			ExpectedDesc.ContainerTypes != ActualDesc.ContainerTypes ||
			ExpectedDesc.PropertyFlags != ActualDesc.PropertyFlags
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			||
			ExpectedDesc.KeyType != ActualDesc.KeyType ||
			ExpectedDesc.KeyTypeObject.Get() != ActualDesc.KeyTypeObject.Get()
#endif
			)
		{
			return false;
		}

		const TValueOrError<FString, EPropertyBagResult> ExpectedValue =
			Expected.GetValueSerializedString(ExpectedDesc.Name);
		const TValueOrError<FString, EPropertyBagResult> ActualValue =
			Actual->GetValueSerializedString(ActualDesc.Name);
		if (!ExpectedValue.IsValid() ||
			!ActualValue.IsValid() ||
			ExpectedValue.GetValue() != ActualValue.GetValue())
		{
			return false;
		}
	}

	return true;
}
}
