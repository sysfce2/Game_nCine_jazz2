#pragma once

#include "../ActorBase.h"

namespace Jazz2::Actors::Environment
{
	/**
		@brief Ambient bubbles
		
		Decorative emitter that releases streams of rising bubbles, used in underwater
		sections to give submerged areas life. Purely cosmetic---the bubbles drift upward
		and fade away without affecting gameplay, so nothing is emitted when
		@ref PreferencesCache::Particles is set to @ref ParticleQuality::Off.
	*/
	class AmbientBubbles : public ActorBase
	{
		DEATH_RUNTIME_OBJECT(ActorBase);

	public:
		/** @brief Creates a new instance */
		AmbientBubbles();

		/** @brief Preloads all assets required by this actor */
		static void Preload(const ActorActivationDetails& details);

	protected:
		Task<bool> OnActivatedAsync(const ActorActivationDetails& details) override;
		void OnUpdate(float timeMult) override;

	private:
		static constexpr float BaseTime = 20.0f;

		uint8_t _speed;
		float _cooldown;
		std::int32_t _bubblesLeft;
		float _delay;

		void SpawnBubbles(std::int32_t count);
	};
}