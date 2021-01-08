#include "OFP_Videobrowser.h"

#include "OFS_Util.h"
#include "EventSystem.h"
#include "OFS_ImGui.h"
#include "Funscript.h"
#include "OFP.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "SDL_thread.h"
#include "SDL_atomic.h"
#include "SDL_timer.h"

#include <filesystem>
#include <sstream>

#include <future>

#ifdef WIN32
//used to obtain drives on windows
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif


#ifndef NDEBUG
constexpr uint64_t MaxFileSearchFutures = 4;
#else
constexpr uint64_t MaxFileSearchFutures = 16;
#endif


SDL_sem* Videobrowser::ThumbnailThreadSem = SDL_CreateSemaphore(Videobrowser::MaxThumbailProcesses);

static uint64_t GetFileAge(const std::filesystem::path& path) {
	std::error_code ec;
	uint64_t timestamp = 0;

#ifdef WIN32
	HANDLE file = CreateFileW((wchar_t*)path.u16string().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		LOGF_ERROR("Could not open file \"%s\", error 0x%08x", path.u8string().c_str(), GetLastError());
	}
	else {
		FILETIME ftCreate;
		if (!GetFileTime(file, &ftCreate, NULL, NULL))
		{
			LOG_ERROR("Couldn't GetFileTime");
			timestamp = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
		}
		else {
			timestamp = *(uint64_t*)&ftCreate;
		}
		CloseHandle(file);
	}
#else
	timestamp = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
#endif
	return timestamp;
}

