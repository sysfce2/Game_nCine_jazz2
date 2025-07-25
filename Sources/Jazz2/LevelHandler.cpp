﻿#include "LevelHandler.h"
#include "ContentResolver.h"
#include "PreferencesCache.h"
#include "Rendering/PlayerViewport.h"
#include "UI/DiscordRpcClient.h"
#include "UI/HUD.h"
#include "UI/InGameConsole.h"
#include "UI/Menu/InGameMenu.h"
#include "../Main.h"

#if defined(WITH_ANGELSCRIPT)
#	include "Scripting/LevelScriptLoader.h"
#endif

#include "../nCine/I18n.h"
#include "../nCine/MainApplication.h"
#include "../nCine/ServiceLocator.h"
#include "../nCine/tracy.h"
#include "../nCine/Base/Random.h"
#include "../nCine/Graphics/Camera.h"
#include "../nCine/Graphics/Texture.h"
#include "../nCine/Graphics/Viewport.h"
#include "../nCine/Input/JoyMapping.h"

#include "Actors/Player.h"
#include "Actors/SolidObjectBase.h"
#include "Actors/Enemies/Bosses/BossBase.h"
#include "Actors/Environment/IceBlock.h"

#include <float.h>

#include <Containers/StaticArray.h>
#include <Containers/StringConcatenable.h>
#include <Utf8.h>

using namespace nCine;
using namespace Jazz2::Tiles;

namespace Jazz2
{
	namespace Resources
	{
		static constexpr AnimState Snow = (AnimState)0;
		static constexpr AnimState Rain = (AnimState)1;
	}

	using namespace Jazz2::Resources;

#if defined(WITH_AUDIO)
	class AudioBufferPlayerForSplitscreen : public AudioBufferPlayer
	{
		DEATH_RUNTIME_OBJECT(AudioBufferPlayer);

	public:
		explicit AudioBufferPlayerForSplitscreen(AudioBuffer* audioBuffer, ArrayView<std::unique_ptr<Rendering::PlayerViewport>> viewports);

		Vector3f getAdjustedPosition(IAudioDevice& device, const Vector3f& pos, bool isSourceRelative, bool isAs2D) override;

		void updatePosition();
		void updateViewports(ArrayView<std::unique_ptr<Rendering::PlayerViewport>> viewports);

	private:
		ArrayView<std::unique_ptr<Rendering::PlayerViewport>> _viewports;
	};

	AudioBufferPlayerForSplitscreen::AudioBufferPlayerForSplitscreen(AudioBuffer* audioBuffer, ArrayView<std::unique_ptr<Rendering::PlayerViewport>> viewports)
		: AudioBufferPlayer(audioBuffer), _viewports(viewports)
	{
	}

	Vector3f AudioBufferPlayerForSplitscreen::getAdjustedPosition(IAudioDevice& device, const Vector3f& pos, bool isSourceRelative, bool isAs2D)
	{
		if (isSourceRelative || isAs2D) {
			return AudioBufferPlayer::getAdjustedPosition(device, pos, isSourceRelative, isAs2D);
		}

		std::size_t minIndex = 0;
		float minDistance = FLT_MAX;

		for (std::size_t i = 0; i < _viewports.size(); i++) {
			float distance = (pos.ToVector2() - _viewports[i]->_cameraPos).SqrLength();
			if (minDistance > distance) {
				minDistance = distance;
				minIndex = i;
			}
		}

		Vector3f relativePos = (pos - Vector3f(_viewports[minIndex]->_cameraPos, 0.0f));
		return AudioBufferPlayer::getAdjustedPosition(device, relativePos, false, false);
	}

	void AudioBufferPlayerForSplitscreen::updatePosition()
	{
		if (state_ != PlayerState::Playing || GetFlags(PlayerFlags::SourceRelative) || GetFlags(PlayerFlags::As2D)) {
			return;
		}

		IAudioDevice& device = theServiceLocator().GetAudioDevice();
		setPositionInternal(getAdjustedPosition(device, position_, false, false));
	}

	void AudioBufferPlayerForSplitscreen::updateViewports(ArrayView<std::unique_ptr<Rendering::PlayerViewport>> viewports)
	{
		_viewports = viewports;
	}
#endif

	LevelHandler::LevelHandler(IRootController* root)
		: _root(root), _lightingShader(nullptr), _blurShader(nullptr), _downsampleShader(nullptr), _combineShader(nullptr),
			_combineWithWaterShader(nullptr), _eventSpawner(this), _difficulty(GameDifficulty::Default), _isReforged(false),
			_cheatsUsed(false), _checkpointCreated(false), _nextLevelType(ExitType::None),
			_nextLevelTime(0.0f), _elapsedMillisecondsBegin(0), _elapsedFrames(0.0f), _checkpointFrames(0.0f),
			_waterLevel(FLT_MAX), _weatherType(WeatherType::None), _pressedKeys(ValueInit, (std::size_t)Keys::Count),
			_overrideActions(0)
	{
	}

	LevelHandler::~LevelHandler()
	{
		_players.clear();

		// Remove nodes from UpscaleRenderPass
		for (auto& viewport : _assignedViewports) {
			viewport->_combineRenderer->setParent(nullptr);
		}
		_hud->setParent(nullptr);
		_console->setParent(nullptr);

		TracyPlot("Actors", 0LL);
	}

	bool LevelHandler::Initialize(const LevelInitialization& levelInit)
	{
		ZoneScopedC(0x4876AF);

		_levelName = levelInit.LevelName;
		_difficulty = levelInit.Difficulty;
		_isReforged = levelInit.IsReforged;
		_cheatsUsed = levelInit.CheatsUsed;
		_elapsedMillisecondsBegin = levelInit.ElapsedMilliseconds;

		auto& resolver = ContentResolver::Get();
		resolver.BeginLoading();

		_noiseTexture = resolver.GetNoiseTexture();

		_rootNode = std::make_unique<SceneNode>();
		_rootNode->setVisitOrderState(SceneNode::VisitOrderState::Disabled);

		_console = std::make_unique<UI::InGameConsole>(this);

		auto p = _levelName.partition('/');

		// Try to search also "unknown" directory
		LevelDescriptor descriptor;
		if (!resolver.TryLoadLevel(_levelName, _difficulty, descriptor) &&
			(p[0] == "unknown"_s || !resolver.TryLoadLevel(String("unknown/"_s + p[2]), _difficulty, descriptor))) {
			LOGE("Cannot load level \"{}\"", _levelName);
			return false;
		}

		_console->WriteLine(UI::MessageLevel::Debug, _f("Level \"{}\" initialized", descriptor.DisplayName));

		AttachComponents(std::move(descriptor));
		SpawnPlayers(levelInit);		

		OnInitialized();
		resolver.EndLoading();

		return true;
	}

	bool LevelHandler::Initialize(Stream& src, std::uint16_t version)
	{
		ZoneScopedC(0x4876AF);

		std::uint8_t flags = src.ReadValue<std::uint8_t>();

		std::uint8_t stringSize = src.ReadValue<std::uint8_t>();
		String episodeName(NoInit, stringSize);
		src.Read(episodeName.data(), stringSize);

		stringSize = src.ReadValue<std::uint8_t>();
		String levelFileName(NoInit, stringSize);
		src.Read(levelFileName.data(), stringSize);

		_levelName = episodeName + '/' + levelFileName;

		_difficulty = (GameDifficulty)src.ReadValue<std::uint8_t>();
		_isReforged = (flags & 0x01) != 0;
		_cheatsUsed = (flags & 0x02) != 0;
		if (version >= 3) {
			_elapsedMillisecondsBegin = src.ReadVariableUint64();
		}
		_checkpointFrames = src.ReadValue<float>();

		auto& resolver = ContentResolver::Get();
		resolver.BeginLoading();

		_noiseTexture = resolver.GetNoiseTexture();

		_rootNode = std::make_unique<SceneNode>();
		_rootNode->setVisitOrderState(SceneNode::VisitOrderState::Disabled);

		_console = std::make_unique<UI::InGameConsole>(this);

		LevelDescriptor descriptor;
		if (!resolver.TryLoadLevel(_levelName, _difficulty, descriptor)) {
			LOGE("Cannot load level \"{}\"", _levelName);
			return false;
		}

		_console->WriteLine(UI::MessageLevel::Debug, _f("Level \"{}\" initialized", descriptor.DisplayName));

		AttachComponents(std::move(descriptor));

		// All components are ready, deserialize the rest of state
		_waterLevel = src.ReadValue<float>();
		_weatherType = (WeatherType)src.ReadValue<std::uint8_t>();
		_weatherIntensity = src.ReadValue<std::uint8_t>();

		_tileMap->InitializeFromStream(src);
		_eventMap->InitializeFromStream(src);

		std::uint32_t playerCount = src.ReadValue<std::uint8_t>();
		_players.reserve(playerCount);

		for (std::uint32_t i = 0; i < playerCount; i++) {
			std::shared_ptr<Actors::Player> player = std::make_shared<Actors::Player>();
			player->InitializeFromStream(this, src, version);

			Actors::Player* ptr = player.get();
			_players.push_back(ptr);
			AddActor(player);
			AssignViewport(ptr);
		}

		_hud = CreateHUD();
		_hud->BeginFadeIn(false);

		OnInitialized();
		resolver.EndLoading();

		// Set it at the end, so ambient light transition is skipped
		_elapsedFrames = _checkpointFrames;

		return true;
	}

	void LevelHandler::OnInitialized()
	{
		auto& resolver = ContentResolver::Get();
		_commonResources = resolver.RequestMetadata("Common/Scenery"_s);
		resolver.PreloadMetadataAsync("Common/Explosions"_s);

		_eventMap->PreloadEventsAsync();

		InitializeRumbleEffects();
		UpdateRichPresence();

		_console->OnInitialized();

#if defined(WITH_ANGELSCRIPT)
		if (_scripts != nullptr) {
			_scripts->OnLevelLoad();
		}
#endif
	}

	Events::EventSpawner* LevelHandler::EventSpawner()
	{
		return &_eventSpawner;
	}

	Events::EventMap* LevelHandler::EventMap()
	{
		return _eventMap.get();
	}

	Tiles::TileMap* LevelHandler::TileMap()
	{
		return _tileMap.get();
	}

	GameDifficulty LevelHandler::GetDifficulty() const
	{
		return _difficulty;
	}

	bool LevelHandler::IsLocalSession() const
	{
		return true;
	}

	bool LevelHandler::IsServer() const
	{
		return true;
	}

	bool LevelHandler::IsPausable() const
	{
		return true;
	}

	bool LevelHandler::IsReforged() const
	{
		return _isReforged;
	}

	bool LevelHandler::CanActivateSugarRush() const
	{
		return true;
	}

	bool LevelHandler::CanEventDisappear(EventType eventType) const
	{
		return true;
	}

	bool LevelHandler::CanPlayersCollide() const
	{
		// TODO
		return false;
	}

