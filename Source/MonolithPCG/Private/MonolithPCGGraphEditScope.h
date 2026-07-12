#pragma once

#include "CoreMinimal.h"

#include "PCGGraph.h"

#include "Misc/EngineVersionComparison.h"

/**
 * Batches PCG graph notifications without double-dispatching the delayed
 * change emitted by UPCGGraph/UPCGSettings editor callbacks. User-paused
 * notification state is preserved while still using the engine's explicit
 * one-shot bypass when an external modification must be announced.
 */
class FMonolithPCGScopedGraphEditNotifications
{
public:
	explicit FMonolithPCGScopedGraphEditNotifications(UPCGGraph* InGraph)
		: Graph(InGraph)
	{
		check(IsInGameThread());
		if (Graph)
		{
			bNotificationsPausedByUser = Graph->NotificationsForEditorArePausedByUser();
			Graph->PrimeGraphCompilationCache();
			Graph->DisableNotificationsForEditor();
		}
	}

	~FMonolithPCGScopedGraphEditNotifications()
	{
		if (!Graph)
		{
			return;
		}

		if (bExternalModification && !bNotificationsPausedByUser)
		{
			ForceExternalModification();
		}
		Graph->EnableNotificationsForEditor();
		if (bExternalModification && bNotificationsPausedByUser)
		{
			ForceExternalModification();
		}
	}

	void MarkExternalModification()
	{
		bExternalModification = true;
	}

private:
	void ForceExternalModification() const
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		Graph->ForceNotificationForEditor(EPCGChangeType::ExternalModification);
#else
		Graph->ForceNotificationForEditor(EPCGChangeType::Structural);
#endif
	}

	UPCGGraph* Graph = nullptr;
	bool bExternalModification = false;
	bool bNotificationsPausedByUser = false;
};