void Videobrowser::updateLibraryCache() noexcept
{
	CacheUpdateInProgress = true;
	CacheNeedsUpdate = false;
	struct UpdateLibraryThreadData {
		Videobrowser* browser;
	};

	auto updateLibrary = [](void* ctx) -> int {
		auto data = static_cast<UpdateLibraryThreadData*>(ctx);
		auto& browser = *data->browser;

		// clear current items
		{
			auto handle = EventSystem::WaitableSingleShot(
				[](void* ctx)
				{
					auto browser = (Videobrowser*)ctx;
					// this needs to run on the gl thread because
					//textures might be freed
					SDL_AtomicLock(&browser->ItemsLock);
					browser->Items.clear();
					SDL_AtomicUnlock(&browser->ItemsLock);
				}, &browser);
			handle->wait();
		}

		auto checkForRemovedFiles = []()
		{
			auto allVideos = Videolibrary::GetAll<Video>();
			for (auto& video : allVideos) {
				LOGF_INFO("Looking for %s", video.videoFilename.c_str());
				if (!Util::FileExists(video.videoPath))
				{
					Videolibrary::Remove<Video>(video.id);
					LOGF_INFO("Couldn't find %s", video.videoPath.c_str());
				}
				else if (!Util::FileExists(video.scriptPath))
				{
					Videolibrary::Remove<Video>(video.id);
					LOGF_INFO("Couldn't find %s", video.scriptPath.c_str());
				}
			}
		};

		auto searchPathsForVideos = [](UpdateLibraryThreadData* data)
		{
			auto& searchPaths = data->browser->settings->SearchPaths;
			for (auto& sPath : searchPaths) {
				auto pathObj = Util::PathFromString(sPath.path);

				auto handleItem = [browser = data->browser](auto p) {
					auto extension = p.path().extension().u8string();
					auto pathString = p.path().u8string();
					auto filename = p.path().filename().u8string();

					auto it = std::find_if(BrowserExtensions.begin(), BrowserExtensions.end(),
						[&](auto& ext) {
							return std::strcmp(ext.first, extension.c_str()) == 0;
						});
					if (it != BrowserExtensions.end()) {
						// extension matches
						auto funscript = p.path();
						funscript.replace_extension(".funscript");
						auto funscript_path = funscript.u8string();
						bool matchingScript = Util::FileExists(funscript_path);
						if (!matchingScript) return;


						// valid extension + matching script
						size_t byte_count = p.file_size();
						auto timestamp = GetFileAge(p.path());

						Video vid;
						vid.videoPath = pathString;
						vid.byte_count = byte_count;
						vid.videoFilename = filename;
						vid.hasScript = matchingScript;
						vid.timestamp = timestamp;
						vid.shouldGenerateThumbnail = it->second;
						vid.scriptPath = funscript_path;
						if (!vid.try_insert()) {
							// failed to insert
							return;
						}

						{
							Funscript::Metadata meta;
							meta.loadFromFunscript(funscript.u8string());
							for (auto& mtag : meta.tags) {
								Tag imported;
								imported.tag = mtag;
								imported.try_insert();

								if (imported.id < 0) {
									using namespace sqlite_orm;
									try
									{
										auto tag = Videolibrary::TagByName(imported.tag);
										FUN_ASSERT(tag.has_value(), "where did the tag go!?");
										imported.id = tag->id;
									}
									catch (std::system_error& er) {
										LOGF_ERROR("%s", er.what());
									}
								}
								VideoAndTag connect;
								connect.tagId = imported.id;
								connect.videoId = vid.id;
								try {
									// must be replaced because tagId & videoId are primary keys
									connect.replace();
								}
								catch (std::system_error& er) {
									LOGF_ERROR("%s", er.what());
								}
							}
						}

						SDL_AtomicLock(&browser->ItemsLock);
						browser->Items.emplace_back(std::move(vid));
						SDL_AtomicUnlock(&browser->ItemsLock);
						LOGF_DEBUG("done hanlding %s", filename.c_str());
					}
				};

				std::error_code ec;
				std::vector<std::future<void>> futures;
				if (sPath.recursive) {
					for (auto& p : std::filesystem::recursive_directory_iterator(pathObj, ec)) {
						if (p.is_directory()) continue;
						futures.emplace_back(std::async(std::launch::async, handleItem, p));
						if (futures.size() == MaxFileSearchFutures) {
							for (auto& future : futures) {
								future.wait();
							}
							futures.clear();
						}
					}
				}
				else {
					for (auto& p : std::filesystem::directory_iterator(pathObj, ec)) {
						if (p.is_directory()) continue;
						futures.emplace_back(std::async(std::launch::async, handleItem, p));
						if (futures.size() == MaxFileSearchFutures) {
							for (auto& future : futures) {
								future.wait();
							}
							futures.clear();
						}
					}
				}
				for (auto& fut : futures) { fut.wait(); }
				LOGF_DEBUG("Done iterating \"%s\" %s", sPath.path.c_str(), sPath.recursive ? "recursively" : "");
			}
		};

		checkForRemovedFiles();
		searchPathsForVideos(data);

		// set items equal to all videos
		{
			auto handle = EventSystem::WaitableSingleShot(
				[](void* ctx)
				{
					auto browser = (Videobrowser*)ctx;
					// this needs to run on the gl thread 
					SDL_AtomicLock(&browser->ItemsLock);
					auto allVideos = Videolibrary::GetAll<Video>();
					browser->setVideos(allVideos);
					SDL_AtomicUnlock(&browser->ItemsLock);
				}
				, &browser);
			handle->wait();
		}

		data->browser->CacheUpdateInProgress = false;
		delete data;
		return 0;
	};

	auto data = new UpdateLibraryThreadData;
	data->browser = this;
	auto thread = SDL_CreateThread(updateLibrary, "UpdateVideoLibrary", data);
	SDL_DetachThread(thread);
}

Videobrowser::Videobrowser(VideobrowserSettings* settings)
	: settings(settings)
{
	Videolibrary::init();
#ifndef NDEBUG
	//foo
	if constexpr(false)
	{
		Videolibrary::DeleteAll();
	}
#endif
	auto vidCache = Videolibrary::GetAll<Video>();
	CacheNeedsUpdate = vidCache.empty();

	preview.setup();

	SDL_AtomicLock(&ItemsLock);
	for (auto& cached : vidCache) {
		Items.emplace_back(std::move(cached));
	}
	std::sort(Items.begin(), Items.end(),
		[](auto& item1, auto& item2) {
			return item1.video.timestamp > item2.video.timestamp;
	});
	SDL_AtomicUnlock(&ItemsLock);	
}