	Recti LevelHandler::GetLevelBounds() const
	{
		return _levelBounds;
	}

	float LevelHandler::GetElapsedFrames() const
	{
		return _elapsedFrames;
	}

	float LevelHandler::GetGravity() const
	{
		constexpr float DefaultGravity = 0.3f;

		// Higher gravity in Reforged mode
		return (_isReforged ? DefaultGravity : DefaultGravity * 0.8f);
	}

	float LevelHandler::GetWaterLevel() const
	{
		return _waterLevel;
	}

	float LevelHandler::GetHurtInvulnerableTime() const
	{
		return 180.0f;
	}

	ArrayView<const std::shared_ptr<Actors::ActorBase>> LevelHandler::GetActors() const
	{
		return _actors;
	}

	ArrayView<Actors::Player* const> LevelHandler::GetPlayers() const
	{
		return _players;
	}

	float LevelHandler::GetDefaultAmbientLight() const
	{
		return _defaultAmbientLight.W;
	}

	float LevelHandler::GetAmbientLight(Actors::Player* player) const
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				return viewport->_ambientLightTarget;
			}
		}
		return 0.0f;
	}

	void LevelHandler::SetAmbientLight(Actors::Player* player, float value)
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				viewport->_ambientLightTarget = value;

				// Skip transition if it was changed at the beginning of level
				if (_elapsedFrames < FrameTimer::FramesPerSecond * 0.25f) {
					viewport->_ambientLight.W = value;
				}
			}
		}
	}

	void LevelHandler::InvokeAsync(Function<void()>&& callback)
	{
		_root->InvokeAsync(weak_from_this(), std::move(callback));
	}

	void LevelHandler::AttachComponents(LevelDescriptor&& descriptor)
	{
		ZoneScopedC(0x4876AF);

		_levelDisplayName = std::move(descriptor.DisplayName);

		LOGI("Level \"{}\" (\"{}.j2l\") loaded", _levelDisplayName, _levelName);

		if (!_levelDisplayName.empty()) {
			theApplication().GetGfxDevice().setWindowTitle(String(NCINE_APP_NAME " - " + _levelDisplayName));
		} else {
			theApplication().GetGfxDevice().setWindowTitle(NCINE_APP_NAME);
		}

		_defaultNextLevel = std::move(descriptor.NextLevel);
		_defaultSecretLevel = std::move(descriptor.SecretLevel);

		_tileMap = std::move(descriptor.TileMap);
		_tileMap->SetOwner(this);
		_tileMap->setParent(_rootNode.get());

		_eventMap = std::move(descriptor.EventMap);
		_eventMap->SetLevelHandler(this);

		Vector2i levelBounds = _tileMap->GetLevelBounds();
		_levelBounds = Recti(0, 0, levelBounds.X, levelBounds.Y);
		_viewBoundsTarget = _levelBounds.As<float>();

		_defaultAmbientLight = descriptor.AmbientColor;

		_weatherType = descriptor.Weather;
		_weatherIntensity = descriptor.WeatherIntensity;
		_waterLevel = descriptor.WaterLevel;

		_musicCurrentPath = std::move(descriptor.MusicPath);
		_musicDefaultPath = _musicCurrentPath;

#if defined(WITH_AUDIO)
		if (!_musicCurrentPath.empty()) {
			_music = ContentResolver::Get().GetMusic(_musicCurrentPath);
			if (_music != nullptr) {
				_music->setLooping(true);
				_music->setGain(PreferencesCache::MasterVolume * PreferencesCache::MusicVolume);
				_music->setSourceRelative(true);
				_music->play();
			}
		}
#endif

		_levelTexts = std::move(descriptor.LevelTexts);

#if defined(WITH_ANGELSCRIPT) || defined(DEATH_TRACE)
		// TODO: Allow script signing
		if (PreferencesCache::AllowUnsignedScripts) {
			const StringView foundDot = descriptor.FullPath.findLastOr('.', descriptor.FullPath.end());
			String scriptPath = (foundDot == descriptor.FullPath.end() ? StringView(descriptor.FullPath) : descriptor.FullPath.prefix(foundDot.begin())) + ".j2as"_s;
			if (auto scriptPathCaseInsensitive = fs::FindPathCaseInsensitive(scriptPath)) {
				if (fs::IsReadableFile(scriptPathCaseInsensitive)) {
#	if defined(WITH_ANGELSCRIPT)
					_scripts = std::make_unique<Scripting::LevelScriptLoader>(this, scriptPathCaseInsensitive);
#	else
					LOGW("Level requires scripting, but scripting support is disabled in this build");
#	endif
				}
			}
		}
#endif
	}

	std::unique_ptr<UI::HUD> LevelHandler::CreateHUD()
	{
		return std::make_unique<UI::HUD>(this);
	}

	void LevelHandler::SpawnPlayers(const LevelInitialization& levelInit)
	{
		std::int32_t playerCount = levelInit.GetPlayerCount();

		for (std::int32_t i = 0; i < std::int32_t(arraySize(levelInit.PlayerCarryOvers)); i++) {
			if (levelInit.PlayerCarryOvers[i].Type == PlayerType::None) {
				continue;
			}

			Vector2 spawnPosition = _eventMap->GetSpawnPosition(levelInit.PlayerCarryOvers[i].Type);
			if (spawnPosition.X < 0.0f && spawnPosition.Y < 0.0f) {
				spawnPosition = _eventMap->GetSpawnPosition(PlayerType::Jazz);
				if (spawnPosition.X < 0.0f && spawnPosition.Y < 0.0f) {
					continue;
				}
			}

			std::shared_ptr<Actors::Player> player = std::make_shared<Actors::Player>();
			std::uint8_t playerParams[2] = { (std::uint8_t)levelInit.PlayerCarryOvers[i].Type, (std::uint8_t)i };
			player->OnActivated(Actors::ActorActivationDetails(
				this,
				Vector3i((std::int32_t)spawnPosition.X + (i * 10) - ((playerCount - 1) * 5), (std::int32_t)spawnPosition.Y - (i * 20) + ((playerCount - 1) * 5), PlayerZ - i),
				playerParams
			));

			Actors::Player* ptr = player.get();
			_players.push_back(ptr);
			AddActor(player);
			AssignViewport(ptr);

			ptr->ReceiveLevelCarryOver(levelInit.LastExitType, levelInit.PlayerCarryOvers[i]);
		}

		_hud = CreateHUD();
		_hud->BeginFadeIn((levelInit.LastExitType & ExitType::FastTransition) == ExitType::FastTransition);
	}

	bool LevelHandler::IsCheatingAllowed()
	{
		return PreferencesCache::AllowCheats;
	}

	Vector2i LevelHandler::GetViewSize() const
	{
		return _viewSize;
	}

	void LevelHandler::OnBeginFrame()
	{
		ZoneScopedC(0x4876AF);

		float timeMult = theApplication().GetTimeMult();

		if (_pauseMenu == nullptr) {
			UpdatePressedActions();

			bool isGamepad;
			if (PlayerActionHit(nullptr, PlayerAction::Menu)) {
				if (_console->IsVisible()) {
					_console->Hide();
				} else if (_nextLevelType == ExitType::None) {
					PauseGame();
				}
			} else if (PlayerActionHit(nullptr, PlayerAction::Console, true, isGamepad)) {
				if (_console->IsVisible()) {
					if (isGamepad) {
						_console->Hide();
					}
				} else {
					_console->Show();
				}
			}
#if defined(DEATH_DEBUG)
			if (IsCheatingAllowed() && PlayerActionPressed(nullptr, PlayerAction::ChangeWeapon) && PlayerActionHit(0, PlayerAction::Jump)) {
				_cheatsUsed = true;
				BeginLevelChange(nullptr, ExitType::Warp | ExitType::FastTransition);
			}
#endif
		}

#if defined(WITH_AUDIO)
		// Destroy stopped players and resume music after Sugar Rush
		if (_sugarRushMusic != nullptr && _sugarRushMusic->isStopped()) {
			_sugarRushMusic = nullptr;
			if (_music != nullptr) {
				_music->play();
			}
		}

		auto it = _playingSounds.begin();
		while (it != _playingSounds.end()) {
			if ((*it)->isStopped()) {
				it = _playingSounds.eraseUnordered(it);
				continue;
			}
			++it;
		}
#endif

		if (!IsPausable() || _pauseMenu == nullptr) {
			if (_nextLevelType != ExitType::None) {
				_nextLevelTime -= timeMult;
				ProcessQueuedNextLevel();
			}

			ProcessEvents(timeMult);
			ProcessWeather(timeMult);

			// Active Boss
			if (_activeBoss != nullptr && _activeBoss->GetHealth() <= 0) {
				_activeBoss = nullptr;
				BeginLevelChange(nullptr, ExitType::Boss);
			}

#if defined(WITH_ANGELSCRIPT)
			if (_scripts != nullptr) {
				_scripts->OnLevelUpdate(timeMult);
			}
#endif
		}
	}

	void LevelHandler::OnEndFrame()
	{
		ZoneScopedC(0x4876AF);

		float timeMult = theApplication().GetTimeMult();
		auto& resolver = ContentResolver::Get();

		_tileMap->OnEndFrame();

		if (!IsPausable() || _pauseMenu == nullptr) {
			ResolveCollisions(timeMult);

			if (!resolver.IsHeadless()) {
#if defined(NCINE_HAS_GAMEPAD_RUMBLE)
				_rumble.OnEndFrame(timeMult);
#endif

				for (auto& viewport : _assignedViewports) {
					viewport->UpdateCamera(timeMult);
				}

#if defined(WITH_AUDIO)
				if (!_assignedViewports.empty()) {
					// Update audio listener position
					IAudioDevice& audioDevice = theServiceLocator().GetAudioDevice();
					if (_assignedViewports.size() == 1) {
						audioDevice.updateListener(Vector3f(_assignedViewports[0]->_cameraPos, 0.0f),
							Vector3f(_assignedViewports[0]->_targetActor->GetSpeed(), 0.0f));
					} else {
						audioDevice.updateListener(Vector3f::Zero, Vector3f::Zero);

						// All audio players must be updated to the nearest listener
						for (auto& current : _playingSounds) {
							if (auto* currentForSplitscreen = runtime_cast<AudioBufferPlayerForSplitscreen>(current.get())) {
								currentForSplitscreen->updatePosition();
							}
						}
					}
				}
#endif
			}

			_elapsedFrames += timeMult;
		}

		if (!resolver.IsHeadless()) {
			for (auto& viewport : _assignedViewports) {
				viewport->OnEndFrame();
			}

#if defined(DEATH_DEBUG) && defined(WITH_IMGUI)
			if (PreferencesCache::ShowPerformanceMetrics) {
				ImDrawList* drawList = ImGui::GetBackgroundDrawList();

				std::size_t actorsCount = _actors.size();
				for (std::size_t i = 0; i < actorsCount; i++) {
					auto* actor = _actors[i].get();

					auto pos = WorldPosToScreenSpace(actor->_pos);
					auto aabbMin = WorldPosToScreenSpace({ actor->AABB.L, actor->AABB.T });
					auto aabbMax = WorldPosToScreenSpace({ actor->AABB.R, actor->AABB.B });
					auto aabbInnerMin = WorldPosToScreenSpace({ actor->AABBInner.L, actor->AABBInner.T });
					auto aabbInnerMax = WorldPosToScreenSpace({ actor->AABBInner.R, actor->AABBInner.B });

					drawList->AddRect(ImVec2(pos.x - 2.4f, pos.y - 2.4f), ImVec2(pos.x + 2.4f, pos.y + 2.4f), ImColor(0, 0, 0, 220));
					drawList->AddRect(ImVec2(pos.x - 1.0f, pos.y - 1.0f), ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImColor(120, 255, 200, 220));
					drawList->AddRect(aabbMin, aabbMax, ImColor(120, 200, 255, 180));
					drawList->AddRect(aabbInnerMin, aabbInnerMax, ImColor(255, 255, 255));
				}
			}
#endif
		}

		TracyPlot("Actors", static_cast<std::int64_t>(_actors.size()));
	}

	void LevelHandler::OnInitializeViewport(std::int32_t width, std::int32_t height)
	{
		ZoneScopedC(0x4876AF);

		auto& resolver = ContentResolver::Get();
		if (resolver.IsHeadless()) {
			// Use only the main viewport in headless mode
			_rootNode->setParent(&theApplication().GetRootNode());
			return;
		}

		constexpr float defaultRatio = (float)DefaultWidth / DefaultHeight;
		float currentRatio = (float)width / height;

		std::int32_t w, h;
		if (currentRatio > defaultRatio) {
			w = std::min(DefaultWidth, width);
			h = (std::int32_t)roundf(w / currentRatio);
		} else if (currentRatio < defaultRatio) {
			h = std::min(DefaultHeight, height);
			w = (std::int32_t)roundf(h * currentRatio);
		} else {
			w = std::min(DefaultWidth, width);
			h = std::min(DefaultHeight, height);
		}

		_viewSize = Vector2i(w, h);
		_upscalePass.Initialize(w, h, width, height);

		bool notInitialized = (_combineShader == nullptr);
		if (notInitialized) {
			LOGI("Acquiring required shaders");

			_lightingShader = resolver.GetShader(PrecompiledShader::Lighting);
			if (_lightingShader == nullptr) { LOGW("PrecompiledShader::Lighting failed"); }
			_blurShader = resolver.GetShader(PrecompiledShader::Blur);
			if (_blurShader == nullptr) { LOGW("PrecompiledShader::Blur failed"); }
			_downsampleShader = resolver.GetShader(PrecompiledShader::Downsample);
			if (_downsampleShader == nullptr) { LOGW("PrecompiledShader::Downsample failed"); }
			_combineShader = resolver.GetShader(PrecompiledShader::Combine);
			if (_combineShader == nullptr) { LOGW("PrecompiledShader::Combine failed"); }

			if (_hud != nullptr) {
				_hud->setParent(_upscalePass.GetNode());
			}
			if (_console != nullptr) {
				_console->setParent(_upscalePass.GetNode());
			}
		}

		_combineWithWaterShader = resolver.GetShader(PreferencesCache::LowWaterQuality
			? PrecompiledShader::CombineWithWaterLow
			: PrecompiledShader::CombineWithWater);
		if (_combineWithWaterShader == nullptr) {
			if (PreferencesCache::LowWaterQuality) {
				LOGW("PrecompiledShader::CombineWithWaterLow failed");
			} else {
				LOGW("PrecompiledShader::CombineWithWater failed");
			}
		}

		bool useHalfRes = (PreferencesCache::PreferZoomOut && _assignedViewports.size() >= 3);

		for (std::size_t i = 0; i < _assignedViewports.size(); i++) {
			Rendering::PlayerViewport& viewport = *_assignedViewports[i];
			Recti bounds = GetPlayerViewportBounds(w, h, (std::int32_t)i);
			if (viewport.Initialize(_rootNode.get(), _upscalePass.GetNode(), bounds, useHalfRes)) {
				InitializeCamera(viewport);
			}
		}

		// Viewports must be registered in reverse order
		_upscalePass.Register();

		for (std::size_t i = 0; i < _assignedViewports.size(); i++) {
			Rendering::PlayerViewport& viewport = *_assignedViewports[i];
			viewport.Register();			

			if (_pauseMenu != nullptr) {
				viewport.UpdateCamera(0.0f);	// Force update camera if game is paused
			}
		}

		if (_tileMap != nullptr) {
			_tileMap->OnInitializeViewport();
		}

		if (_pauseMenu != nullptr) {
			_pauseMenu->OnInitializeViewport(_viewSize.X, _viewSize.Y);
		}
	}

	bool LevelHandler::OnConsoleCommand(StringView line)
	{
		if (line == "/help"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			_console->WriteLine(UI::MessageLevel::Confirm, _("For more information, visit the official website:") + " \f[w:80]\f[c:#707070]https://deat.tk/jazz2/help\f[/c]\f[/w]"_s);
			return true;
		} else if (line == "jjk"_s || line == "jjkill"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatKill();
		} else if (line == "jjgod"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatGod();
		} else if (line == "jjnext"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatNext();
		} else if (line == "jjguns"_s || line == "jjammo"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatGuns();
		} else if (line == "jjrush"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatRush();
		} else if (line == "jjgems"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatGems();
		} else if (line == "jjbird"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatBird();
		} else if (line == "jjlife"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatLife();
		} else if (line == "jjpower"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatPower();
		} else if (line == "jjcoins"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatCoins();
		} else if (line == "jjmorph"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatMorph();
		} else if (line == "jjshield"_s) {
			_console->WriteLine(UI::MessageLevel::Echo, line);
			return CheatShield();
		} else {
			return false;
		}
	}

	void LevelHandler::OnKeyPressed(const KeyboardEvent& event)
	{
		_pressedKeys.set((std::size_t)event.sym);

		if (_pauseMenu != nullptr) {
			_pauseMenu->OnKeyPressed(event);
		} else if (_console->IsVisible()) {
			_console->OnKeyPressed(event);
		}
	}

	void LevelHandler::OnKeyReleased(const KeyboardEvent& event)
	{
		_pressedKeys.reset((std::size_t)event.sym);

		if (_pauseMenu != nullptr) {
			_pauseMenu->OnKeyReleased(event);
		}
	}

	void LevelHandler::OnTextInput(const TextInputEvent& event)
	{
		if (_console->IsVisible()) {
			_console->OnTextInput(event);
		}
	}

	void LevelHandler::OnTouchEvent(const  TouchEvent& event)
	{
		if (_pauseMenu != nullptr) {
			_pauseMenu->OnTouchEvent(event);
		} else {
			_hud->OnTouchEvent(event, _overrideActions);
		}
	}

	void LevelHandler::AddActor(std::shared_ptr<Actors::ActorBase> actor)
	{
		actor->SetParent(_rootNode.get());

		if (!actor->GetState(Actors::ActorState::ForceDisableCollisions)) {
			actor->UpdateAABB();
			actor->_collisionProxyID = _collisions.CreateProxy(actor->AABB, actor.get());
		}

		_actors.push_back(std::move(actor));
	}

	std::shared_ptr<AudioBufferPlayer> LevelHandler::PlaySfx(Actors::ActorBase* self, StringView identifier, AudioBuffer* buffer, const Vector3f& pos, bool sourceRelative, float gain, float pitch)
	{
#if defined(WITH_AUDIO)
		if (buffer != nullptr) {
			auto& player = _playingSounds.emplace_back(_assignedViewports.size() > 1
				? std::make_shared<AudioBufferPlayerForSplitscreen>(buffer, _assignedViewports)
				: std::make_shared<AudioBufferPlayer>(buffer));
			player->setPosition(Vector3f(pos.X, pos.Y, 100.0f));
			player->setGain(gain * PreferencesCache::MasterVolume * PreferencesCache::SfxVolume);
			player->setSourceRelative(sourceRelative);

			if (pos.Y >= _waterLevel) {
				player->setLowPass(0.05f);
				player->setPitch(pitch * 0.7f);
			} else {
				player->setPitch(pitch);
			}

			player->play();
			return player;
		}
#endif
		return nullptr;
	}

	std::shared_ptr<AudioBufferPlayer> LevelHandler::PlayCommonSfx(StringView identifier, const Vector3f& pos, float gain, float pitch)
	{
#if defined(WITH_AUDIO)
		auto it = _commonResources->Sounds.find(String::nullTerminatedView(identifier));
		if (it != _commonResources->Sounds.end() && !it->second.Buffers.empty()) {
			std::int32_t idx = (it->second.Buffers.size() > 1 ? Random().Next(0, (std::int32_t)it->second.Buffers.size()) : 0);
			auto* buffer = &it->second.Buffers[idx]->Buffer;
			auto& player = _playingSounds.emplace_back(_assignedViewports.size() > 1
				? std::make_shared<AudioBufferPlayerForSplitscreen>(buffer, _assignedViewports)
				: std::make_shared<AudioBufferPlayer>(buffer));
			player->setPosition(Vector3f(pos.X, pos.Y, 100.0f));
			player->setGain(gain * PreferencesCache::MasterVolume * PreferencesCache::SfxVolume);

			if (pos.Y >= _waterLevel) {
				player->setLowPass(0.05f);
				player->setPitch(pitch * 0.7f);
			} else {
				player->setPitch(pitch);
			}

			player->play();
			return player;
		}	
#endif
		return nullptr;
	}

	void LevelHandler::WarpCameraToTarget(Actors::ActorBase* actor, bool fast)
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == actor) {
				viewport->WarpCameraToTarget(fast);
			}
		}
	}

	bool LevelHandler::IsPositionEmpty(Actors::ActorBase* self, const AABBf& aabb, TileCollisionParams& params, Actors::ActorBase** collider)
	{
		*collider = nullptr;

		if (self->GetState(Actors::ActorState::CollideWithTileset)) {
			if (_tileMap != nullptr) {
				if (self->GetState(Actors::ActorState::CollideWithTilesetReduced) && aabb.B - aabb.T >= 20.0f) {
					// If hitbox height is larger than 20px, check bottom and top separately (and top only if going upwards)
					AABBf aabbTop = aabb;
					aabbTop.B = aabbTop.T + 6.0f;
					AABBf aabbBottom = aabb;
					aabbBottom.T = aabbBottom.B - std::max(14.0f, (aabb.B - aabb.T) - 10.0f);
					if (!_tileMap->IsTileEmpty(aabbBottom, params)) {
						return false;
					}
					if (!params.Downwards) {
						params.Downwards = false;
						if (!_tileMap->IsTileEmpty(aabbTop, params)) {
							return false;
						}
					}
				} else {
					if (!_tileMap->IsTileEmpty(aabb, params)) {
						return false;
					}
				}
			}
		}

		// Check for solid objects
		if (self->GetState(Actors::ActorState::CollideWithSolidObjects)) {
			Actors::ActorBase* colliderActor = nullptr;
			FindCollisionActorsByAABB(self, aabb, [self, &colliderActor, &params](Actors::ActorBase* actor) -> bool {
				if ((actor->GetState() & (Actors::ActorState::IsSolidObject | Actors::ActorState::IsDestroyed)) != Actors::ActorState::IsSolidObject) {
					return true;
				}
				if (self->GetState(Actors::ActorState::ExcludeSimilar) && actor->GetState(Actors::ActorState::ExcludeSimilar)) {
					// If both objects have ExcludeSimilar, ignore it
					return true;
				}
				if (self->GetState(Actors::ActorState::CollideWithSolidObjectsBelow) &&
					self->AABBInner.B > (actor->AABBInner.T + actor->AABBInner.B) * 0.5f) {
					return true;
				}

				auto* solidObject = runtime_cast<Actors::SolidObjectBase>(actor);
				if (solidObject == nullptr || !solidObject->IsOneWay || params.Downwards) {
					std::shared_ptr selfShared = self->shared_from_this();
					std::shared_ptr actorShared = actor->shared_from_this();
					if (!selfShared->OnHandleCollision(actorShared) && !actorShared->OnHandleCollision(selfShared)) {
						colliderActor = actor;
						return false;
					}
				}

				return true;
			});

			*collider = colliderActor;
		}

		return (*collider == nullptr);
	}

	void LevelHandler::FindCollisionActorsByAABB(const Actors::ActorBase* self, const AABBf& aabb, Function<bool(Actors::ActorBase*)>&& callback)
	{
		struct QueryHelper {
			const LevelHandler* Handler;
			const Actors::ActorBase* Self;
			const AABBf& AABB;
			Function<bool(Actors::ActorBase*)>& Callback;

			bool OnCollisionQuery(std::int32_t nodeId) {
				Actors::ActorBase* actor = (Actors::ActorBase*)Handler->_collisions.GetUserData(nodeId);
				if (Self == actor || (actor->GetState() & (Actors::ActorState::CollideWithOtherActors | Actors::ActorState::IsDestroyed)) != Actors::ActorState::CollideWithOtherActors) {
					return true;
				}
				if (actor->IsCollidingWith(AABB)) {
					return Callback(actor);
				}
				return true;
			}
		};

		QueryHelper helper = { this, self, aabb, callback };
		_collisions.Query(&helper, aabb);
	}

	void LevelHandler::FindCollisionActorsByRadius(float x, float y, float radius, Function<bool(Actors::ActorBase*)>&& callback)
	{
		AABBf aabb = AABBf(x - radius, y - radius, x + radius, y + radius);
		float radiusSquared = (radius * radius);

		struct QueryHelper {
			const LevelHandler* Handler;
			const float x, y;
			const float RadiusSquared;
			Function<bool(Actors::ActorBase*)>& Callback;

			bool OnCollisionQuery(std::int32_t nodeId) {
				Actors::ActorBase* actor = (Actors::ActorBase*)Handler->_collisions.GetUserData(nodeId);
				if ((actor->GetState() & (Actors::ActorState::CollideWithOtherActors | Actors::ActorState::IsDestroyed)) != Actors::ActorState::CollideWithOtherActors) {
					return true;
				}

				// Find the closest point to the circle within the rectangle
				float closestX = std::clamp(x, actor->AABB.L, actor->AABB.R);
				float closestY = std::clamp(y, actor->AABB.T, actor->AABB.B);

				// Calculate the distance between the circle's center and this closest point
				float distanceX = (x - closestX);
				float distanceY = (y - closestY);

				// If the distance is less than the circle's radius, an intersection occurs
				float distanceSquared = (distanceX * distanceX) + (distanceY * distanceY);
				if (distanceSquared < RadiusSquared) {
					return Callback(actor);
				}

				return true;
			}
		};

		QueryHelper helper = { this, x, y, radiusSquared, callback };
		_collisions.Query(&helper, aabb);
	}

	void LevelHandler::GetCollidingPlayers(const AABBf& aabb, Function<bool(Actors::ActorBase*)>&& callback)
	{
		for (auto& player : _players) {
			if (aabb.Overlaps(player->AABB)) {
				if (!callback(player)) {
					break;
				}
			}
		}
	}

	void LevelHandler::BroadcastTriggeredEvent(Actors::ActorBase* initiator, EventType eventType, std::uint8_t* eventParams)
	{
		switch (eventType) {
			case EventType::AreaActivateBoss: {
				if (_activeBoss == nullptr && _nextLevelType == ExitType::None) {
					for (auto& actor : _actors) {
						_activeBoss = runtime_cast<Actors::Bosses::BossBase>(actor);
						if (_activeBoss != nullptr) {
							break;
						}
					}

					if (_activeBoss == nullptr) {
						// No boss was found, it's probably a bug in the level, so go to the next level
						LOGW("No boss was found, skipping to the next level");
						BeginLevelChange(nullptr, ExitType::Boss);
						return;
					}

					if (_activeBoss->OnActivatedBoss()) {
						HandleBossActivated(_activeBoss.get(), initiator);

						if (eventParams != nullptr) {
							size_t musicPathLength = strnlen((const char*)eventParams, 16);
							StringView musicPath((const char*)eventParams, musicPathLength);
							BeginPlayMusic(musicPath);
						}
					}
				}
				break;
			}
			case EventType::AreaCallback: {
#if defined(WITH_ANGELSCRIPT)
				if (_scripts != nullptr) {
					_scripts->OnLevelCallback(initiator, eventParams);
				}
#endif
				break;
			}
			case EventType::ModifierSetWater: {
				// TODO: Implement Instant (non-instant transition), Lighting
				_waterLevel = *(std::uint16_t*)&eventParams[0];
				break;
			}
		}

		for (auto& actor : _actors) {
			actor->OnTriggeredEvent(eventType, eventParams);
		}
	}

	void LevelHandler::BeginLevelChange(Actors::ActorBase* initiator, ExitType exitType, StringView nextLevel)
	{
		if (_nextLevelType != ExitType::None) {
			return;
		}

		_nextLevelName = nextLevel;
		_nextLevelType = exitType;
		
		if ((exitType & ExitType::FastTransition) == ExitType::FastTransition) {
			ExitType exitTypeMasked = (exitType & ExitType::TypeMask);
			if (exitTypeMasked == ExitType::Warp || exitTypeMasked == ExitType::Bonus || exitTypeMasked == ExitType::Boss) {
				_nextLevelTime = 70.0f;
			} else {
				_nextLevelTime = 0.0f;
			}
		} else {
			_nextLevelTime = 360.0f;

			if (_hud != nullptr) {
				_hud->BeginFadeOut(_nextLevelTime - 40.0f);
			}

#if defined(WITH_AUDIO)
			if (_sugarRushMusic != nullptr) {
				_sugarRushMusic->stop();
				_sugarRushMusic = nullptr;
			}
			if (_music != nullptr) {
				_music->stop();
				_music = nullptr;
			}
#endif
		}

		for (auto player : _players) {
			player->OnLevelChanging(initiator, exitType);
		}
	}

	void LevelHandler::SendPacket(const Actors::ActorBase* self, ArrayView<const std::uint8_t> data)
	{
		// Packet cannot be sent anywhere in local sessions
	}

	void LevelHandler::HandleBossActivated(Actors::Bosses::BossBase* boss, Actors::ActorBase* initiator)
	{
		// Used only in derived classes
	}

	void LevelHandler::HandleLevelChange(LevelInitialization&& levelInit)
	{
		_root->ChangeLevel(std::move(levelInit));
	}

	void LevelHandler::HandleGameOver(Actors::Player* player)
	{
		LevelInitialization levelInit;
		PrepareNextLevelInitialization(levelInit);
		levelInit.LevelName = ":gameover"_s;
		HandleLevelChange(std::move(levelInit));
	}

	bool LevelHandler::HandlePlayerDied(Actors::Player* player)
	{
#if defined(WITH_ANGELSCRIPT)
		if (_scripts != nullptr) {
			// TODO: killer
			_scripts->OnPlayerDied(player, nullptr);
		}
#endif

		if (_activeBoss != nullptr) {
			if (_activeBoss->OnPlayerDied()) {
				_activeBoss = nullptr;
			}

			// Warp all other players to checkpoint without transition to avoid issues
			for (auto* otherPlayer : _players) {
				if (otherPlayer != player) {
					otherPlayer->WarpToCheckpoint();
				}
			}
		}

		RollbackToCheckpoint(player);

		// Single player can respawn immediately
		return true;
	}

	void LevelHandler::HandlePlayerWarped(Actors::Player* player, Vector2f prevPos, WarpFlags flags)
	{
		if ((flags & WarpFlags::Fast) == WarpFlags::Fast) {
			WarpCameraToTarget(player, true);
		} else {
			Vector2f pos = player->GetPos();
			if ((prevPos - pos).Length() > 250.0f) {
				WarpCameraToTarget(player);
			}
		}
	}

	void LevelHandler::HandlePlayerCoins(Actors::Player* player, std::int32_t prevCount, std::int32_t newCount)
	{
		// Coins are shared in cooperation, add it also to all other local players
		if (prevCount < newCount) {
			std::int32_t increment = (newCount - prevCount);
			for (auto current : _players) {
				if (current != player) {
					current->AddCoinsInternal(increment);
				}
			}
		}

		_hud->ShowCoins(newCount);
	}

	void LevelHandler::HandlePlayerGems(Actors::Player* player, std::uint8_t gemType, std::int32_t prevCount, std::int32_t newCount)
	{
		_hud->ShowGems(gemType, newCount);
	}

	void LevelHandler::SetCheckpoint(Actors::Player* player, Vector2f pos)
	{
		_checkpointFrames = GetElapsedFrames();

		// All players will be respawned at the checkpoint, so also set the same ambient light
		float ambientLight = _defaultAmbientLight.W;
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				ambientLight = viewport->_ambientLightTarget;
				break;
			}
		}

		for (auto& player : _players) {
			player->SetCheckpoint(pos, ambientLight);
		}

		if (IsLocalSession()) {
			_eventMap->CreateCheckpointForRollback();
			_tileMap->CreateCheckpointForRollback();
		}
	}

	void LevelHandler::RollbackToCheckpoint(Actors::Player* player)
	{
		// Reset the camera
		LimitCameraView(player, player->_pos, 0, 0);

		WarpCameraToTarget(player);

		if (IsLocalSession()) {
			for (auto& actor : _actors) {
				// Despawn all actors that were created after the last checkpoint
				if (actor->_spawnFrames > _checkpointFrames && !actor->GetState(Actors::ActorState::PreserveOnRollback)) {
					if ((actor->_state & (Actors::ActorState::IsCreatedFromEventMap | Actors::ActorState::IsFromGenerator)) != Actors::ActorState::None) {
						Vector2i originTile = actor->_originTile;
						if ((actor->_state & Actors::ActorState::IsFromGenerator) == Actors::ActorState::IsFromGenerator) {
							_eventMap->ResetGenerator(originTile.X, originTile.Y);
						}

						_eventMap->Deactivate(originTile.X, originTile.Y);
					}

					actor->_state |= Actors::ActorState::IsDestroyed;
				}
			}

			_eventMap->RollbackToCheckpoint();
			// Don't rollback the tilemap in local sessions for now
			//_tileMap->RollbackToCheckpoint();
			_elapsedFrames = _checkpointFrames;
		}

		BeginPlayMusic(_musicDefaultPath);

#if defined(WITH_ANGELSCRIPT)
		if (_scripts != nullptr) {
			_scripts->OnLevelReload();
		}
#endif
	}

	void LevelHandler::HandleActivateSugarRush(Actors::Player* player)
	{
#if defined(WITH_AUDIO)
		if (_sugarRushMusic != nullptr) {
			return;
		}

		auto it = _commonResources->Sounds.find(String::nullTerminatedView("SugarRush"_s));
		if (it != _commonResources->Sounds.end()) {
			std::int32_t idx = (it->second.Buffers.size() > 1 ? Random().Next(0, (std::int32_t)it->second.Buffers.size()) : 0);
			_sugarRushMusic = _playingSounds.emplace_back(std::make_shared<AudioBufferPlayer>(&it->second.Buffers[idx]->Buffer));
			_sugarRushMusic->setPosition(Vector3f(0.0f, 0.0f, 100.0f));
			_sugarRushMusic->setGain(PreferencesCache::MasterVolume * PreferencesCache::MusicVolume);
			_sugarRushMusic->setSourceRelative(true);
			_sugarRushMusic->play();

			if (_music != nullptr) {
				_music->pause();
			}
		}
#endif
	}

	void LevelHandler::HandleCreateParticleDebrisOnPerish(const Actors::ActorBase* self, Actors::ParticleDebrisEffect effect, Vector2f speed)
	{
		// Used only in derived classes
	}

	void LevelHandler::HandleCreateSpriteDebris(const Actors::ActorBase* self, AnimState state, std::int32_t count)
	{
		// Used only in derived classes
	}

	void LevelHandler::ShowLevelText(StringView text, Actors::ActorBase* initiator)
	{
		_hud->ShowLevelText(text);
	}

	StringView LevelHandler::GetLevelText(std::uint32_t textId, std::int32_t index, std::uint32_t delimiter)
	{
		if (textId >= _levelTexts.size()) {
			return {};
		}

		StringView text = _levelTexts[textId];
		std::int32_t textSize = (std::int32_t)text.size();

		if (textSize > 0 && index >= 0) {
			std::int32_t delimiterCount = 0;
			std::int32_t start = 0;
			std::int32_t idx = 0;
			do {
				Pair<char32_t, std::size_t> cursor = Death::Utf8::NextChar(text, idx);

				if (cursor.first() == delimiter) {
					if (delimiterCount == index - 1) {
						start = idx + 1;
					} else if (delimiterCount == index) {
						return StringView(text.data() + start, idx - start);
					}
					delimiterCount++;
				}

				idx = (std::int32_t)cursor.second();
			} while (idx < textSize);

			if (delimiterCount == index) {
				return StringView(text.data() + start, text.size() - start);
			} else {
				return {};
			}
		}

		return _x(_levelName, text.data());
	}

	void LevelHandler::OverrideLevelText(std::uint32_t textId, StringView value)
	{
		if (textId >= _levelTexts.size()) {
			if (value.empty()) {
				return;
			}

			_levelTexts.resize(textId + 1);
		}

		_levelTexts[textId] = value;
	}

	bool LevelHandler::PlayerActionPressed(Actors::Player* player, PlayerAction action, bool includeGamepads)
	{
		bool isGamepad;
		return PlayerActionPressed(player, action, includeGamepads, isGamepad);
	}

	bool LevelHandler::PlayerActionPressed(Actors::Player* player, PlayerAction action, bool includeGamepads, bool& isGamepad)
	{
		if (_console->IsVisible() && action != PlayerAction::Menu && action != PlayerAction::Console) {
			return false;
		}

		isGamepad = false;
		std::int32_t playerIndex = (player ? player->GetPlayerIndex() : 0);
		auto& input = _playerInputs[playerIndex];
		if ((input.PressedActions & (1ull << (std::int32_t)action)) != 0) {
			isGamepad = (input.PressedActions & (1ull << (32 + (std::int32_t)action))) != 0;
			return true;
		}

		return false;
	}

	bool LevelHandler::PlayerActionHit(Actors::Player* player, PlayerAction action, bool includeGamepads)
	{
		bool isGamepad;
		return PlayerActionHit(player, action, includeGamepads, isGamepad);
	}

	bool LevelHandler::PlayerActionHit(Actors::Player* player, PlayerAction action, bool includeGamepads, bool& isGamepad)
	{
		if (_console->IsVisible() && action != PlayerAction::Menu && action != PlayerAction::Console) {
			return false;
		}

		isGamepad = false;
		std::int32_t playerIndex = (player ? player->GetPlayerIndex() : 0);
		auto& input = _playerInputs[playerIndex];
		if ((input.PressedActions & (1ull << (std::int32_t)action)) != 0 && (input.PressedActionsLast & (1ull << (std::int32_t)action)) == 0) {
			isGamepad = (input.PressedActions & (1ull << (32 + (std::int32_t)action))) != 0;
			return true;
		}

		return false;
	}

	float LevelHandler::PlayerHorizontalMovement(Actors::Player* player)
	{
		if (_console->IsVisible()) {
			return 0.0f;
		}

		auto& input = _playerInputs[player->GetPlayerIndex()];
		return (input.Frozen ? input.FrozenMovement.X : input.RequiredMovement.X);
	}

	float LevelHandler::PlayerVerticalMovement(Actors::Player* player)
	{
		if (_console->IsVisible()) {
			return 0.0f;
		}

		auto& input = _playerInputs[player->GetPlayerIndex()];
		return (input.Frozen ? input.FrozenMovement.Y : input.RequiredMovement.Y);
	}

	void LevelHandler::PlayerExecuteRumble(Actors::Player* player, StringView rumbleEffect)
	{
#if defined(NCINE_HAS_GAMEPAD_RUMBLE)
		auto it = _rumbleEffects.find(String::nullTerminatedView(rumbleEffect));
		if (it == _rumbleEffects.end()) {
			return;
		}

		std::int32_t joyIdx = ControlScheme::GetGamepadForPlayer(player->GetPlayerIndex());
		if (joyIdx >= 0) {
			_rumble.ExecuteEffect(joyIdx, it->second);
		}
#endif
	}

	bool LevelHandler::SerializeResumableToStream(Stream& dest)
	{
		std::uint8_t flags = 0;
		if (_isReforged) flags |= 0x01;
		if (_cheatsUsed) flags |= 0x02;
		dest.WriteValue<std::uint8_t>(flags);

		auto p = _levelName.partition('/');

		dest.WriteValue<std::uint8_t>((std::uint8_t)p[0].size());
		dest.Write(p[0].data(), (std::uint32_t)p[0].size());
		dest.WriteValue<std::uint8_t>((std::uint8_t)p[2].size());
		dest.Write(p[2].data(), (std::uint32_t)p[2].size());

		dest.WriteValue<std::uint8_t>((std::uint8_t)_difficulty);
		dest.WriteVariableUint64(_elapsedMillisecondsBegin);
		dest.WriteValue<float>(_checkpointFrames);
		dest.WriteValue<float>(_waterLevel);
		dest.WriteValue<std::uint8_t>((std::uint8_t)_weatherType);
		dest.WriteValue<std::uint8_t>(_weatherIntensity);

		_tileMap->SerializeResumableToStream(dest, true);
		_eventMap->SerializeResumableToStream(dest, true);

		std::size_t playerCount = _players.size();
		dest.WriteValue<std::uint8_t>((std::uint8_t)playerCount);
		for (std::size_t i = 0; i < playerCount; i++) {
			_players[i]->SerializeResumableToStream(dest);
		}

		return true;
	}

	void LevelHandler::OnTileFrozen(std::int32_t x, std::int32_t y)
	{
		bool iceBlockFound = false;
		FindCollisionActorsByAABB(nullptr, AABBf(x - 1.0f, y - 1.0f, x + 1.0f, y + 1.0f), [&iceBlockFound](Actors::ActorBase* actor) -> bool {
			if ((actor->GetState() & Actors::ActorState::IsDestroyed) != Actors::ActorState::None) {
				return true;
			}

			auto* iceBlock = runtime_cast<Actors::Environment::IceBlock>(actor);
			if (iceBlock != nullptr) {
				iceBlock->ResetTimeLeft();
				iceBlockFound = true;
				return false;
			}

			return true;
		});

		if (!iceBlockFound) {
			std::shared_ptr<Actors::Environment::IceBlock> iceBlock = std::make_shared<Actors::Environment::IceBlock>();
			iceBlock->OnActivated(Actors::ActorActivationDetails(
				this,
				Vector3i(x - 1, y - 2, ILevelHandler::MainPlaneZ)
			));
			AddActor(iceBlock);
		}
	}

	void LevelHandler::BeforeActorDestroyed(Actors::ActorBase* actor)
	{
		// Nothing to do here
	}

	void LevelHandler::ProcessEvents(float timeMult)
	{
		ZoneScopedC(0x4876AF);

		if (!_players.empty()) {
			std::size_t playerCount = _players.size();
			SmallVector<AABBi, ControlScheme::MaxSupportedPlayers * 2> playerZones;
			playerZones.reserve(playerCount * 2);
			for (std::size_t i = 0; i < playerCount; i++) {
				auto pos = _players[i]->GetPos();
				std::int32_t tx = (std::int32_t)pos.X / TileSet::DefaultTileSize;
				std::int32_t ty = (std::int32_t)pos.Y / TileSet::DefaultTileSize;

				const auto& activationRange = playerZones.emplace_back(tx - ActivateTileRange, ty - ActivateTileRange, tx + ActivateTileRange, ty + ActivateTileRange);
				playerZones.emplace_back(activationRange.L - 4, activationRange.T - 4, activationRange.R + 4, activationRange.B + 4);
			}

			for (auto& actor : _actors) {
				if ((actor->_state & (Actors::ActorState::IsCreatedFromEventMap | Actors::ActorState::IsFromGenerator)) != Actors::ActorState::None) {
					Vector2i originTile = actor->_originTile;
					bool isInside = false;
					for (std::size_t i = 1; i < playerZones.size(); i += 2) {
						if (playerZones[i].Contains(originTile)) {
							isInside = true;
							break;
						}
					}

					if (!isInside && actor->OnTileDeactivated()) {
						if ((actor->_state & Actors::ActorState::IsFromGenerator) == Actors::ActorState::IsFromGenerator) {
							_eventMap->ResetGenerator(originTile.X, originTile.Y);
						}

						_eventMap->Deactivate(originTile.X, originTile.Y);
						actor->_state |= Actors::ActorState::IsDestroyed;
					}
				}
			}

			for (std::size_t i = 0; i < playerZones.size(); i += 2) {
				const auto& activationZone = playerZones[i];
				_eventMap->ActivateEvents(activationZone.L, activationZone.T, activationZone.R, activationZone.B, true);
			}

			if (!_checkpointCreated) {
				// Create checkpoint after first call to ActivateEvents() to avoid duplication of objects that are spawned near player spawn
				_checkpointCreated = true;
				_eventMap->CreateCheckpointForRollback();
				_tileMap->CreateCheckpointForRollback();
#if defined(WITH_ANGELSCRIPT)
				if (_scripts != nullptr) {
					_scripts->OnLevelBegin();
				}
#endif
			}
		}

		_eventMap->ProcessGenerators(timeMult);
	}

	void LevelHandler::ProcessQueuedNextLevel()
	{
		bool playersReady = true;
		for (auto player : _players) {
			// Exit type was already provided in BeginLevelChange()
			playersReady &= player->OnLevelChanging(nullptr, ExitType::None);
		}

		if (playersReady && _nextLevelTime <= 0.0f) {
			LevelInitialization levelInit;
			PrepareNextLevelInitialization(levelInit);
			HandleLevelChange(std::move(levelInit));
		}
	}

	void LevelHandler::PrepareNextLevelInitialization(LevelInitialization& levelInit)
	{
		StringView realNextLevel;
		if (!_nextLevelName.empty()) {
			realNextLevel = _nextLevelName;
		} else {
			realNextLevel = ((_nextLevelType & ExitType::TypeMask) == ExitType::Bonus ? _defaultSecretLevel : _defaultNextLevel);
		}

		auto p = _levelName.partition('/');
		if (!realNextLevel.empty()) {
			if (realNextLevel.contains('/')) {
				levelInit.LevelName = realNextLevel;
			} else {
				levelInit.LevelName = p[0] + '/' + realNextLevel;
			}
		}

		levelInit.Difficulty = _difficulty;
		levelInit.IsReforged = _isReforged;
		levelInit.CheatsUsed = _cheatsUsed;
		levelInit.LastExitType = _nextLevelType;
		levelInit.LastEpisodeName = p[0];
		levelInit.ElapsedMilliseconds = _elapsedMillisecondsBegin + (std::uint64_t)(_elapsedFrames * FrameTimer::SecondsPerFrame * 1000.0f);

		for (std::int32_t i = 0; i < _players.size(); i++) {
			levelInit.PlayerCarryOvers[i] = _players[i]->PrepareLevelCarryOver();
		}
	}

	Recti LevelHandler::GetPlayerViewportBounds(std::int32_t w, std::int32_t h, std::int32_t index)
	{
		std::int32_t count = (std::int32_t)_assignedViewports.size();

		switch (count) {
			default:
			case 1: {
				return Recti(0, 0, w, h);
			}
			case 2: {
				if (PreferencesCache::PreferVerticalSplitscreen) {
					std::int32_t halfW = w / 2;
					return Recti(index * halfW, 0, halfW, h);
				} else {
					std::int32_t halfH = h / 2;
					return Recti(0, index * halfH, w, halfH);
				}
			}
			case 3:
			case 4: {
				std::int32_t halfW = (w + 1) / 2;
				std::int32_t halfH = (h + 1) / 2;
				return Recti((index % 2) * halfW, (index / 2) * halfH, halfW, halfH);
			}
		}
	}

	void LevelHandler::ProcessWeather(float timeMult)
	{
		if (_weatherType == WeatherType::None) {
			return;
		}

		std::size_t playerCount = _assignedViewports.size();
		SmallVector<Rectf, ControlScheme::MaxSupportedPlayers> playerZones;
		playerZones.reserve(playerCount);
		for (std::size_t i = 0; i < playerCount; i++) {
			Rectf cullingRect = _assignedViewports[i]->_view->GetCullingRect();

			bool found = false;
			for (std::size_t j = 0; j < playerZones.size(); j++) {
				if (playerZones[j].Overlaps(cullingRect)) {
					playerZones[j].Union(cullingRect);
					found = true;
					break;
				}
			}

			if (!found) {
				playerZones.push_back(std::move(cullingRect));
			}
		}

		std::int32_t weatherIntensity = std::max((std::int32_t)(_weatherIntensity * timeMult), 1);

		for (auto& zone : playerZones) {
			for (std::int32_t i = 0; i < weatherIntensity; i++) {
				TileMap::DebrisFlags debrisFlags;
				if ((_weatherType & WeatherType::OutdoorsOnly) == WeatherType::OutdoorsOnly) {
					debrisFlags = TileMap::DebrisFlags::Disappear;
				} else {
					debrisFlags = (Random().FastFloat() > 0.7f
						? TileMap::DebrisFlags::None
						: TileMap::DebrisFlags::Disappear);
				}

				Vector2f debrisPos = Vector2f(zone.X + Random().FastFloat(zone.W * -1.0f, zone.W * 2.0f),
					zone.Y + Random().NextFloat(zone.H * -1.0f, zone.H * 2.0f));

				WeatherType realWeatherType = (_weatherType & ~WeatherType::OutdoorsOnly);
				if (realWeatherType == WeatherType::Rain) {
					auto* res = _commonResources->FindAnimation(Rain);
					if (res != nullptr) {
						auto& resBase = res->Base;
						Vector2i texSize = resBase->TextureDiffuse->size();
						float scale = Random().FastFloat(0.4f, 1.1f);
						float speedX = Random().FastFloat(2.2f, 2.7f) * scale;
						float speedY = Random().FastFloat(7.6f, 8.6f) * scale;

						TileMap::DestructibleDebris debris = { };
						debris.Pos = debrisPos;
						debris.Depth = MainPlaneZ - 100 + (std::uint16_t)(200 * scale);
						debris.Size = resBase->FrameDimensions.As<float>();
						debris.Speed = Vector2f(speedX, speedY);
						debris.Acceleration = Vector2f(0.0f, 0.0f);

						debris.Scale = scale;
						debris.ScaleSpeed = 0.0f;
						debris.Angle = atan2f(speedY, speedX);
						debris.AngleSpeed = 0.0f;
						debris.Alpha = 1.0f;
						debris.AlphaSpeed = 0.0f;

						debris.Time = 180.0f;

						std::uint32_t curAnimFrame = res->FrameOffset + Random().Next(0, res->FrameCount);
						std::uint32_t col = curAnimFrame % resBase->FrameConfiguration.X;
						std::uint32_t row = curAnimFrame / resBase->FrameConfiguration.X;
						debris.TexScaleX = (float(resBase->FrameDimensions.X) / float(texSize.X));
						debris.TexBiasX = (float(resBase->FrameDimensions.X * col) / float(texSize.X));
						debris.TexScaleY = (float(resBase->FrameDimensions.Y) / float(texSize.Y));
						debris.TexBiasY = (float(resBase->FrameDimensions.Y * row) / float(texSize.Y));

						debris.DiffuseTexture = resBase->TextureDiffuse.get();
						debris.Flags = debrisFlags;

						_tileMap->CreateDebris(debris);
					}
				} else {
					auto* res = _commonResources->FindAnimation(Snow);
					if (res != nullptr) {
						auto& resBase = res->Base;
						Vector2i texSize = resBase->TextureDiffuse->size();
						float scale = Random().FastFloat(0.4f, 1.1f);
						float speedX = Random().FastFloat(-1.6f, -1.2f) * scale;
						float speedY = Random().FastFloat(3.0f, 4.0f) * scale;
						float accel = Random().FastFloat(-0.008f, 0.008f) * scale;

						TileMap::DestructibleDebris debris = { };
						debris.Pos = debrisPos;
						debris.Depth = MainPlaneZ - 100 + (std::uint16_t)(200 * scale);
						debris.Size = resBase->FrameDimensions.As<float>();
						debris.Speed = Vector2f(speedX, speedY);
						debris.Acceleration = Vector2f(accel, -std::abs(accel));

						debris.Scale = scale;
						debris.ScaleSpeed = 0.0f;
						debris.Angle = Random().FastFloat(0.0f, fTwoPi);
						debris.AngleSpeed = speedX * 0.02f;
						debris.Alpha = 1.0f;
						debris.AlphaSpeed = 0.0f;

						debris.Time = 180.0f;

						std::uint32_t curAnimFrame = res->FrameOffset + Random().Next(0, res->FrameCount);
						std::uint32_t col = curAnimFrame % resBase->FrameConfiguration.X;
						std::uint32_t row = curAnimFrame / resBase->FrameConfiguration.X;
						debris.TexScaleX = (float(resBase->FrameDimensions.X) / float(texSize.X));
						debris.TexBiasX = (float(resBase->FrameDimensions.X * col) / float(texSize.X));
						debris.TexScaleY = (float(resBase->FrameDimensions.Y) / float(texSize.Y));
						debris.TexBiasY = (float(resBase->FrameDimensions.Y * row) / float(texSize.Y));

						debris.DiffuseTexture = resBase->TextureDiffuse.get();
						debris.Flags = debrisFlags;

						_tileMap->CreateDebris(debris);
					}
				}
			}
		}
	}

	void LevelHandler::ResolveCollisions(float timeMult)
	{
		ZoneScopedC(0x4876AF);

		auto it = _actors.begin();
		while (it != _actors.end()) {
			Actors::ActorBase* actor = it->get();
			if (actor->GetState(Actors::ActorState::IsDestroyed)) {
				BeforeActorDestroyed(actor);
				if (actor->_collisionProxyID != Collisions::NullNode) {
					_collisions.DestroyProxy(actor->_collisionProxyID);
					actor->_collisionProxyID = Collisions::NullNode;
				}
				it = _actors.eraseUnordered(it);
				continue;
			}
			
			if (actor->GetState(Actors::ActorState::IsDirty)) {
				if (actor->_collisionProxyID == Collisions::NullNode) {
					continue;
				}

				actor->UpdateAABB();
				_collisions.MoveProxy(actor->_collisionProxyID, actor->AABB, actor->_speed * timeMult);
				actor->SetState(Actors::ActorState::IsDirty, false);
			}
			++it;
		}

		struct UpdatePairsHelper {
			void OnPairAdded(void* proxyA, void* proxyB) {
				Actors::ActorBase* actorA = (Actors::ActorBase*)proxyA;
				Actors::ActorBase* actorB = (Actors::ActorBase*)proxyB;
				if (((actorA->GetState() | actorB->GetState()) & (Actors::ActorState::CollideWithOtherActors | Actors::ActorState::IsDestroyed)) != Actors::ActorState::CollideWithOtherActors) {
					return;
				}

				if (actorA->IsCollidingWith(actorB)) {
					std::shared_ptr<Actors::ActorBase> actorSharedA = actorA->shared_from_this();
					std::shared_ptr<Actors::ActorBase> actorSharedB = actorB->shared_from_this();
					if (!actorSharedA->OnHandleCollision(actorSharedB->shared_from_this())) {
						actorSharedB->OnHandleCollision(actorSharedA->shared_from_this());
					}
				}
			}
		};
		UpdatePairsHelper helper;
		_collisions.UpdatePairs(&helper);
	}

	void LevelHandler::AssignViewport(Actors::Player* player)
	{
		_assignedViewports.push_back(std::make_unique<Rendering::PlayerViewport>(this, player));

#if defined(WITH_AUDIO)
		for (auto& current : _playingSounds) {
			if (auto* currentForSplitscreen = runtime_cast<AudioBufferPlayerForSplitscreen>(current.get())) {
				currentForSplitscreen->updateViewports(_assignedViewports);
			}
		}
#endif
	}

	void LevelHandler::InitializeCamera(Rendering::PlayerViewport& viewport)
	{
		if (viewport._targetActor == nullptr) {
			return;
		}

		viewport._viewBounds = _viewBoundsTarget;

		// The position to focus on
		Vector2f focusPos = viewport._targetActor->_pos;
		Vector2i halfView = viewport._view->GetSize() / 2;

		// Clamp camera position to level bounds
		if (viewport._viewBounds.W > halfView.X * 2) {
			viewport._cameraPos.X = std::round(std::clamp(focusPos.X, viewport._viewBounds.X + halfView.X, viewport._viewBounds.X + viewport._viewBounds.W - halfView.X));
		} else {
			viewport._cameraPos.X = std::round(viewport._viewBounds.X + viewport._viewBounds.W * 0.5f);
		}
		if (viewport._viewBounds.H > halfView.Y * 2) {
			viewport._cameraPos.Y = std::round(std::clamp(focusPos.Y, viewport._viewBounds.Y + halfView.Y, viewport._viewBounds.Y + viewport._viewBounds.H - halfView.Y));
		} else {
			viewport._cameraPos.Y = std::round(viewport._viewBounds.Y + viewport._viewBounds.H * 0.5f);
		}

		viewport._cameraLastPos = viewport._cameraPos;
		viewport._camera->SetView(viewport._cameraPos, 0.0f, 1.0f);
	}

	Vector2f LevelHandler::GetCameraPos(Actors::Player* player) const
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				return viewport->_cameraPos;
			}
		}
		return {};
	}

	void LevelHandler::LimitCameraView(Actors::Player* player, Vector2f playerPos, std::int32_t left, std::int32_t width)
	{
		_levelBounds.X = left;
		if (width > 0.0f) {
			_levelBounds.W = width;
		} else {
			_levelBounds.W = _tileMap->GetLevelBounds().X - left;
		}

		Rectf bounds = _levelBounds.As<float>();
		if (left == 0 && width == 0) {
			for (auto& viewport : _assignedViewports) {
				viewport->_viewBounds = bounds;
			}
			_viewBoundsTarget = bounds;
		} else {
			Rendering::PlayerViewport* currentViewport = nullptr;
			float maxViewWidth = 0.0f;
			for (auto& viewport : _assignedViewports) {
				auto size = viewport->GetViewportSize();
				if (maxViewWidth < size.X) {
					maxViewWidth = (float)size.X;
				}
				if (viewport->_targetActor == player) {
					currentViewport = viewport.get();
				}
			}

			if (bounds.W < maxViewWidth) {
				bounds.X -= (maxViewWidth - bounds.W);
				bounds.W = maxViewWidth;
			}

			if (_viewBoundsTarget != bounds) {
				_viewBoundsTarget = bounds;

				if (currentViewport != nullptr) {
					float limit = currentViewport->_cameraPos.X - (maxViewWidth * 0.6f);
					if (currentViewport->_viewBounds.X < limit) {
						currentViewport->_viewBounds.W += (currentViewport->_viewBounds.X - limit);
						currentViewport->_viewBounds.X = limit;
					}
				}

				// Warp all other distant players to this player
				for (auto& viewport : _assignedViewports) {
					if (viewport->_targetActor != player) {
						float limit = viewport->_cameraPos.X - (maxViewWidth * 0.6f);
						if (viewport->_viewBounds.X < limit) {
							viewport->_viewBounds.W += (viewport->_viewBounds.X - limit);
							viewport->_viewBounds.X = limit;
						}

						auto pos = viewport->_targetActor->_pos;
						if ((pos.X < bounds.X || pos.X >= bounds.X + bounds.W) && (pos - playerPos).Length() > 100.0f) {
							if (auto* otherPlayer = runtime_cast<Actors::Player>(viewport->_targetActor)) {
								otherPlayer->WarpToPosition(playerPos, WarpFlags::SkipWarpIn);
								if (currentViewport != nullptr) {
									viewport->_ambientLight = currentViewport->_ambientLight;
									viewport->_ambientLightTarget = currentViewport->_ambientLightTarget;
								}
							}
						}
					}
				}
			}
		}
	}

	void LevelHandler::OverrideCameraView(Actors::Player* player, float x, float y, bool topLeft)
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				viewport->OverrideCamera(x, y, topLeft);
			}
		}
	}

	void LevelHandler::ShakeCameraView(Actors::Player* player, float duration)
	{
		for (auto& viewport : _assignedViewports) {
			if (viewport->_targetActor == player) {
				viewport->ShakeCameraView(duration);
			}
		}

		PlayerExecuteRumble(player, "Shake"_s);
	}

	void LevelHandler::ShakeCameraViewNear(Vector2f pos, float duration)
	{
		constexpr float MaxDistance = 800.0f;

		for (auto& viewport : _assignedViewports) {
			if ((viewport->_targetActor->_pos - pos).Length() <= MaxDistance) {
				viewport->ShakeCameraView(duration);

				if (auto* player = runtime_cast<Actors::Player>(viewport->_targetActor)) {
					PlayerExecuteRumble(player, "Shake"_s);
				}
			}
		}
	}

	bool LevelHandler::GetTrigger(std::uint8_t triggerId)
	{
		return _tileMap->GetTrigger(triggerId);
	}

	void LevelHandler::SetTrigger(std::uint8_t triggerId, bool newState)
	{
		_tileMap->SetTrigger(triggerId, newState);
	}

	void LevelHandler::SetWeather(WeatherType type, std::uint8_t intensity)
	{
		_weatherType = type;
		_weatherIntensity = intensity;
	}

	bool LevelHandler::BeginPlayMusic(StringView path, bool setDefault, bool forceReload)
	{
		bool result = false;

#if defined(WITH_AUDIO)
		if (_sugarRushMusic != nullptr) {
			_sugarRushMusic->stop();
		}

		if (!forceReload && _musicCurrentPath == path) {
			// Music is already playing or is paused
			if (_music != nullptr) {
				_music->play();
			}
			if (setDefault) {
				_musicDefaultPath = path;
			}
			return false;
		}

		if (_music != nullptr) {
			_music->stop();
		}

		if (!path.empty()) {
			_music = ContentResolver::Get().GetMusic(path);
			if (_music != nullptr) {
				_music->setLooping(true);
				_music->setGain(PreferencesCache::MasterVolume * PreferencesCache::MusicVolume);
				_music->setSourceRelative(true);
				_music->play();
				result = true;
			}
		} else {
			_music = nullptr;
		}

		_musicCurrentPath = path;
		if (setDefault) {
			_musicDefaultPath = path;
		}
#endif

		return result;
	}

	void LevelHandler::UpdatePressedActions()
	{
		ZoneScopedC(0x4876AF);

		auto& input = theApplication().GetInputManager();

		const JoyMappedState* joyStates[ControlScheme::MaxConnectedGamepads];
		std::int32_t joyStatesCount = 0;
		for (std::int32_t i = 0; i < JoyMapping::MaxNumJoysticks && joyStatesCount < std::int32_t(arraySize(joyStates)); i++) {
			if (input.isJoyMapped(i)) {
				joyStates[joyStatesCount++] = &input.joyMappedState(i);
			}
		}

		for (std::int32_t i = 0; i < ControlScheme::MaxSupportedPlayers; i++) {
			auto& input = _playerInputs[i];
			auto processedInput = ControlScheme::FetchProcessedInput(i,
				_pressedKeys, ArrayView(joyStates, joyStatesCount), input.PressedActions,
				_hud == nullptr || !_hud->IsWeaponWheelVisible(i));

			input.PressedActionsLast = input.PressedActions;
			input.PressedActions = processedInput.PressedActions;
			input.RequiredMovement = processedInput.Movement;
		}

		// Also apply overriden actions (by touch controls)
		{
			auto& input = _playerInputs[0];
			input.PressedActions |= _overrideActions;

			if ((_overrideActions & (1 << (std::int32_t)PlayerAction::Right)) != 0) {
				input.RequiredMovement.X = 1.0f;
			} else if ((_overrideActions & (1 << (std::int32_t)PlayerAction::Left)) != 0) {
				input.RequiredMovement.X = -1.0f;
			}
			if ((_overrideActions & (1 << (std::int32_t)PlayerAction::Down)) != 0) {
				input.RequiredMovement.Y = 1.0f;
			} else if ((_overrideActions & (1 << (std::int32_t)PlayerAction::Up)) != 0) {
				input.RequiredMovement.Y = -1.0f;
			}
		}
	}

	void LevelHandler::UpdateRichPresence()
	{
#if (defined(DEATH_TARGET_WINDOWS) && !defined(DEATH_TARGET_WINDOWS_RT)) || defined(DEATH_TARGET_UNIX)
		if (!PreferencesCache::EnableDiscordIntegration || !UI::DiscordRpcClient::Get().IsSupported()) {
			return;
		}

		auto p = _levelName.partition('/');

		UI::DiscordRpcClient::RichPresence richPresence;
		if (p[0] == "prince"_s) {
			if (p[2] == "01_castle1"_s || p[2] == "02_castle1n"_s) {
				richPresence.LargeImage = "level-prince-01"_s;
			} else if (p[2] == "03_carrot1"_s || p[2] == "04_carrot1n"_s) {
				richPresence.LargeImage = "level-prince-02"_s;
			} else if (p[2] == "05_labrat1"_s || p[2] == "06_labrat2"_s || p[2] == "bonus_labrat3"_s) {
				richPresence.LargeImage = "level-prince-03"_s;
			}
		} else if (p[0] == "rescue"_s) {
			if (p[2] == "01_colon1"_s || p[2] == "02_colon2"_s) {
				richPresence.LargeImage = "level-rescue-01"_s;
			} else if (p[2] == "03_psych1"_s || p[2] == "04_psych2"_s || p[2] == "bonus_psych3"_s) {
				richPresence.LargeImage = "level-rescue-02"_s;
			} else if (p[2] == "05_beach"_s || p[2] == "06_beach2"_s) {
				richPresence.LargeImage = "level-rescue-03"_s;
			}
		} else if (p[0] == "flash"_s) {
			if (p[2] == "01_diam1"_s || p[2] == "02_diam3"_s) {
				richPresence.LargeImage = "level-flash-01"_s;
			} else if (p[2] == "03_tube1"_s || p[2] == "04_tube2"_s || p[2] == "bonus_tube3"_s) {
				richPresence.LargeImage = "level-flash-02"_s;
			} else if (p[2] == "05_medivo1"_s || p[2] == "06_medivo2"_s || p[2] == "bonus_garglair"_s) {
				richPresence.LargeImage = "level-flash-03"_s;
			}
		} else if (p[0] == "monk"_s) {
			if (p[2] == "01_jung1"_s || p[2] == "02_jung2"_s) {
				richPresence.LargeImage = "level-monk-01"_s;
			} else if (p[2] == "03_hell"_s || p[2] == "04_hell2"_s) {
				richPresence.LargeImage = "level-monk-02"_s;
			} else if (p[2] == "05_damn"_s || p[2] == "06_damn2"_s) {
				richPresence.LargeImage = "level-monk-03"_s;
			}
		} else if (p[0] == "secretf"_s) {
			if (p[2] == "01_easter1"_s || p[2] == "02_easter2"_s || p[2] == "03_easter3"_s) {
				richPresence.LargeImage = "level-secretf-01"_s;
			} else if (p[2] == "04_haunted1"_s || p[2] == "05_haunted2"_s || p[2] == "06_haunted3"_s) {
				richPresence.LargeImage = "level-secretf-02"_s;
			} else if (p[2] == "07_town1"_s || p[2] == "08_town2"_s || p[2] == "09_town3"_s) {
				richPresence.LargeImage = "level-secretf-03"_s;
			}
		} else if (p[0] == "xmas98"_s || p[0] == "xmas99"_s) {
			richPresence.LargeImage = "level-xmas"_s;
		} else if (p[0] == "share"_s) {
			richPresence.LargeImage = "level-share"_s;
		}

		if (richPresence.LargeImage.empty()) {
			richPresence.Details = "Playing as "_s;
			richPresence.LargeImage = "main-transparent"_s;

			if (!_players.empty())
				switch (_players[0]->GetPlayerType()) {
					default:
					case PlayerType::Jazz: richPresence.SmallImage = "playing-jazz"_s; break;
					case PlayerType::Spaz: richPresence.SmallImage = "playing-spaz"_s; break;
					case PlayerType::Lori: richPresence.SmallImage = "playing-lori"_s; break;
				}
		} else {
			richPresence.Details = "Playing episode as "_s;
		}

		if (!_players.empty()) {
			switch (_players[0]->GetPlayerType()) {
				default:
				case PlayerType::Jazz: richPresence.Details += "Jazz"_s; break;
				case PlayerType::Spaz: richPresence.Details += "Spaz"_s; break;
				case PlayerType::Lori: richPresence.Details += "Lori"_s; break;
			}
		}

		UI::DiscordRpcClient::Get().SetRichPresence(richPresence);
#endif
	}

	void LevelHandler::InitializeRumbleEffects()
	{
#if defined(NCINE_HAS_GAMEPAD_RUMBLE)
		if (auto* breakTile = RegisterRumbleEffect("BreakTile"_s)) {
			breakTile->AddToTimeline(10, 1.0f, 0.0f);
		}

		if (auto* hurt = RegisterRumbleEffect("Hurt"_s)) {
			hurt->AddToTimeline(4, 0.15f, 0.0f);
			hurt->AddToTimeline(8, 0.45f, 0.0f);
			hurt->AddToTimeline(12, 0.15f, 0.0f);
		}

		if (auto* die = RegisterRumbleEffect("Die"_s)) {
			die->AddToTimeline(4, 0.9f, 0.3f);
			die->AddToTimeline(8, 0.3f, 0.9f);
			die->AddToTimeline(12, 0.0f, 0.9f);
		}

		if (auto* land = RegisterRumbleEffect("Land"_s)) {
			land->AddToTimeline(4, 0.0f, 0.525f);
		}

		if (auto* spring = RegisterRumbleEffect("Spring"_s)) {
			spring->AddToTimeline(10, 0.0f, 0.8f);
		}

		if (auto* fire = RegisterRumbleEffect("Fire"_s)) {
			fire->AddToTimeline(4, 0.0f, 0.0f, 0.0f, 0.3f);
		}

		if (auto* fireWeak = RegisterRumbleEffect("FireWeak"_s)) {
			fireWeak->AddToTimeline(16, 0.0f, 0.0f, 0.0f, 0.04f);
		}

		if (auto* warp = RegisterRumbleEffect("Warp"_s)) {
			warp->AddToTimeline(2, 0.0f, 0.0f, 0.02f, 0.01f);
			warp->AddToTimeline(6, 0.3f, 0.0f, 0.04f, 0.02f);
			warp->AddToTimeline(10, 0.2f, 0.0f, 0.08f, 0.02f);
			warp->AddToTimeline(13, 0.1f, 0.0f, 0.04f, 0.04f);
			warp->AddToTimeline(16, 0.0f, 0.0f, 0.02f, 0.08f);
			warp->AddToTimeline(20, 0.0f, 0.0f, 0.0f, 0.04f);
			warp->AddToTimeline(22, 0.0f, 0.0f, 0.0f, 0.02f);
		}

		if (auto* shake = RegisterRumbleEffect("Shake"_s)) {
			shake->AddToTimeline(20, 1.0f, 1.0f);
			shake->AddToTimeline(20, 0.6f, 0.6f);
			shake->AddToTimeline(30, 0.2f, 0.2f);
			shake->AddToTimeline(40, 0.2f, 0.0f);
		}
#endif
	}

	RumbleDescription* LevelHandler::RegisterRumbleEffect(StringView name)
	{
#if defined(NCINE_HAS_GAMEPAD_RUMBLE)
		auto it = _rumbleEffects.emplace(name, std::make_shared<RumbleDescription>());
		return (it.second ? it.first->second.get() : nullptr);
#else
		return nullptr;
#endif
	}

	void LevelHandler::PauseGame()
	{
		// Show in-game pause menu
		_pauseMenu = std::make_shared<UI::Menu::InGameMenu>(this);
		if (IsPausable()) {
			// Prevent updating of all level objects
			_rootNode->setUpdateEnabled(false);
#if defined(NCINE_HAS_GAMEPAD_RUMBLE)
			_rumble.CancelAllEffects();
#endif
		}

#if defined(WITH_AUDIO)
		// Use low-pass filter on music and pause all SFX
		if (_music != nullptr) {
			_music->setLowPass(0.1f);
		}
		if (IsPausable()) {
			for (auto& sound : _playingSounds) {
				if (sound->isPlaying()) {
					sound->pause();
				}
			}
			// If Sugar Rush music is playing, pause it and play normal music instead
			if (_sugarRushMusic != nullptr && _music != nullptr) {
				_music->play();
			}
		}
#endif
	}

	void LevelHandler::ResumeGame()
	{
		// Resume all level objects
		_rootNode->setUpdateEnabled(true);
		// Hide in-game pause menu
		_pauseMenu = nullptr;

#if defined(WITH_AUDIO)
		// If Sugar Rush music was playing, resume it and pause normal music again
		if (_sugarRushMusic != nullptr && _music != nullptr) {
			_music->pause();
		}
		// Resume all SFX
		for (auto& sound : _playingSounds) {
			if (sound->isPaused()) {
				sound->play();
			}
		}
		if (_music != nullptr) {
			_music->setLowPass(1.0f);
		}
#endif

		// Mark Menu button as already pressed to avoid some issues
		for (auto& input : _playerInputs) {
			input.PressedActions |= (1ull << (std::int32_t)PlayerAction::Menu);
			input.PressedActionsLast |= (1ull << (std::int32_t)PlayerAction::Menu);
		}
	}

	bool LevelHandler::CheatKill()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->TakeDamage(INT32_MAX, 0.0f, true);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatGod()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->SetInvulnerability(36000.0f, Actors::Player::InvulnerableType::Shielded);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatNext()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			BeginLevelChange(nullptr, ExitType::Warp | ExitType::FastTransition);
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatGuns()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				for (std::int32_t i = 0; i < (std::int32_t)WeaponType::Count; i++) {
					player->AddAmmo((WeaponType)i, 99);
				}
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatRush()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->ActivateSugarRush(1300.0f);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatGems()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->AddGems(0, 5);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatBird()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->SpawnBird(0, player->GetPos());
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatLife()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				player->AddLives(5);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatPower()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				for (std::int32_t i = 0; i < (std::int32_t)WeaponType::Count; i++) {
					player->AddWeaponUpgrade((WeaponType)i, 0x01);
				}
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatCoins()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			// Coins are synchronized automatically
			_players[0]->AddCoins(5);
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatMorph()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;

			PlayerType newType;
			switch (_players[0]->GetPlayerType()) {
				case PlayerType::Jazz: newType = PlayerType::Spaz; break;
				case PlayerType::Spaz: newType = PlayerType::Lori; break;
				default: newType = PlayerType::Jazz; break;
			}

			if (!_players[0]->MorphTo(newType)) {
				_players[0]->MorphTo(PlayerType::Jazz);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

	bool LevelHandler::CheatShield()
	{
		if (IsCheatingAllowed() && !_players.empty()) {
			_cheatsUsed = true;
			for (auto* player : _players) {
				ShieldType shieldType = (ShieldType)(((std::int32_t)player->GetActiveShield() + 1) % (std::int32_t)ShieldType::Count);
				player->SetShield(shieldType, 40.0f * FrameTimer::FramesPerSecond);
			}
		} else {
			_console->WriteLine(UI::MessageLevel::Error, _("Cheats are not allowed in current context"));
		}
		return true;
	}

#if defined(WITH_IMGUI)
	ImVec2 LevelHandler::WorldPosToScreenSpace(const Vector2f pos)
	{
		auto& mainViewport = _assignedViewports[0];
		
		Rectf bounds = mainViewport->GetBounds();
		Vector2i originalSize = mainViewport->_view->GetSize();
		Vector2f upscaledSize = _upscalePass.GetTargetSize();
		Vector2f halfView = bounds.Center();
		return ImVec2(
			(pos.X - mainViewport->_cameraPos.X + halfView.X) * upscaledSize.X / originalSize.X,
			(pos.Y - mainViewport->_cameraPos.Y + halfView.Y) * upscaledSize.Y / originalSize.Y
		);
	}
#endif

	LevelHandler::PlayerInput::PlayerInput()
		: PressedActions(0), PressedActionsLast(0), Frozen(false)
	{
	}
}