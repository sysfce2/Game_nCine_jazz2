﻿#pragma once

#include "EnemyBase.h"

namespace Jazz2::Actors::Enemies
{
	/** @brief Turtle */
	class Turtle : public EnemyBase
	{
		DEATH_RUNTIME_OBJECT(EnemyBase);

	public:
		Turtle();

		static void Preload(const ActorActivationDetails& details);

	protected:
		Task<bool> OnActivatedAsync(const ActorActivationDetails& details) override;
		void OnUpdate(float timeMult) override;
		void OnUpdateHitbox() override;
		bool OnPerish(ActorBase* collider) override;

	private:
		static constexpr float DefaultSpeed = 1.0f;

		std::uint8_t _theme;
		bool _isAttacking;
		bool _isTurning;
		bool _isWithdrawn;
		bool _isDodging;
		float _dodgeCooldown;

		void HandleTurn(bool isFirstPhase);
		void Attack();
		bool ShouldDodge() const;
	};
}