void Videobrowser::Randomizer(const char* Id, bool* open) noexcept
{
	if (open != nullptr && !*open) return;

	//if (Random) {
	//	ImGui::OpenPopup(Id);
	//	ImGui::SetNextWindowSizeConstraints(ImVec2(500, 500), ImVec2(3000, 3000),
	//		[](ImGuiSizeCallbackData* data) {
	//			if (data->CurrentSize.x == data->DesiredSize.x && data->CurrentSize.y == data->DesiredSize.y)return;
	//			float size;
	//			if (data->CurrentSize.x < data->DesiredSize.x || data->CurrentSize.y < data->DesiredSize.y)
	//			{
	//				size = std::min(data->DesiredSize.x, data->DesiredSize.y);
	//			}
	//			else
	//			{
	//				size = std::max(data->DesiredSize.x, data->DesiredSize.y);
	//			}
	//			data->DesiredSize = ImVec2(size, size);
	//		});
	//}

	ImGui::Begin(Id, open, ImGuiWindowFlags_None | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
	//auto hostWidth = ImGui::GetContentRegionAvail().x;
	ImGui::BeginChild("RandomVideo"/* ImVec2(hostWidth, hostWidth), true*/);
	renderRandomizer();
	ImGui::EndChild();
	ImGui::End();
}

void Videobrowser::renderRandomizer() noexcept
{
	if (Items.empty()) return;

	constexpr int32_t MaxRollFreqMs = 125;
	constexpr int32_t PickAfterRolls = 100;
	constexpr int32_t ShowResultTime = 3000;

	static int32_t RollFreqMs = MaxRollFreqMs;
	static int32_t RollCount = 0;
	static int32_t LastRollTime = 0;
	static int32_t LastRoll = 0;

	if (SDL_GetTicks() - LastRollTime >= RollFreqMs)
	{
		if (RollCount == PickAfterRolls) {
			fileClickedHandler(Items[LastRoll]);
			RollFreqMs = MaxRollFreqMs;
			RollCount = 0;
			Random = false;
		}
		else
		{
			if (RollCount == 0)
			{
				srand(SDL_GetTicks());
			}
			LastRoll = rand() % Items.size();
			LastRollTime = SDL_GetTicks();
			RollCount++;

			if (RollCount != PickAfterRolls)
			{
				// roll faster which each roll
				auto easeOutCubic = [](float x) -> float {	x = 1 - x;	return 1 - (x*x*x);	};
				RollFreqMs = MaxRollFreqMs - ((MaxRollFreqMs / 1.25f) * (easeOutCubic((float)RollCount/PickAfterRolls)));
			}
			else
			{
				// show result for 3 seconds
				RollFreqMs = ShowResultTime;
			}
		}
	}

	auto& style = ImGui::GetStyle();
	auto window_pos = ImGui::GetWindowPos();
	auto availSize = ImGui::GetContentRegionAvail() - style.ItemSpacing;
	auto window = ImGui::GetCurrentWindowRead();
	auto frame_bb = ImRect(window->DC.CursorPos, window->DC.CursorPos + availSize);
	auto draw_list = ImGui::GetWindowDrawList();
	
	constexpr float scale = 3.f;
	//ImGui::ItemSize(availSize, 0.0f);
	//if (!ImGui::ItemAdd(frame_bb, 0))
	//	return;

	ImVec2 videoSize((availSize.x / scale) * (16.f / 9.f), (availSize.x / scale));

	ImVec2 centerPos(frame_bb.GetWidth() / 2.f, frame_bb.GetHeight() / 2.f);
	centerPos += frame_bb.Min;
	
	ImVec2 thumbPos = centerPos;
	ImVec2 p1 = thumbPos - (videoSize / 2.f);
	ImVec2 p2 = thumbPos + (videoSize / 2.f);
	draw_list->AddRect(p1, p2, ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Border]), 2.f);

	auto& rolledItem = Items[LastRoll];

	uint32_t texId = rolledItem.texture.GetTexId();
	
	if(RollCount == PickAfterRolls)
	{
		if (!preview.loading)
		{
			preview.previewVideo(rolledItem.video.videoPath, 0.1f);
		}
		if (preview.ready)
		{
			texId = preview.render_texture;
		}
	}

	if (texId != 0) {
		draw_list->AddImage((void*)(intptr_t)texId, p1 + style.FramePadding, p2 - style.FramePadding, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
	}
	else
	{
		rolledItem.GenThumbail();
	}
	auto textSize = ImGui::CalcTextSize(rolledItem.video.videoFilename.c_str());
	draw_list->AddText(
		p1 + ImVec2((videoSize.x/2.f) - (textSize.x/2.f), videoSize.y + style.ItemSpacing.y),
		ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Text]), rolledItem.video.videoFilename.c_str());

	if (RollCount == PickAfterRolls)
	{
		char tmp[16];
		stbsp_snprintf(tmp, sizeof(tmp), "%.2f seconds", (ShowResultTime - (SDL_GetTicks() - LastRollTime)) / 1000.f);
		auto countDownTextSize = ImGui::CalcTextSize(tmp);
		draw_list->AddText(
			p1 + ImVec2((videoSize.x / 2.f) - (countDownTextSize.x / 2.f), textSize.y + videoSize.y + (style.ItemSpacing.y*2.f)),
			ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Text]),
			tmp
		);
	}
	else
	{
		draw_list->AddRectFilledMultiColor(
			p1 + ImVec2(0.f, videoSize.y + textSize.y + style.ItemSpacing.y),
			p1 + ImVec2(videoSize.x * (RollCount / (float)PickAfterRolls), ImGui::GetFontSize() + videoSize.y + textSize.y + style.ItemSpacing.y),
			IM_COL32_BLACK, 
			IM_COL32(255, 0, 0, 255), 
			IM_COL32(255, 0, 0, 255),
			IM_COL32_BLACK
		);
	}
}

