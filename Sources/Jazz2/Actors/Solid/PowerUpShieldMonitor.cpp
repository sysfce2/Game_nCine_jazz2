﻿#include "PowerUpShieldMonitor.h"
#include "../../ILevelHandler.h"
#include "../../Tiles/TileMap.h"
#include "../Player.h"
#include "../Weapons/ShotBase.h"
#include "../Weapons/TNT.h"

#include "../../../nCine/Base/FrameTimer.h"

namespace Jazz2::Actors::Solid
{
	PowerUpShieldMonitor::PowerUpShieldMonitor()
	{
	}

	void PowerUpShieldMonitor::Preload(const ActorActivationDetails& details)
	{
		ShieldType shieldType = (ShieldType)details.Params[0];
		switch (shieldType) {
			case ShieldType::Fire:
				PreloadMetadataAsync("Object/PowerUp/ShieldFire"_s);
				PreloadMetadataAsync("Weapon/ShieldFire"_s);
				break;
			case ShieldType::Water:
				PreloadMetadataAsync("Object/PowerUp/ShieldWater"_s);
				PreloadMetadataAsync("Weapon/ShieldWater"_s);
				break;
			case ShieldType::Laser:
				PreloadMetadataAsync("Object/PowerUp/ShieldLaser"_s);
				break;
			case ShieldType::Lightning:
				PreloadMetadataAsync("Object/PowerUp/ShieldLightning"_s);
				PreloadMetadataAsync("Weapon/ShieldLightning"_s);
				break;
			default: PreloadMetadataAsync("Object/PowerUp/Empty"_s); break;
		}
	}

	Task<bool> PowerUpShieldMonitor::OnActivatedAsync(const ActorActivationDetails& details)
	{
		_shieldType = (ShieldType)details.Params[0];

		SetState(ActorState::CanBeFrozen, true);
		Movable = true;

		switch (_shieldType) {
			case ShieldType::Fire: async_await RequestMetadataAsync("Object/PowerUp/ShieldFire"_s); break;
			case ShieldType::Water: async_await RequestMetadataAsync("Object/PowerUp/ShieldWater"_s); break;
			case ShieldType::Laser: async_await RequestMetadataAsync("Object/PowerUp/ShieldLaser"_s); break;
			case ShieldType::Lightning: async_await RequestMetadataAsync("Object/PowerUp/ShieldLightning"_s); break;
			default: async_await RequestMetadataAsync("Object/PowerUp/Empty"_s); break;
		}

		SetAnimation(AnimState::Default);

		async_return true;
	}

	void PowerUpShieldMonitor::OnUpdateHitbox()
	{
		SolidObjectBase::OnUpdateHitbox();

		// Mainly to fix the power up in `tube1.j2l`
		AABBInner.L += 2.0f;
		AABBInner.R -= 2.0f;
	}

	bool PowerUpShieldMonitor::OnHandleCollision(std::shared_ptr<ActorBase> other)
	{
		if (_health == 0) {
			return SolidObjectBase::OnHandleCollision(other);
		}

		if (auto* shotBase = runtime_cast<Weapons::ShotBase>(other.get())) {
			Player* owner = shotBase->GetOwner();
			WeaponType weaponType = shotBase->GetWeaponType();
			if (owner != nullptr && shotBase->GetStrength() > 0) {
				DestroyAndApplyToPlayer(owner);
				shotBase->DecreaseHealth(INT32_MAX);
			} else if (weaponType == WeaponType::Freezer) {
				_frozenTimeLeft = 10.0f * FrameTimer::FramesPerSecond;
				shotBase->DecreaseHealth(INT32_MAX);
			}
			return true;
		} else if (auto* tnt = runtime_cast<Weapons::TNT>(other.get())) {
			Player* owner = tnt->GetOwner();
			if (owner != nullptr) {
				DestroyAndApplyToPlayer(owner);
			}
			return true;
		} else if (auto* player = runtime_cast<Player>(other.get())) {
			if (player->CanBreakSolidObjects()) {
				DestroyAndApplyToPlayer(player);
				return true;
			}
		}

		return SolidObjectBase::OnHandleCollision(std::move(other));
	}

	bool PowerUpShieldMonitor::CanCauseDamage(ActorBase* collider)
	{
		return _levelHandler->IsReforged() || runtime_cast<Weapons::TNT>(collider);
	}

	bool PowerUpShieldMonitor::OnPerish(ActorBase* collider)
	{
		CreateParticleDebrisOnPerish(ParticleDebrisEffect::Standard, Vector2f::Zero);

		return SolidObjectBase::OnPerish(collider);
	}

	void PowerUpShieldMonitor::DestroyAndApplyToPlayer(Player* player)
	{
		if (player->SetShield(_shieldType, 30.0f * FrameTimer::FramesPerSecond)) {
			PlaySfx("Break"_s);
			DecreaseHealth(INT32_MAX, player);
		}
	}
}