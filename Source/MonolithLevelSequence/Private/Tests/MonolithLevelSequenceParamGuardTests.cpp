#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithLevelSequenceActions.h"
#include "MonolithMovieRenderQueueActions.h"
#include "MonolithTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceTypedParamsTest, "Monolith.ParamValidation.MonolithLevelSequence.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceTypedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(TEXT("level_sequence"));

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("level_sequence"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithLevelSequenceActions::RegisterActions(Registry);
		},
		{
			{ TEXT("ping"), true, TEXT("level_sequence.ping is registered") },
			{ TEXT("get_replay_status"), true, TEXT("level_sequence.get_replay_status is registered") },
			{ TEXT("get_anim_mixer_status"), true, TEXT("level_sequence.get_anim_mixer_status is registered") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("level_sequence"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithLevelSequenceActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("list_saved_replays"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("limit"), 0.0);
				},
				TEXT("limit"),
				TEXT("list_saved_replays rejects limit below range")
			},
			{
				TEXT("get_saved_replay"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("saved_relative_path"), 42.0);
				},
				TEXT("saved_relative_path"),
				TEXT("get_saved_replay rejects non-string saved_relative_path")
			},
			{
				TEXT("get_saved_replay"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("saved_relative_path"), TEXT("Demos/Replay1"));
					Params->SetNumberField(TEXT("file_limit"), 501.0);
				},
				TEXT("file_limit"),
				TEXT("get_saved_replay rejects file_limit above range")
			},
			{
				TEXT("list_directors"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path_filter"), 42.0);
				},
				TEXT("asset_path_filter"),
				TEXT("list_directors rejects non-string asset_path_filter")
			},
			{
				TEXT("get_director_info"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetBoolField(TEXT("asset_path"), true);
				},
				TEXT("asset_path"),
				TEXT("get_director_info rejects non-string asset_path")
			},
			{
				TEXT("list_director_functions"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cinematics/LS_Intro.LS_Intro"));
					Params->SetStringField(TEXT("kind"), TEXT("bogus"));
				},
				TEXT("kind"),
				TEXT("list_director_functions rejects unknown kind")
			},
			{
				TEXT("list_event_bindings"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 42.0);
				},
				TEXT("asset_path"),
				TEXT("list_event_bindings rejects non-string asset_path")
			},
			{
				TEXT("find_director_function_callers"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetBoolField(TEXT("function_name"), true);
				},
				TEXT("function_name"),
				TEXT("find_director_function_callers rejects non-string function_name")
			},
			{
				TEXT("list_director_variables"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("asset_path"), 42.0);
				},
				TEXT("asset_path"),
				TEXT("list_director_variables rejects non-string asset_path")
			},
			{
				TEXT("list_bindings"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cinematics/LS_Intro.LS_Intro"));
					Params->SetStringField(TEXT("kind"), TEXT("bogus"));
				},
				TEXT("kind"),
				TEXT("list_bindings rejects unknown kind")
			},
			{
				TEXT("list_anim_mixer_tracks"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cinematics/LS_Intro.LS_Intro"));
					Params->SetStringField(TEXT("include_layers"), TEXT("true"));
				},
				TEXT("include_layers"),
				TEXT("list_anim_mixer_tracks rejects non-boolean include_layers")
			}
		});

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMovieRenderTypedParamsTest, "Monolith.ParamValidation.MonolithLevelSequence.MovieRender.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMovieRenderTypedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(TEXT("movie_render"));

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("movie_render"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithMovieRenderQueueActions::RegisterActions(Registry);
		},
		{
			{ TEXT("get_queue"), true, TEXT("movie_render.get_queue is registered") },
			{ TEXT("delete_all_jobs"), true, TEXT("movie_render.delete_all_jobs is registered") },
			{ TEXT("is_rendering"), true, TEXT("movie_render.is_rendering is registered") },
			{ TEXT("render_progress"), true, TEXT("movie_render.render_progress is registered") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("movie_render"),
		[](FMonolithToolRegistry& Registry)
		{
			FMonolithMovieRenderQueueActions::RegisterActions(Registry);
		},
		{
			{
				TEXT("load_queue"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cinematics/Q_Render.Q_Render"));
					Params->SetStringField(TEXT("prompt_on_dirty"), TEXT("false"));
				},
				TEXT("prompt_on_dirty"),
				TEXT("load_queue rejects non-boolean prompt_on_dirty")
			},
			{
				TEXT("save_queue"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cinematics/Q_Render"));
					Params->SetStringField(TEXT("allow_overwrite"), TEXT("false"));
				},
				TEXT("allow_overwrite"),
				TEXT("save_queue rejects non-boolean allow_overwrite")
			},
			{
				TEXT("add_job"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("sequence_path"), TEXT("/Game/Cinematics/LS_Intro.LS_Intro"));
					Params->SetStringField(TEXT("clear_existing"), TEXT("false"));
				},
				TEXT("clear_existing"),
				TEXT("add_job rejects non-boolean clear_existing")
			},
			{
				TEXT("duplicate_job"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("index"), 1.25);
				},
				TEXT("index"),
				TEXT("duplicate_job rejects non-integral index")
			},
			{
				TEXT("delete_job"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetBoolField(TEXT("index"), true);
				},
				TEXT("index"),
				TEXT("delete_job rejects non-integer index")
			},
			{
				TEXT("set_job_index"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("index"), 1.0);
					Params->SetStringField(TEXT("new_index"), TEXT("2"));
				},
				TEXT("new_index"),
				TEXT("set_job_index rejects non-integer new_index")
			},
			{
				TEXT("list_settings"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetBoolField(TEXT("filter"), true);
				},
				TEXT("filter"),
				TEXT("list_settings rejects non-string filter")
			},
			{
				TEXT("render_queue"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("confirm"), TEXT("true"));
				},
				TEXT("confirm"),
				TEXT("render_queue rejects non-boolean confirm")
			},
			{
				TEXT("cancel_render"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("cancel_all"), TEXT("true"));
				},
				TEXT("cancel_all"),
				TEXT("cancel_render rejects non-boolean cancel_all")
			}
		});

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