void Videobrowser::fileClickedHandler(VideobrowserItem& item) noexcept
{
	if (item.video.hasScript) {
		ClickedFilePath = item.video.videoPath;
		EventSystem::PushEvent(VideobrowserEvents::VideobrowserItemClicked);
	}
}

void Videobrowser::ShowBrowser(const char* Id, bool* open) noexcept
{
	uint32_t window_flags = 0;
	window_flags |= ImGuiWindowFlags_NoScrollbar;

	if (open != NULL && !*open) return;
	if (CacheUpdateInProgress) {
		ImGui::Begin(Id, open, window_flags);
		ImGui::TextUnformatted("Updating library... this may take a while.");
		ImGui::NewLine();
		ImGui::Text("Found %ld new videos", Items.size());
		ImGui::SameLine(); OFS::Spinner("it do be spinning doe", ImGui::GetFontSize()/2.f, ImGui::GetFontSize()/4.f, IM_COL32(66, 150, 250, 255));
		ImGui::End();
		return;
	}
	if (CacheNeedsUpdate) { updateLibraryCache(); }

	if (Random)
	{
		SDL_AtomicLock(&ItemsLock);
		Randomizer(Id, open);
		SDL_AtomicUnlock(&ItemsLock);
		return;
	}

	// gamepad control items per row
	if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiInputReadMode_Pressed))
	{
		settings->ItemsPerRow--;
		settings->ItemsPerRow = Util::Clamp(settings->ItemsPerRow, 1, 25);
	}
	else if (ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiInputReadMode_Pressed)) 
	{
		settings->ItemsPerRow++;
		settings->ItemsPerRow = Util::Clamp(settings->ItemsPerRow, 1, 25);
	}
	
	
	window_flags |= ImGuiWindowFlags_MenuBar;
	SDL_AtomicLock(&ItemsLock);
	ImGui::SetNextWindowScroll(ImVec2(0,0)); // prevent scrolling

	ImGui::Begin(Id, open, window_flags);
	auto availSpace = ImGui::GetContentRegionMax();
	auto& style = ImGui::GetStyle();
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu("View")) {
			ImGui::MenuItem("Show thumbnails", NULL, &settings->showThumbnails);
			ImGui::SetNextItemWidth(ImGui::GetFontSize()*5.f);
			ImGui::InputInt("Items", &settings->ItemsPerRow, 1, 10);
			settings->ItemsPerRow = Util::Clamp(settings->ItemsPerRow, 1, 25);
			ImGui::EndMenu();
		}
		ImGui::MenuItem("Settings", NULL, &ShowSettings);
		ImGui::MenuItem("Random", NULL, &Random);
		ImGui::EndMenuBar();
	}

	if (ImGui::Button(ICON_REFRESH)) {
		CacheNeedsUpdate = true;
	}
	ImGui::SameLine();
	ImGui::Bullet();
	ImGui::Text("Videos: %ld", Items.size());
	ImGui::Separator();
	
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("Filter", &Filter);
	
	if (ImGui::BeginTable("##VideobrowserUI", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##TagCol", ImGuiTableColumnFlags_WidthStretch, 0.15);
		ImGui::TableSetupColumn("##ItemsCol", ImGuiTableColumnFlags_WidthStretch, 0.85);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();


		ImGui::BeginChild("Tags");
		auto addTag = [](const std::string& newTag) {
			Tag tag;
			tag.tag = newTag;
			tag.insert();
		};
		static std::string tagBuffer;
		if (ImGui::InputText("##TagInput", &tagBuffer, ImGuiInputTextFlags_EnterReturnsTrue)) { addTag(tagBuffer); }
		ImGui::SameLine();
		if (ImGui::Button("Add##AddTag", ImVec2(-1,0))) { addTag(tagBuffer); }


		if (Tags.size() != Videolibrary::Count<Tag>()) {
			try {
				Tags = Videolibrary::GetAll<Tag>();
			}
			catch (std::system_error& er) {
				LOGF_ERROR("%s", er.what());
			}
		}

		if (ImGui::Button("All##AllTagsButton", ImVec2(-1, 0))) {
			for (auto& t : Tags) { t.FilterActive = false; }
		}
		static std::vector<int64_t> activeFilterTags;
		int activeCount = activeFilterTags.size();
		activeFilterTags.clear();
		for (auto& tagItem : Tags) {
			if (ImGui::Checkbox(tagItem.tag.c_str(), &tagItem.FilterActive)) {}
			if (tagItem.FilterActive) { activeFilterTags.emplace_back(tagItem.id); }
		}

		if (activeCount != activeFilterTags.size()) {
			if (activeFilterTags.size() > 0) {
				auto vids = Videolibrary::GetVideosWithTags(activeFilterTags);
				setVideos(vids);
			}
			else
			{
				// all
				auto vids = Videolibrary::GetAll<Video>();
				setVideos(vids);
			}
		}


		ImGui::EndChild();

		ImGui::TableNextColumn();

		// HACK: there currently doesn't seem to be a way to obtain the column width
		const auto& column = ImGui::GetCurrentContext()->CurrentTable->Columns[ImGui::TableGetColumnIndex()];
		float ItemWidth = ((availSpace.x * column.StretchWeight)
			- style.ScrollbarSize 
			- (3.f * style.ItemInnerSpacing.x)
			- (settings->ItemsPerRow*style.ItemSpacing.x)) / (float)settings->ItemsPerRow;
		ItemWidth = std::max(ItemWidth, 3.f);
		const float ItemHeight = (9.f/16.f)*ItemWidth;
		const auto ItemDim = ImVec2(ItemWidth, ItemHeight);

		ImGui::BeginChild("Items");
		VideobrowserItem* previewItem = nullptr;
		bool itemFocussed = false;
		int index = 0;
		for (auto& item : Items) {
			if (!Filter.empty()) {
				if (!Util::ContainsInsensitive(item.video.videoFilename, Filter)) {
					continue;
				}
			}

			ImColor FileTintColor = item.video.hasScript ? IM_COL32_WHITE : IM_COL32(200, 200, 200, 255);
			if (!item.Focussed) {
				FileTintColor.Value.x *= 0.75f;
				FileTintColor.Value.y *= 0.75f;
				FileTintColor.Value.z *= 0.75f;
			}

			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_PlotLinesHovered]);
			ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_PlotLines]);

			ImGui::PushID(index);
			auto texId = item.texture.GetTexId();
			if (settings->showThumbnails && texId != 0) {
				ImVec2 padding = item.video.hasScript ? ImVec2(0, 0) : ImVec2(ItemWidth*0.1f, ItemWidth*0.1f);
				if(ImGui::ImageButtonEx(ImGui::GetID(item.video.videoPath.c_str()),
					item.Focussed && preview.ready ? (void*)(intptr_t)preview.render_texture : (void*)(intptr_t)texId,
					ItemDim - padding,
					ImVec2(0, 0), ImVec2(1, 1),
					padding/2.f,
					style.Colors[ImGuiCol_PlotLines],
					FileTintColor)) {
					fileClickedHandler(item);
				}
				item.Focussed = (ImGui::IsItemHovered() || (ImGui::IsItemActive() || ImGui::IsItemActivated() || ImGui::IsItemFocused())) && !itemFocussed;
				if (item.Focussed) {
					itemFocussed = true;
					previewItem = &item;
				}
			}
			else {
				if(!item.video.hasScript) { ImGui::PushStyleColor(ImGuiCol_Button, FileTintColor.Value); }
				
				if (ImGui::Button(item.video.videoFilename.c_str(), ItemDim)) {
					fileClickedHandler(item);
				}
				if (settings->showThumbnails && item.video.HasThumbnail() && ImGui::IsItemVisible()) {
					item.GenThumbail();
				}
				if (!item.video.hasScript) { ImGui::PopStyleColor(1); }
			}
			ImGui::PopID();
			ImGui::PopStyleColor(2);
		
			if (ImGui::BeginPopupContextItem() || OFS::GamepadContextMenu())
			{
				if (ImGui::BeginMenu("Add tag...")) {
					for (auto& t : Tags) {
						if (ImGui::MenuItem(t.tag.c_str())) {
							VideoAndTag connect;
							connect.tagId = t.id;
							connect.videoId = item.video.id;
							if (connect.replace())
							{
								// TODO: write tag into funscript
							}
						}
					}
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Remove tag...")) {
					item.UpdateTags();
					for (auto& t : item.AssignedTags) {
						if (ImGui::MenuItem(t.tag.c_str())) {
							Videolibrary::Remove<VideoAndTag>(item.video.id, t.id);
							// TODO: remove tag from funscript
						}
					}
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Explore location...")) {
					auto path = Util::PathFromString(item.video.videoPath);
					path.remove_filename();
					Util::OpenFileExplorer(path.u8string().c_str());
				}
				ImGui::EndPopup();
			}
			else {
				Util::Tooltip(item.video.videoFilename.c_str());
			}

			index++;
			if (index % settings->ItemsPerRow != 0) {
				ImGui::SameLine();
			}
		}

		if (previewItem != nullptr && !preview.loading || previewItem != nullptr && previewItem->texture.Id != previewItemId) {
			previewItemId = previewItem->texture.Id;
			preview.previewVideo(previewItem->video.videoPath, 0.2f);
		}
		else if (previewItem == nullptr && preview.ready) {
			preview.closeVideo();
		}

		ImGui::EndChild();

		ImGui::EndTable();
	}
	
	ImGui::End();
	SDL_AtomicUnlock(&ItemsLock);

	ShowBrowserSettings(&ShowSettings);
}

