﻿#include "GemGiant.h"
#include "../../LevelInitialization.h"
#include "../../ILevelHandler.h"
#include "../../Events/EventSpawner.h"
#include "../Player.h"
#include "../Weapons/ShotBase.h"

#include "../../../nCine/Base/Random.h"

namespace Jazz2::Actors::Collectibles
{
	GemGiant::GemGiant()
	{
	}

	void GemGiant::Preload(const ActorActivationDetails& details)
	{
		PreloadMetadataAsync("Object/GemGiant"_s);
		PreloadMetadataAsync("Collectible/Gems"_s);
	}

	Task<bool> GemGiant::OnActivatedAsync(const ActorActivationDetails& details)
	{
		CollisionFlags &= ~CollisionFlags::ApplyGravitation;

		co_await RequestMetadataAsync("Object/GemGiant"_s);

		SetAnimation("GemGiant"_s);

		co_return true;
	}

	bool GemGiant::OnHandleCollision(std::shared_ptr<ActorBase> other)
	{
		if (auto shotBase = dynamic_cast<Weapons::ShotBase*>(other.get())) {
			DecreaseHealth(shotBase->GetStrength(), other.get());
			shotBase->DecreaseHealth(INT32_MAX);
			return true;
		} /*else if (auto shotTnt = dynamic_cast<Weapons::ShotTNT*>(other)) {
			// TODO: TNT
		}*/ else if (auto player = dynamic_cast<Player*>(other.get())) {
			if (player->CanBreakSolidObjects()) {
				DecreaseHealth(INT32_MAX, other.get());
				return true;
			}
		}

		return ActorBase::OnHandleCollision(other);
	}

	bool GemGiant::OnPerish(ActorBase* collider)
	{
		CreateParticleDebris();

		PlaySfx("Break"_s);

		for (int i = 0; i < 10; i++) {
			float fx = Random().NextFloat(-18.0f, 18.0f);
			float fy = Random().NextFloat(-8.0f, 0.2f);
			uint8_t eventParams[1] = { 0 };
			std::shared_ptr<ActorBase> actor = _levelHandler->EventSpawner()->SpawnEvent(EventType::Gem, eventParams, ActorFlags::None, Vector3i((int)(_pos.X + fx * 2.0f), (int)(_pos.Y + fy * 4.0f), _renderer.layer() - 10.0f));
			if (actor != nullptr) {
				actor->AddExternalForce(fx, fy);
				_levelHandler->AddActor(actor);
			}
		}

		return ActorBase::OnPerish(collider);
	}
}