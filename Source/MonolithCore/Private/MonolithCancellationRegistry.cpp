#include "MonolithCancellationRegistry.h"

namespace
{
	bool IsAddressableRequestId(const FString& RequestId)
	{
		return !RequestId.IsEmpty()
			&& RequestId != TEXT("notification")
			&& RequestId != TEXT("unknown");
	}
}

FMonolithCancellationRegistry& FMonolithCancellationRegistry::Get()
{
	static FMonolithCancellationRegistry Instance;
	return Instance;
}

void FMonolithCancellationRegistry::Register(const FString& RequestId)
{
	if (!IsAddressableRequestId(RequestId))
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	ActiveRequests.FindOrAdd(RequestId);
}

void FMonolithCancellationRegistry::Unregister(const FString& RequestId)
{
	if (RequestId.IsEmpty())
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	ActiveRequests.Remove(RequestId);
}

bool FMonolithCancellationRegistry::RequestCancellation(const FString& RequestId, const FString& Reason)
{
	if (RequestId.IsEmpty())
	{
		return false;
	}
	FScopeLock ScopeLock(&Lock);
	FEntry* Entry = ActiveRequests.Find(RequestId);
	if (!Entry)
	{
		return false;
	}
	Entry->bCancelled = true;
	Entry->Reason = Reason;
	return true;
}

bool FMonolithCancellationRegistry::IsCancellationRequested(const FString& RequestId) const
{
	if (RequestId.IsEmpty())
	{
		return false;
	}
	FScopeLock ScopeLock(&Lock);
	const FEntry* Entry = ActiveRequests.Find(RequestId);
	return Entry != nullptr && Entry->bCancelled;
}

int32 FMonolithCancellationRegistry::GetActiveCount() const
{
	FScopeLock ScopeLock(&Lock);
	return ActiveRequests.Num();
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithCancellationRegistry::ResetForTests()
{
	FScopeLock ScopeLock(&Lock);
	ActiveRequests.Empty();
}
#endif

FScopedMonolithCancellationRegistration::FScopedMonolithCancellationRegistration(const FString& InRequestId)
	: RequestId(InRequestId)
{
	if (IsAddressableRequestId(RequestId))
	{
		FMonolithCancellationRegistry::Get().Register(RequestId);
		bActive = true;
	}
}

FScopedMonolithCancellationRegistration::~FScopedMonolithCancellationRegistration()
{
	if (bActive)
	{
		FMonolithCancellationRegistry::Get().Unregister(RequestId);
	}
}

bool FScopedMonolithCancellationRegistration::IsCancellationRequested() const
{
	return bActive && FMonolithCancellationRegistry::Get().IsCancellationRequested(RequestId);
}