void Videobrowser::ShowBrowserSettings(bool* open) noexcept
{
	if (open != nullptr && !*open) return;
	
	if (ShowSettings)
		ImGui::OpenPopup(VideobrowserSettingsId);

	if (ImGui::BeginPopupModal(VideobrowserSettingsId, open, ImGuiWindowFlags_None)) {

		if (ImGui::BeginTable("##SearchPaths", 3, ImGuiTableFlags_Borders)) {
			ImGui::TableSetupColumn("Path");
			//ImGui::TableSetupColumn("Recursive");
			//ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();
			int32_t index = 0;
			for (index = 0; index < settings->SearchPaths.size(); index++) {
				ImGui::PushID(index);
				auto& search = settings->SearchPaths[index];
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(search.path.c_str()); 
				Util::Tooltip(search.path.c_str());

				ImGui::TableNextColumn();
				ImGui::Checkbox("Recursive", &search.recursive);

				ImGui::TableNextColumn();
				if (ImGui::Button("Remove", ImVec2(-1, 0))) {
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			if (index < settings->SearchPaths.size()) {
				settings->SearchPaths.erase(settings->SearchPaths.begin() + index);
			}
			ImGui::EndTable();
		}

		if (ImGui::Button("Choose path", ImVec2(-1, 0)))
		{
			Util::OpenDirectoryDialog("Choose search path", "", [&](auto& result) {
				if (result.files.size() > 0) {
					VideobrowserSettings::LibraryPath path;
					path.path = result.files.front();
					path.recursive = false;
					settings->SearchPaths.emplace_back(std::move(path));
				}
			});
		}

		ImGui::EndPopup();
	}
}

int32_t VideobrowserEvents::VideobrowserItemClicked = 0;

void VideobrowserEvents::RegisterEvents() noexcept
{
	VideobrowserItemClicked = SDL_RegisterEvents(1);
}
